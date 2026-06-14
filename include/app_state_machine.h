#pragma once

#include "app_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void app_state_machine_start(QueueHandle_t event_queue, botijao_t ultimo_botijao_ativo);
