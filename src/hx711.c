#include "hx711.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "app_types.h"
#include "bluetooth_serial.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"

static const char *TAG = "hx711";

#if APP_CALIBRATION_TARE_SAMPLES > APP_HX711_MOVING_AVG_SAMPLES
#error "APP_CALIBRATION_TARE_SAMPLES deve ser menor ou igual a APP_HX711_MOVING_AVG_SAMPLES"
#endif

typedef struct {
    gpio_num_t dout_gpio;
    gpio_num_t sck_gpio;
    int32_t offset;
    float scale;
} hx711_dev_t;

typedef struct {
    int32_t samples[APP_HX711_MOVING_AVG_SAMPLES];
    size_t index;
    size_t count;
    int64_t sum;
} moving_average_t;

/* Protecao simples contra leituras espurias.
 * Se uma amostra salta muitos kg de uma vez, ela e ignorada. Se leituras
 * parecidas persistirem por algumas amostras, o novo patamar e aceito.
 */
typedef struct {
    bool has_last;
    int32_t last_accepted_raw;
    int32_t pending_raw;
    size_t pending_count;
} raw_guard_t;

static QueueHandle_t s_event_queue;

/* O protocolo do HX711 usa pulsos no SCK gerados por software. O mux evita que
 * outra interrupcao/task atrapalhe a sequencia critica de 24 bits.
 */
static portMUX_TYPE s_hx711_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t hx711_gpio_init(const hx711_dev_t *dev)
{
    gpio_config_t dout_conf = {
        .pin_bit_mask = 1ULL << dev->dout_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&dout_conf);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t sck_conf = {
        .pin_bit_mask = 1ULL << dev->sck_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&sck_conf);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(dev->sck_gpio, 0);
    return ESP_OK;
}

static esp_err_t hx711_wait_ready(const hx711_dev_t *dev, uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (gpio_get_level(dev->dout_gpio) != 0) {
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_OK;
}

static esp_err_t hx711_read_raw(const hx711_dev_t *dev, int32_t *raw)
{
    esp_err_t err = hx711_wait_ready(dev, APP_HX711_READY_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t value = 0;

    /* Leitura bit-bang de 24 bits. A ordem dos pulsos segue o datasheet do
     * HX711: DOUT fica pronto em nivel baixo; cada pulso de SCK desloca um bit.
     */
    portENTER_CRITICAL(&s_hx711_mux);
    for (int i = 0; i < 24; ++i) {
        gpio_set_level(dev->sck_gpio, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | (uint32_t)gpio_get_level(dev->dout_gpio);
        gpio_set_level(dev->sck_gpio, 0);
        esp_rom_delay_us(1);
    }

    /* Um pulso extra seleciona ganho 128 no canal A. */
    gpio_set_level(dev->sck_gpio, 1);
    esp_rom_delay_us(1);
    gpio_set_level(dev->sck_gpio, 0);
    esp_rom_delay_us(1);
    portEXIT_CRITICAL(&s_hx711_mux);

    if (value & 0x800000) {
        value |= 0xFF000000;
    }

    *raw = (int32_t)value;
    return ESP_OK;
}

static void moving_average_add(moving_average_t *filter, int32_t sample)
{
    if (filter->count < APP_HX711_MOVING_AVG_SAMPLES) {
        filter->samples[filter->index] = sample;
        filter->sum += sample;
        filter->count++;
    } else {
        filter->sum -= filter->samples[filter->index];
        filter->samples[filter->index] = sample;
        filter->sum += sample;
    }

    filter->index = (filter->index + 1) % APP_HX711_MOVING_AVG_SAMPLES;
}

static void moving_average_reset(moving_average_t *filter)
{
    memset(filter, 0, sizeof(*filter));
}

static bool moving_average_get(const moving_average_t *filter, int32_t *average)
{
    if (filter->count == 0) {
        return false;
    }

    *average = (int32_t)(filter->sum / (int64_t)filter->count);
    return true;
}

static bool moving_average_get_for_output(const moving_average_t *filter, int32_t *average)
{
#if APP_HX711_REQUIRE_FULL_FILTER
    /* Durante a partida, espera preencher a media movel para nao publicar peso
     * baseado em poucas amostras.
     */
    if (filter->count < APP_HX711_MOVING_AVG_SAMPLES) {
        return false;
    }
#endif

    return moving_average_get(filter, average);
}

static float raw_to_kg(const hx711_dev_t *dev, int32_t raw)
{
    if (dev->scale == 0.0f) {
        return 0.0f;
    }

    /* Conversao de calibracao:
     * peso_kg = (media_bruta - offset) / scale
     * Se o peso diminui quando coloca carga, use scale negativo.
     */
    return ((float)(raw - dev->offset)) / dev->scale;
}

static float raw_delta_to_kg(const hx711_dev_t *dev, int32_t raw_a, int32_t raw_b)
{
    if (dev->scale == 0.0f) {
        return 0.0f;
    }

    return fabsf(((float)(raw_a - raw_b)) / dev->scale);
}

static void accept_raw_sample(raw_guard_t *guard, moving_average_t *filter, int32_t raw, bool reset_filter)
{
    if (reset_filter) {
        moving_average_reset(filter);
    }

    moving_average_add(filter, raw);
    guard->last_accepted_raw = raw;
    guard->has_last = true;
    guard->pending_count = 0;
    guard->pending_raw = 0;
}

static bool guarded_moving_average_add(const char *name,
                                       const hx711_dev_t *dev,
                                       raw_guard_t *guard,
                                       moving_average_t *filter,
                                       int32_t raw)
{
#if APP_HX711_OUTLIER_REJECT_ENABLED
    if (!guard->has_last) {
        accept_raw_sample(guard, filter, raw, false);
        return true;
    }

    const float step_kg = raw_delta_to_kg(dev, raw, guard->last_accepted_raw);
    if (step_kg <= APP_HX711_MAX_SINGLE_SAMPLE_STEP_KG) {
        accept_raw_sample(guard, filter, raw, false);
        return true;
    }

    const float pending_step_kg = guard->pending_count == 0
                                      ? 0.0f
                                      : raw_delta_to_kg(dev, raw, guard->pending_raw);
    if (guard->pending_count == 0 || pending_step_kg > APP_HX711_MAX_SINGLE_SAMPLE_STEP_KG) {
        guard->pending_raw = raw;
        guard->pending_count = 1;
        ESP_LOGW(TAG, "%s: salto bruto ignorado: %.2fkg raw=%ld ultimo=%ld",
                 name,
                 step_kg,
                 (long)raw,
                 (long)guard->last_accepted_raw);
        return false;
    }

    guard->pending_count++;
    if (guard->pending_count >= APP_HX711_OUTLIER_ACCEPT_AFTER_SAMPLES) {
        ESP_LOGW(TAG, "%s: salto persistente aceito apos %u amostras: %.2fkg",
                 name,
                 (unsigned)guard->pending_count,
                 step_kg);
        accept_raw_sample(guard, filter, raw, true);
        return true;
    }

    ESP_LOGW(TAG, "%s: salto ainda ignorado: %.2fkg (%u/%u)",
             name,
             step_kg,
             (unsigned)guard->pending_count,
             (unsigned)APP_HX711_OUTLIER_ACCEPT_AFTER_SAMPLES);
    return false;
#else
    (void)name;
    (void)dev;
    accept_raw_sample(guard, filter, raw, false);
    return true;
#endif
}

#if APP_CALIBRATION_AUTO_TARE_ON_STARTUP
static bool moving_average_has_tare_samples(const moving_average_t *filter)
{
    return filter->count >= APP_CALIBRATION_TARE_SAMPLES;
}
#endif

static bool auto_tare_if_needed(hx711_dev_t *b1_dev,
                                hx711_dev_t *b2_dev,
                                const moving_average_t *b1_filter,
                                const moving_average_t *b2_filter,
                                bool *tare_done)
{
#if APP_CALIBRATION_AUTO_TARE_ON_STARTUP
    /* Modo de bancada: com as plataformas vazias ao ligar, usa a media inicial
     * como offset/tara. Em operacao real deve ficar desativado.
     */
    if (*tare_done) {
        return true;
    }

    if (!moving_average_has_tare_samples(b1_filter) || !moving_average_has_tare_samples(b2_filter)) {
        ESP_LOGI(TAG, "Aguardando tara automatica: B1=%u/%u B2=%u/%u amostras",
                 (unsigned)b1_filter->count,
                 (unsigned)APP_CALIBRATION_TARE_SAMPLES,
                 (unsigned)b2_filter->count,
                 (unsigned)APP_CALIBRATION_TARE_SAMPLES);
        return false;
    }

    int32_t avg_b1 = 0;
    int32_t avg_b2 = 0;
    if (!moving_average_get(b1_filter, &avg_b1) || !moving_average_get(b2_filter, &avg_b2)) {
        return false;
    }

    b1_dev->offset = avg_b1;
    b2_dev->offset = avg_b2;
    *tare_done = true;

    ESP_LOGW(TAG, "TARA AUTOMATICA APLICADA: APP_HX711_B1_OFFSET=%ld APP_HX711_B2_OFFSET=%ld",
             (long)b1_dev->offset,
             (long)b2_dev->offset);
    ESP_LOGW(TAG, "Copie esses OFFSETs para app_config.h e depois desative APP_CALIBRATION_AUTO_TARE_ON_STARTUP");

    return true;
#else
    (void)b1_dev;
    (void)b2_dev;
    (void)b1_filter;
    (void)b2_filter;
    (void)tare_done;
    return true;
#endif
}

static void log_calibration_values(const hx711_dev_t *b1_dev,
                                   const hx711_dev_t *b2_dev,
                                   int32_t avg_b1,
                                   int32_t avg_b2,
                                   const app_pesos_t *pesos)
{
#if APP_CALIBRATION_LOG_RAW
    /* Saida de calibracao. A mesma linha vai para o monitor serial USB e para
     * o Bluetooth SPP, permitindo calibrar sem cabo depois de parear.
     */
    char line[APP_BLUETOOTH_TX_LINE_MAX];
    snprintf(line, sizeof(line),
             "RAW media: B1=%ld offset=%ld -> %.2fkg | B2=%ld offset=%ld -> %.2fkg",
             (long)avg_b1,
             (long)b1_dev->offset,
             pesos->peso_b1_kg,
             (long)avg_b2,
             (long)b2_dev->offset,
             pesos->peso_b2_kg);
    ESP_LOGI(TAG, "%s", line);
    bluetooth_serial_printf("%s\r\n", line);

    if (APP_CALIBRATION_KNOWN_WEIGHT_KG > 0.0f) {
        const float scale_b1 = ((float)(avg_b1 - b1_dev->offset)) / APP_CALIBRATION_KNOWN_WEIGHT_KG;
        const float scale_b2 = ((float)(avg_b2 - b2_dev->offset)) / APP_CALIBRATION_KNOWN_WEIGHT_KG;
        snprintf(line, sizeof(line),
                 "SCALE sugerido com %.2fkg: B1=%.2ff B2=%.2ff",
                 APP_CALIBRATION_KNOWN_WEIGHT_KG,
                 scale_b1,
                 scale_b2);
        ESP_LOGI(TAG, "%s", line);
        bluetooth_serial_printf("%s\r\n", line);
    }
#else
    (void)b1_dev;
    (void)b2_dev;
    (void)avg_b1;
    (void)avg_b2;
    (void)pesos;
#endif
}

static void send_weight_event(const hx711_dev_t *b1_dev,
                              const hx711_dev_t *b2_dev,
                              const moving_average_t *b1_filter,
                              const moving_average_t *b2_filter)
{
    /* Converte a media bruta dos filtros em kg e publica um unico evento para
     * a maquina de estados. Este modulo nao decide qual valvula abrir.
     */
    int32_t avg_b1 = 0;
    int32_t avg_b2 = 0;
    const bool b1_valid = moving_average_get_for_output(b1_filter, &avg_b1);
    const bool b2_valid = moving_average_get_for_output(b2_filter, &avg_b2);

    app_pesos_t pesos = {
        .peso_b1_kg = b1_valid ? raw_to_kg(b1_dev, avg_b1) : 0.0f,
        .peso_b2_kg = b2_valid ? raw_to_kg(b2_dev, avg_b2) : 0.0f,
        .b1_valido = b1_valid,
        .b2_valido = b2_valid,
    };

    pesos.b1_possui_gas = pesos.b1_valido && pesos.peso_b1_kg > APP_LIMIAR_VAZIO_KG;
    pesos.b2_possui_gas = pesos.b2_valido && pesos.peso_b2_kg > APP_LIMIAR_VAZIO_KG;

    log_calibration_values(b1_dev, b2_dev, avg_b1, avg_b2, &pesos);

    app_event_t event = {
        .type = APP_EVENT_PESOS_ATUALIZADOS,
        .data.pesos = pesos,
    };

    if (xQueueSend(s_event_queue, &event, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "Fila cheia ao enviar pesos");
    }
}

static void hx711_task(void *arg)
{
    (void)arg;

    /* Task periodica dos sensores de peso.
     * - Le B1 e B2.
     * - Descarta amostras iniciais.
     * - Aplica filtro de media movel e protecao contra saltos.
     * - Gera APP_EVENT_PESOS_ATUALIZADOS para a state_task.
     */
    hx711_dev_t b1_dev = {
        .dout_gpio = APP_HX711_B1_DOUT_GPIO,
        .sck_gpio = APP_HX711_B1_SCK_GPIO,
        .offset = APP_HX711_B1_OFFSET,
        .scale = APP_HX711_B1_SCALE,
    };
    hx711_dev_t b2_dev = {
        .dout_gpio = APP_HX711_B2_DOUT_GPIO,
        .sck_gpio = APP_HX711_B2_SCK_GPIO,
        .offset = APP_HX711_B2_OFFSET,
        .scale = APP_HX711_B2_SCALE,
    };

    ESP_ERROR_CHECK(hx711_gpio_init(&b1_dev));
    ESP_ERROR_CHECK(hx711_gpio_init(&b2_dev));

    moving_average_t b1_filter;
    moving_average_t b2_filter;
    memset(&b1_filter, 0, sizeof(b1_filter));
    memset(&b2_filter, 0, sizeof(b2_filter));

    bool tare_done = false;
    size_t startup_discard_count = 0;
    raw_guard_t b1_guard = {0};
    raw_guard_t b2_guard = {0};

    ESP_LOGI(TAG, "Offsets em uso: B1=%ld B2=%ld",
             (long)b1_dev.offset,
             (long)b2_dev.offset);

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        int32_t raw_b1 = 0;
        int32_t raw_b2 = 0;
        bool b1_ok = false;
        bool b2_ok = false;

        if (hx711_read_raw(&b1_dev, &raw_b1) == ESP_OK) {
            b1_ok = true;
        } else {
            ESP_LOGW(TAG, "Timeout lendo HX711 B1");
        }

        if (hx711_read_raw(&b2_dev, &raw_b2) == ESP_OK) {
            b2_ok = true;
        } else {
            ESP_LOGW(TAG, "Timeout lendo HX711 B2");
        }

        if (b1_ok && b2_ok && startup_discard_count < APP_HX711_STARTUP_DISCARD_SAMPLES) {
            /* O HX711 pode iniciar com leituras deslocadas; essas primeiras
             * amostras nao entram no filtro.
             */
            startup_discard_count++;
            if (startup_discard_count == APP_HX711_STARTUP_DISCARD_SAMPLES) {
                ESP_LOGI(TAG, "Amostras iniciais descartadas; iniciando media movel");
            }
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_WEIGHT_TASK_PERIOD_MS));
            continue;
        }

        if (b1_ok) {
            guarded_moving_average_add("B1", &b1_dev, &b1_guard, &b1_filter, raw_b1);
        }

        if (b2_ok) {
            guarded_moving_average_add("B2", &b2_dev, &b2_guard, &b2_filter, raw_b2);
        }

        if (auto_tare_if_needed(&b1_dev, &b2_dev, &b1_filter, &b2_filter, &tare_done)) {
            send_weight_event(&b1_dev, &b2_dev, &b1_filter, &b2_filter);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_WEIGHT_TASK_PERIOD_MS));
    }
}

esp_err_t hx711_start(QueueHandle_t event_queue)
{
    s_event_queue = event_queue;

    /* Cria a task FreeRTOS que executa as leituras periodicas dos HX711. */
    BaseType_t ok = xTaskCreate(
        hx711_task,
        "hx711_task",
        APP_WEIGHT_TASK_STACK,
        NULL,
        APP_WEIGHT_TASK_PRIO,
        NULL);

    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Task HX711 iniciada");
    return ESP_OK;
}
