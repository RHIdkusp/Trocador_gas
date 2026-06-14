#include "leds.h"

#include "app_config.h"
#include "driver/gpio.h"

static int led_inactive_level(void)
{
    return APP_LED_ACTIVE_LEVEL ? 0 : 1;
}

static void set_led(gpio_num_t gpio, bool on)
{
    gpio_set_level(gpio, on ? APP_LED_ACTIVE_LEVEL : led_inactive_level());
}

esp_err_t leds_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask =
            (1ULL << APP_LED_AMARELO_B1_GPIO) |
            (1ULL << APP_LED_AMARELO_B2_GPIO) |
            (1ULL << APP_LED_VERDE_B1_GPIO) |
            (1ULL << APP_LED_VERDE_B2_GPIO) |
            (1ULL << APP_LED_AZUL_B1_GPIO) |
            (1ULL << APP_LED_AZUL_B2_GPIO) |
            (1ULL << APP_LED_VERMELHO_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    leds_all_off();
    return ESP_OK;
}

void leds_all_off(void)
{
    set_led(APP_LED_AMARELO_B1_GPIO, false);
    set_led(APP_LED_AMARELO_B2_GPIO, false);
    set_led(APP_LED_VERDE_B1_GPIO, false);
    set_led(APP_LED_VERDE_B2_GPIO, false);
    set_led(APP_LED_AZUL_B1_GPIO, false);
    set_led(APP_LED_AZUL_B2_GPIO, false);
    set_led(APP_LED_VERMELHO_GPIO, false);
}

void leds_update(const leds_snapshot_t *snapshot)
{
    const bool sem_gas =
        snapshot->estado == ESTADO_SEM_GAS ||
        ((snapshot->pesos.b1_valido && snapshot->pesos.b2_valido) &&
         !snapshot->pesos.b1_possui_gas && !snapshot->pesos.b2_possui_gas);

    set_led(APP_LED_AZUL_B1_GPIO, snapshot->manual.manual_b1);
    set_led(APP_LED_AZUL_B2_GPIO, snapshot->manual.manual_b2);

    set_led(APP_LED_VERMELHO_GPIO, sem_gas);

    if (sem_gas) {
        set_led(APP_LED_AMARELO_B1_GPIO, false);
        set_led(APP_LED_AMARELO_B2_GPIO, false);
        set_led(APP_LED_VERDE_B1_GPIO, false);
        set_led(APP_LED_VERDE_B2_GPIO, false);
        return;
    }

    set_led(APP_LED_AMARELO_B1_GPIO, snapshot->pesos.b1_valido && snapshot->pesos.b1_possui_gas);
    set_led(APP_LED_AMARELO_B2_GPIO, snapshot->pesos.b2_valido && snapshot->pesos.b2_possui_gas);

    const bool modo_manual = snapshot->manual.manual_b1 || snapshot->manual.manual_b2;
    set_led(APP_LED_VERDE_B1_GPIO, !modo_manual && snapshot->estado == ESTADO_B1_ATIVO);
    set_led(APP_LED_VERDE_B2_GPIO, !modo_manual && snapshot->estado == ESTADO_B2_ATIVO);
}
