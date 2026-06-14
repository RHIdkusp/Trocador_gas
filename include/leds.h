#pragma once

#include "app_types.h"
#include "esp_err.h"

typedef struct {
    estado_sistema_t estado;
    app_pesos_t pesos;
    app_manual_t manual;
} leds_snapshot_t;

esp_err_t leds_init(void);
void leds_update(const leds_snapshot_t *snapshot);
void leds_all_off(void);
