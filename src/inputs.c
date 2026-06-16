#include "inputs.h"

#include "app_config.h"
#include "app_types.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "inputs";

typedef struct {
    gpio_num_t gpio;
    bool raw_level;
    bool stable_level;
    TickType_t changed_at;
} debounce_input_t;

/* Fila compartilhada com a maquina de estados.
 * A task de entradas nunca decide valvulas; ela so informa mudancas estaveis.
 */
static QueueHandle_t s_event_queue;

static bool read_manual_gpio(gpio_num_t gpio)
{
    return gpio_get_level(gpio) == APP_INPUT_ACTIVE_LEVEL;
}

static bool debounce_update(debounce_input_t *input, TickType_t now)
{
    const bool raw = read_manual_gpio(input->gpio);

    /* Quando o nivel muda, reinicia a janela de debounce. A mudanca so vira
     * evento se permanecer igual por APP_INPUT_DEBOUNCE_MS.
     */
    if (raw != input->raw_level) {
        input->raw_level = raw;
        input->changed_at = now;
        return false;
    }

    const TickType_t debounce_ticks = pdMS_TO_TICKS(APP_INPUT_DEBOUNCE_MS);
    if (raw != input->stable_level && (now - input->changed_at) >= debounce_ticks) {
        input->stable_level = raw;
        return true;
    }

    return false;
}

static void send_manual_event(bool manual_b1, bool manual_b2)
{
    /* Evento consumido pela state_task. Modo manual tem prioridade absoluta,
     * entao qualquer mudanca nos fins de curso precisa chegar rapidamente.
     */
    app_event_t event = {
        .type = APP_EVENT_MANUAL_ATUALIZADO,
        .data.manual = {
            .manual_b1 = manual_b1,
            .manual_b2 = manual_b2,
        },
    };

    if (xQueueSend(s_event_queue, &event, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "Fila cheia ao enviar evento de modo manual");
    }
}

static void inputs_task(void *arg)
{
    (void)arg;

    /* Task periodica de entradas digitais.
     * - Le os dois fins de curso.
     * - Aplica debounce por software.
     * - Envia evento somente quando o estado estavel muda.
     */
    debounce_input_t b1 = {
        .gpio = APP_INPUT_FIM_CURSO_B1_GPIO,
        .raw_level = read_manual_gpio(APP_INPUT_FIM_CURSO_B1_GPIO),
    };
    debounce_input_t b2 = {
        .gpio = APP_INPUT_FIM_CURSO_B2_GPIO,
        .raw_level = read_manual_gpio(APP_INPUT_FIM_CURSO_B2_GPIO),
    };

    b1.stable_level = b1.raw_level;
    b2.stable_level = b2.raw_level;
    b1.changed_at = xTaskGetTickCount();
    b2.changed_at = b1.changed_at;

    send_manual_event(b1.stable_level, b2.stable_level);

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        const TickType_t now = xTaskGetTickCount();
        const bool changed_b1 = debounce_update(&b1, now);
        const bool changed_b2 = debounce_update(&b2, now);

        if (changed_b1 || changed_b2) {
            ESP_LOGI(TAG, "Fim de curso: B1=%s B2=%s",
                     b1.stable_level ? "manual" : "auto",
                     b2.stable_level ? "manual" : "auto");
            send_manual_event(b1.stable_level, b2.stable_level);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_INPUT_TASK_PERIOD_MS));
    }
}

esp_err_t inputs_start(QueueHandle_t event_queue)
{
    s_event_queue = event_queue;

    /* Fins de curso em modo entrada. O pull interno e escolhido conforme
     * APP_INPUT_ACTIVE_LEVEL para suportar chave para GND ou para VCC.
     */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << APP_INPUT_FIM_CURSO_B1_GPIO) | (1ULL << APP_INPUT_FIM_CURSO_B2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = APP_INPUT_ACTIVE_LEVEL == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = APP_INPUT_ACTIVE_LEVEL == 1 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    /* Cria a task FreeRTOS responsavel pelo debounce dos fins de curso. */
    BaseType_t ok = xTaskCreate(
        inputs_task,
        "inputs_task",
        APP_INPUT_TASK_STACK,
        NULL,
        APP_INPUT_TASK_PRIO,
        NULL);

    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Task de entradas iniciada");
    return ESP_OK;
}
