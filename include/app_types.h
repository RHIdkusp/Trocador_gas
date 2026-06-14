#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BOTIJAO_B1 = 0,
    BOTIJAO_B2 = 1,
} botijao_t;

typedef enum {
    ESTADO_INICIALIZACAO = 0,
    ESTADO_MODO_MANUAL,
    ESTADO_B1_ATIVO,
    ESTADO_B2_ATIVO,
    ESTADO_SEM_GAS,
} estado_sistema_t;

typedef struct {
    float peso_b1_kg;
    float peso_b2_kg;
    bool b1_possui_gas;
    bool b2_possui_gas;
    bool b1_valido;
    bool b2_valido;
} app_pesos_t;

typedef struct {
    bool manual_b1;
    bool manual_b2;
} app_manual_t;

typedef enum {
    APP_EVENT_PESOS_ATUALIZADOS = 0,
    APP_EVENT_MANUAL_ATUALIZADO,
    APP_EVENT_REAVALIAR_AUTOMATICO,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        app_pesos_t pesos;
        app_manual_t manual;
    } data;
} app_event_t;
