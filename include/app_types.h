#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BOTIJAO_B1 = 0,
    BOTIJAO_B2 = 1,
} botijao_t;

/* Estados formais da maquina principal.
 *
 * ESTADO_SISTEMA_COUNT nao e um estado operacional; ele serve como tamanho
 * das matrizes/tabelas indexadas por estado. Sempre que um estado novo for
 * adicionado, coloque-o antes desse item.
 */
typedef enum {
    ESTADO_INICIALIZACAO = 0,
    ESTADO_MODO_MANUAL,
    ESTADO_B1_ATIVO,
    ESTADO_B2_ATIVO,
    ESTADO_SEM_GAS,
    ESTADO_SISTEMA_COUNT,
} estado_sistema_t;

/* Resultado consolidado da task HX711.
 *
 * As flags *_valido indicam se a media movel ja tem amostras suficientes.
 * As flags *_possui_gas ja aplicam APP_LIMIAR_VAZIO_KG e sao usadas pela
 * maquina de estados para decidir se troca, mantem ou entra em SEM_GAS.
 */
typedef struct {
    float peso_b1_kg;
    float peso_b2_kg;
    bool b1_possui_gas;
    bool b2_possui_gas;
    bool b1_valido;
    bool b2_valido;
} app_pesos_t;

/* Estado dos fins de curso depois do debounce.
 * true significa que aquele botijao esta em modo manual.
 */
typedef struct {
    bool manual_b1;
    bool manual_b2;
} app_manual_t;

/* Eventos que entram na fila principal.
 *
 * APP_EVENT_PESOS_ATUALIZADOS: produzido pela task HX711 periodicamente.
 * APP_EVENT_MANUAL_ATUALIZADO: produzido pela task de entradas apos debounce.
 * APP_EVENT_REAVALIAR_AUTOMATICO: evento interno/reservado para forcar uma
 * nova decisao sem alterar pesos nem entradas.
 *
 * APP_EVENT_COUNT nao e enviado; ele dimensiona a matriz de eventos.
 */
typedef enum {
    APP_EVENT_PESOS_ATUALIZADOS = 0,
    APP_EVENT_MANUAL_ATUALIZADO,
    APP_EVENT_REAVALIAR_AUTOMATICO,
    APP_EVENT_COUNT,
} app_event_type_t;

/* Envelope unico para a fila FreeRTOS.
 *
 * O campo type informa qual membro da union data e valido. Isso evita filas
 * separadas para cada origem e deixa a state_task como ponto central de
 * decisao do sistema.
 */
typedef struct {
    app_event_type_t type;
    union {
        app_pesos_t pesos;
        app_manual_t manual;
    } data;
} app_event_t;
