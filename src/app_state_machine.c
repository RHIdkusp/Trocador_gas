#include "app_state_machine.h"

#include "app_config.h"
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

    if (s_ctx.estado == estado_desejado) {
        update_leds();
        return;
    }

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
    if (modo_manual_ativo()) {
        entrar_modo_manual();
        return;
    }

    if (!pesos_validos()) {
        valves_close_all();
        set_estado(ESTADO_INICIALIZACAO);
        return;
    }

    switch (s_ctx.estado) {
    case ESTADO_INICIALIZACAO:
    case ESTADO_MODO_MANUAL:
        restaurar_ultimo_ou_reserva();
        break;

    case ESTADO_B1_ATIVO:
        if (s_ctx.pesos.b1_possui_gas) {
            ativar_botijao(BOTIJAO_B1);
        } else if (s_ctx.pesos.b2_possui_gas) {
            ESP_LOGI(TAG, "B1 vazio; alternando para B2");
            ativar_botijao(BOTIJAO_B2);
        } else {
            entrar_sem_gas();
        }
        break;

    case ESTADO_B2_ATIVO:
        if (s_ctx.pesos.b2_possui_gas) {
            ativar_botijao(BOTIJAO_B2);
        } else if (s_ctx.pesos.b1_possui_gas) {
            ESP_LOGI(TAG, "B2 vazio; alternando para B1");
            ativar_botijao(BOTIJAO_B1);
        } else {
            entrar_sem_gas();
        }
        break;

    case ESTADO_SEM_GAS:
        recuperar_de_sem_gas();
        break;

    default:
        valves_close_all();
        set_estado(ESTADO_INICIALIZACAO);
        break;
    }
}

static void handle_event(const app_event_t *event)
{
    switch (event->type) {
    case APP_EVENT_PESOS_ATUALIZADOS:
        s_ctx.pesos = event->data.pesos;
        ESP_LOGI(TAG, "Pesos: B1=%.2fkg(%s) B2=%.2fkg(%s)",
                 s_ctx.pesos.peso_b1_kg,
                 s_ctx.pesos.b1_possui_gas ? "gas" : "vazio",
                 s_ctx.pesos.peso_b2_kg,
                 s_ctx.pesos.b2_possui_gas ? "gas" : "vazio");
        avaliar_automatico();
        break;

    case APP_EVENT_MANUAL_ATUALIZADO:
        s_ctx.manual = event->data.manual;
        if (modo_manual_ativo()) {
            entrar_modo_manual();
        } else {
            ESP_LOGI(TAG, "Modo automatico liberado pelos fins de curso");
            avaliar_automatico();
        }
        break;

    case APP_EVENT_REAVALIAR_AUTOMATICO:
        avaliar_automatico();
        break;
    }
}

static void state_task(void *arg)
{
    (void)arg;

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
    s_ctx.queue = event_queue;
    s_ctx.estado = ESTADO_INICIALIZACAO;
    s_ctx.ultimo_botijao_ativo = ultimo_botijao_ativo;
    s_ctx.pesos = (app_pesos_t){0};
    s_ctx.manual = (app_manual_t){0};

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
