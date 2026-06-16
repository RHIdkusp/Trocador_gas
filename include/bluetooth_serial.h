#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Inicializa Bluetooth Classic SPP.
 * Quando ativo, o ESP32 aparece no computador como uma porta serial Bluetooth.
 */
esp_err_t bluetooth_serial_start(void);

/* Indica se algum computador/celular abriu a conexao SPP. */
bool bluetooth_serial_is_connected(void);

/* Enfileira texto para envio pela serial Bluetooth.
 * A chamada nao bloqueia esperando o pacote sair pelo radio.
 */
void bluetooth_serial_printf(const char *fmt, ...);
