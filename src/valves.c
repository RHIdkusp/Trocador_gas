#include "valves.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "valves";

static int inactive_level(void)
{
    return APP_RELAY_ACTIVE_LEVEL ? 0 : 1;
}

static void set_relay(gpio_num_t gpio, bool active)
{
    gpio_set_level(gpio, active ? APP_RELAY_ACTIVE_LEVEL : inactive_level());
}

esp_err_t valves_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << APP_RELAY_B1_GPIO) | (1ULL << APP_RELAY_B2_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    valves_close_all();
    ESP_LOGI(TAG, "Valvulas inicializadas e fechadas");
    return ESP_OK;
}

void valves_close_all(void)
{
    set_relay(APP_RELAY_B1_GPIO, false);
    set_relay(APP_RELAY_B2_GPIO, false);
}

void valves_open_exclusive(botijao_t botijao)
{
    /* Fecha antes de abrir para garantir que nunca existam duas valvulas abertas juntas. */
    valves_close_all();

    if (botijao == BOTIJAO_B1) {
        set_relay(APP_RELAY_B1_GPIO, true);
    } else {
        set_relay(APP_RELAY_B2_GPIO, true);
    }
}
