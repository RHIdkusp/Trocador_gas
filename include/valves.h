#pragma once

#include "app_types.h"
#include "esp_err.h"

esp_err_t valves_init(void);
void valves_close_all(void);
void valves_open_exclusive(botijao_t botijao);
