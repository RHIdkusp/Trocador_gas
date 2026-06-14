#pragma once

#include "app_types.h"
#include "esp_err.h"

esp_err_t storage_nvs_init(void);
esp_err_t storage_nvs_load_last_botijao(botijao_t *botijao);
esp_err_t storage_nvs_save_last_botijao(botijao_t botijao);
