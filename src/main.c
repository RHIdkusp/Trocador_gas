#include "app_config.h"
#include "app_state_machine.h"
#include "bluetooth_serial.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hx711.h"
#include "inputs.h"
#include "leds.h"
#include "storage_nvs.h"
#include "valves.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Inicializando sistema automatico de troca de botijoes GLP");

    /* A NVS guarda o ultimo botijao ativo para recuperacao apos falta de energia. */
    ESP_ERROR_CHECK(storage_nvs_init());

    botijao_t ultimo_botijao = BOTIJAO_B1;
    esp_err_t load_err = storage_nvs_load_last_botijao(&ultimo_botijao);
    if (load_err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel ler NVS; usando B1 como padrao");
        ultimo_botijao = BOTIJAO_B1;
    }

    ESP_ERROR_CHECK(valves_init());
    ESP_ERROR_CHECK(leds_init());
    valves_close_all();
    leds_all_off();

    /* Bluetooth SPP e opcional para a logica de seguranca.
     * Se ele falhar, o sistema continua funcionando pelo monitor serial USB.
     */
    esp_err_t bt_err = bluetooth_serial_start();
    if (bt_err != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth nao iniciou: %s", esp_err_to_name(bt_err));
    }

    /* Fila central de eventos.
     * Entradas e HX711 apenas publicam eventos; a maquina de estados e a unica
     * responsavel por decidir valvulas, LEDs e persistencia.
     */
    QueueHandle_t event_queue = xQueueCreate(APP_EVENT_QUEUE_LENGTH, sizeof(app_event_t));
    if (event_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar fila de eventos");
        abort();
    }

    /* Tasks criadas:
     * - state_task: consome a fila e executa a matriz de estados/eventos.
     * - inputs_task: faz debounce dos fins de curso e envia modo manual/auto.
     * - hx711_task: le os HX711, filtra pesos e envia pesos atualizados.
     * O Bluetooth cria internamente bt_tx_task para escoar texto via SPP.
     */
    app_state_machine_start(event_queue, ultimo_botijao);
    ESP_ERROR_CHECK(inputs_start(event_queue));
    ESP_ERROR_CHECK(hx711_start(event_queue));

    ESP_LOGI(TAG, "Sistema iniciado");
}
