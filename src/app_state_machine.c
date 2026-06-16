#include "app_state_machine.h"

#include <stdio.h>

#include "app_config.h"
#include "bluetooth_serial.h"
#include "esp_log.h"
#include "leds.h"
#include "storage_nvs.h"
#include "valves.h"

static const char *TAG = "state";

typedef struct {
    QueueHandle_t queue;
    estado_sistema_t estado;
    botijao_t ultimo_botijao_ativo;
    app_pesos_t pesos;
    app_manual_t manual;
} state_ctx_t;

/* Contexto unico da maquina de estados.
 * Como somente state_task altera esse contexto, nao precisamos de mutex aqui.
 */
static state_ctx_t s_ctx;

static const char *estado_to_str(estado_sistema_t estado)
{
    switch (estado) {
    case ESTADO_INICIALIZACAO:
        return "INICIALIZACAO";
    case ESTADO_MODO_MANUAL:
        return "MODO_MANUAL";
    case ESTADO_B1_ATIVO:
        return "B1_ATIVO";
    case ESTADO_B2_ATIVO:
        return "B2_ATIVO";
    case ESTADO_SEM_GAS:
        return "SEM_GAS";
    default:
        return "DESCONHECIDO";
    }
}

static bool modo_manual_ativo(void)
{
    return s_ctx.manual.manual_b1 || s_ctx.manual.manual_b2;
}

static bool pesos_validos(void)
{
    return s_ctx.pesos.b1_valido && s_ctx.pesos.b2_valido;
}

static void update_leds(void)
{
    leds_snapshot_t snapshot = {
        .estado = s_ctx.estado,
        .pesos = s_ctx.pesos,
        .manual = s_ctx.manual,
    };
    leds_update(&snapshot);
}

static void set_estado(estado_sistema_t novo_estado)
{
    if (s_ctx.estado == novo_estado) {
        /* Mesmo sem trocar de estado, os LEDs podem precisar refletir novos
         * pesos ou novas entradas manuais.
         */
        update_leds();
        return;
    }

    ESP_LOGI(TAG, "Estado: %s -> %s", estado_to_str(s_ctx.estado), estado_to_str(novo_estado));
    s_ctx.estado = novo_estado;
    update_leds();
}

static void salvar_ultimo(botijao_t botijao)
{
    if (s_ctx.ultimo_botijao_ativo == botijao) {
        return;
    }

    s_ctx.ultimo_botijao_ativo = botijao;
    esp_err_t err = storage_nvs_save_last_botijao(botijao);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao salvar ultimo botijao ativo na NVS: %s", esp_err_to_name(err));
    }
}

static void ativar_botijao(botijao_t botijao)
{
    const estado_sistema_t estado_desejado =
        botijao == BOTIJAO_B1 ? ESTADO_B1_ATIVO : ESTADO_B2_ATIVO;

    /* Evita reacionar o rele a cada leitura de peso quando o botijao ativo
     * continua sendo o mesmo.
     */
    if (s_ctx.estado == estado_desejado) {
        update_leds();
        return;
    }

    /* valves_open_exclusive fecha tudo antes de abrir a valvula desejada.
     * Isso preserva a regra de nunca manter as duas valvulas abertas juntas.
     */
    valves_open_exclusive(botijao);
    salvar_ultimo(botijao);
    set_estado(estado_desejado);

    ESP_LOGI(TAG, "Botijao ativo: B%d", botijao == BOTIJAO_B1 ? 1 : 2);
}

static void entrar_sem_gas(void)
{
    valves_close_all();
    set_estado(ESTADO_SEM_GAS);
    ESP_LOGW(TAG, "Ambos os botijoes estao vazios");
}

static void entrar_modo_manual(void)
{
    valves_close_all();
    set_estado(ESTADO_MODO_MANUAL);
    ESP_LOGI(TAG, "Modo manual ativo: B1=%s B2=%s",
             s_ctx.manual.manual_b1 ? "acionado" : "normal",
             s_ctx.manual.manual_b2 ? "acionado" : "normal");
}

static void restaurar_ultimo_ou_reserva(void)
{
    /* Usado na inicializacao e no retorno do modo manual.
     * Tenta respeitar o ultimo botijao salvo na NVS; se ele estiver vazio,
     * usa o outro, desde que tenha gas.
     */
    if (!pesos_validos()) {
        valves_close_all();
        set_estado(ESTADO_INICIALIZACAO);
        ESP_LOGW(TAG, "Aguardando leituras validas dos dois HX711");
        return;
    }

    if (!s_ctx.pesos.b1_possui_gas && !s_ctx.pesos.b2_possui_gas) {
        entrar_sem_gas();
        return;
    }

    if (s_ctx.ultimo_botijao_ativo == BOTIJAO_B2) {
        if (s_ctx.pesos.b2_possui_gas) {
            ativar_botijao(BOTIJAO_B2);
        } else {
            ativar_botijao(BOTIJAO_B1);
        }
        return;
    }

    if (s_ctx.pesos.b1_possui_gas) {
        ativar_botijao(BOTIJAO_B1);
    } else {
        ativar_botijao(BOTIJAO_B2);
    }
}

static void recuperar_de_sem_gas(void)
{
    if (!pesos_validos()) {
        valves_close_all();
        update_leds();
        return;
    }

    if (!s_ctx.pesos.b1_possui_gas && !s_ctx.pesos.b2_possui_gas) {
        entrar_sem_gas();
        return;
    }

    /* A prioridade do B1 so vale na recuperacao do estado SEM_GAS. */
    if (s_ctx.pesos.b1_possui_gas) {
        ativar_botijao(BOTIJAO_B1);
    } else {
        ativar_botijao(BOTIJAO_B2);
    }
}

static void avaliar_automatico(void)
{
    /* Guarda global da logica automatica.
     * Antes de olhar qualquer matriz, modo manual sempre fecha valvulas e
     * impede troca automatica.
     */
    if (modo_manual_ativo()) {
        entrar_modo_manual();
        return;
    }

    if (!pesos_validos()) {
        valves_close_all();
        set_estado(ESTADO_INICIALIZACAO);
        return;
    }

    if (s_ctx.estado >= ESTADO_SISTEMA_COUNT) {
        valves_close_all();
        set_estado(ESTADO_INICIALIZACAO);
        return;
    }

    typedef void (*state_action_fn_t)(void);

    /* Matriz/tabela de acoes por estado para a avaliacao automatica.
     *
     * Estados INICIALIZACAO e MODO_MANUAL usam a mesma acao de restauracao:
     * ler pesos atuais, respeitar NVS e escolher o botijao valido.
     *
     * Estado SEM_GAS usa uma regra propria: se voltar gas nos dois, a
     * prioridade de recuperacao e B1, conforme a especificacao.
     *
     * Estados B1_ATIVO e B2_ATIVO ficam como NULL porque usam a regra comum
     * abaixo: manter o ativo se tem gas, trocar para a reserva se o ativo
     * acabou, ou entrar em SEM_GAS se ambos acabaram.
     */
    static const state_action_fn_t state_matrix[ESTADO_SISTEMA_COUNT] = {
        [ESTADO_INICIALIZACAO] = restaurar_ultimo_ou_reserva,
        [ESTADO_MODO_MANUAL] = restaurar_ultimo_ou_reserva,
        [ESTADO_B1_ATIVO] = NULL,
        [ESTADO_B2_ATIVO] = NULL,
        [ESTADO_SEM_GAS] = recuperar_de_sem_gas,
    };

    /* Complemento da matriz: mapeia cada estado ativo para o botijao que ele
     * representa. A regra comum usa esse mapa para calcular ativo/reserva.
     */
    static const botijao_t active_by_state[ESTADO_SISTEMA_COUNT] = {
        [ESTADO_B1_ATIVO] = BOTIJAO_B1,
        [ESTADO_B2_ATIVO] = BOTIJAO_B2,
    };

    state_action_fn_t action = state_matrix[s_ctx.estado];
    if (action != NULL) {
        action();
        return;
    }

    const botijao_t active = active_by_state[s_ctx.estado];
    const botijao_t reserve = active == BOTIJAO_B1 ? BOTIJAO_B2 : BOTIJAO_B1;
    const bool active_has_gas = active == BOTIJAO_B1 ? s_ctx.pesos.b1_possui_gas : s_ctx.pesos.b2_possui_gas;
    const bool reserve_has_gas = reserve == BOTIJAO_B1 ? s_ctx.pesos.b1_possui_gas : s_ctx.pesos.b2_possui_gas;

    if (active_has_gas) {
        ativar_botijao(active);
    } else if (reserve_has_gas) {
        ESP_LOGI(TAG, "B%d vazio; alternando para B%d",
                 active == BOTIJAO_B1 ? 1 : 2,
                 reserve == BOTIJAO_B1 ? 1 : 2);
        ativar_botijao(reserve);
    } else {
        entrar_sem_gas();
    }
}

static void handle_pesos_event(const app_event_t *event)
{
    /* Evento produzido pela hx711_task.
     * Atualiza o snapshot de pesos, espelha a leitura no USB/Bluetooth e
     * entao pede uma nova decisao automatica.
     */
    s_ctx.pesos = event->data.pesos;

    char line[APP_BLUETOOTH_TX_LINE_MAX];
    snprintf(line, sizeof(line),
             "Pesos: B1=%.2fkg(%s) B2=%.2fkg(%s)",
             s_ctx.pesos.peso_b1_kg,
             s_ctx.pesos.b1_possui_gas ? "gas" : "vazio",
             s_ctx.pesos.peso_b2_kg,
             s_ctx.pesos.b2_possui_gas ? "gas" : "vazio");
    ESP_LOGI(TAG, "%s", line);
    bluetooth_serial_printf("%s\r\n", line);

    avaliar_automatico();
}

static void handle_manual_event(const app_event_t *event)
{
    /* Evento produzido pela inputs_task.
     * Se qualquer fim de curso ficou ativo, entra imediatamente em manual.
     * Quando ambos voltam ao normal, reavalia o automatico com os pesos atuais.
     */
    s_ctx.manual = event->data.manual;
    if (modo_manual_ativo()) {
        entrar_modo_manual();
    } else {
        ESP_LOGI(TAG, "Modo automatico liberado pelos fins de curso");
        avaliar_automatico();
    }
}

static void handle_reavaliar_event(const app_event_t *event)
{
    /* Evento interno para forcar uma nova avaliacao sem alterar dados.
     * Hoje e reservado para expansoes, mas ja fica na matriz.
     */
    (void)event;
    avaliar_automatico();
}

static void handle_event(const app_event_t *event)
{
    if (event->type >= APP_EVENT_COUNT || s_ctx.estado >= ESTADO_SISTEMA_COUNT) {
        ESP_LOGW(TAG, "Evento/estado invalido: event=%d estado=%d", event->type, s_ctx.estado);
        return;
    }

    typedef void (*event_action_fn_t)(const app_event_t *event);

    /* Matriz de eventos por estado.
     *
     * Linhas: estado atual da maquina.
     * Colunas: tipo de evento recebido pela fila FreeRTOS.
     * Celula: funcao handler que deve tratar aquele evento naquele estado.
     *
     * Neste projeto os tres eventos sao validos em todos os estados, mas a
     * matriz deixa essa politica explicita. Se no futuro algum evento nao
     * fizer sentido em um estado, basta deixar a celula como NULL.
     */
    static const event_action_fn_t event_matrix[ESTADO_SISTEMA_COUNT][APP_EVENT_COUNT] = {
        [ESTADO_INICIALIZACAO] = {
            [APP_EVENT_PESOS_ATUALIZADOS] = handle_pesos_event,
            [APP_EVENT_MANUAL_ATUALIZADO] = handle_manual_event,
            [APP_EVENT_REAVALIAR_AUTOMATICO] = handle_reavaliar_event,
        },
        [ESTADO_MODO_MANUAL] = {
            [APP_EVENT_PESOS_ATUALIZADOS] = handle_pesos_event,
            [APP_EVENT_MANUAL_ATUALIZADO] = handle_manual_event,
            [APP_EVENT_REAVALIAR_AUTOMATICO] = handle_reavaliar_event,
        },
        [ESTADO_B1_ATIVO] = {
            [APP_EVENT_PESOS_ATUALIZADOS] = handle_pesos_event,
            [APP_EVENT_MANUAL_ATUALIZADO] = handle_manual_event,
            [APP_EVENT_REAVALIAR_AUTOMATICO] = handle_reavaliar_event,
        },
        [ESTADO_B2_ATIVO] = {
            [APP_EVENT_PESOS_ATUALIZADOS] = handle_pesos_event,
            [APP_EVENT_MANUAL_ATUALIZADO] = handle_manual_event,
            [APP_EVENT_REAVALIAR_AUTOMATICO] = handle_reavaliar_event,
        },
        [ESTADO_SEM_GAS] = {
            [APP_EVENT_PESOS_ATUALIZADOS] = handle_pesos_event,
            [APP_EVENT_MANUAL_ATUALIZADO] = handle_manual_event,
            [APP_EVENT_REAVALIAR_AUTOMATICO] = handle_reavaliar_event,
        },
    };

    event_action_fn_t action = event_matrix[s_ctx.estado][event->type];
    if (action != NULL) {
        action(event);
    }
}

static void state_task(void *arg)
{
    (void)arg;

    /* Task principal da aplicacao.
     * Ela e a unica task que consome eventos e muda o estado do sistema.
     * Isso evita disputa entre HX711, entradas digitais e controle de valvulas.
     */
    valves_close_all();
    set_estado(ESTADO_INICIALIZACAO);

    while (true) {
        app_event_t event;
        if (xQueueReceive(s_ctx.queue, &event, portMAX_DELAY) == pdTRUE) {
            handle_event(&event);
        }
    }
}

void app_state_machine_start(QueueHandle_t event_queue, botijao_t ultimo_botijao_ativo)
{
    /* Inicializa o contexto com o ultimo botijao salvo na NVS. A decisao real
     * de abrir B1/B2 so ocorre depois que houver pesos validos e fins de curso.
     */
    s_ctx.queue = event_queue;
    s_ctx.estado = ESTADO_INICIALIZACAO;
    s_ctx.ultimo_botijao_ativo = ultimo_botijao_ativo;
    s_ctx.pesos = (app_pesos_t){0};
    s_ctx.manual = (app_manual_t){0};

    /* Cria a task FreeRTOS da maquina de estados. */
    BaseType_t ok = xTaskCreate(
        state_task,
        "state_task",
        APP_STATE_TASK_STACK,
        NULL,
        APP_STATE_TASK_PRIO,
        NULL);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task da maquina de estados");
        abort();
    }

    ESP_LOGI(TAG, "Maquina de estados iniciada com ultimo botijao B%d",
             ultimo_botijao_ativo == BOTIJAO_B1 ? 1 : 2);
}
