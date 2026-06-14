#include "storage_nvs.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage_nvs";
static const char *NVS_NAMESPACE = "glp_auto";
static const char *KEY_ULTIMO = "ultimo_ativo";

esp_err_t storage_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS inicializada");
    }

    return err;
}

esp_err_t storage_nvs_load_last_botijao(botijao_t *botijao)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *botijao = BOTIJAO_B1;
        ESP_LOGI(TAG, "Namespace NVS ainda nao existe; usando B1");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, KEY_ULTIMO, &value);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *botijao = BOTIJAO_B1;
        ESP_LOGI(TAG, "Ultimo botijao nao gravado; usando B1");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    *botijao = (value == BOTIJAO_B2) ? BOTIJAO_B2 : BOTIJAO_B1;
    ESP_LOGI(TAG, "Ultimo botijao ativo recuperado: B%d", *botijao == BOTIJAO_B1 ? 1 : 2);
    return ESP_OK;
}

esp_err_t storage_nvs_save_last_botijao(botijao_t botijao)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, KEY_ULTIMO, (uint8_t)botijao);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS atualizada: ultimo_botijao_ativo=B%d", botijao == BOTIJAO_B1 ? 1 : 2);
    }

    return err;
}
