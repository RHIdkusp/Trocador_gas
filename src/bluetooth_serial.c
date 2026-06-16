#include "bluetooth_serial.h"

#include <stdarg.h>
#include <stdio.h>

#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if APP_BLUETOOTH_SERIAL_ENABLED
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#endif

static const char *TAG = "bt_serial";

typedef struct {
    size_t len;
    char text[APP_BLUETOOTH_TX_LINE_MAX];
} bt_tx_msg_t;

/* Fila de saida do Bluetooth.
 * Quem quer transmitir texto apenas enfileira uma linha; a bt_tx_task faz a
 * escrita SPP em segundo plano.
 */
static QueueHandle_t s_tx_queue;

#if APP_BLUETOOTH_SERIAL_ENABLED
/* Handle SPP da conexao atual. So e valido entre ESP_SPP_SRV_OPEN_EVT e
 * ESP_SPP_CLOSE_EVT.
 */
static uint32_t s_spp_handle;
static bool s_connected;
static bool s_congested;

static void bt_tx_task(void *arg)
{
    (void)arg;

    /* Task de transmissao Bluetooth.
     * Ela desacopla as tasks criticas do radio: HX711/state_task nao ficam
     * bloqueadas esperando o envio SPP terminar.
     */
    bt_tx_msg_t msg;
    while (true) {
        if (xQueueReceive(s_tx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!s_connected || s_congested || msg.len == 0) {
            continue;
        }

        esp_err_t err = esp_spp_write(s_spp_handle, (int)msg.len, (uint8_t *)msg.text);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Falha ao enviar via Bluetooth: %s", esp_err_to_name(err));
        }
    }
}

static void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    /* Callback do perfil Serial Port Profile.
     * O ESP32 trabalha como servidor/escravo: o computador pareia e abre uma
     * porta COM Bluetooth para receber as linhas de calibracao.
     */
    switch (event) {
    case ESP_SPP_INIT_EVT: {
        ESP_LOGI(TAG, "SPP inicializado; iniciando servidor");

        /* Deixa o dispositivo visivel e conectavel no pareamento Bluetooth. */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

        esp_spp_start_srv_cfg_t srv_cfg = {
            .local_scn = 0,
            .create_spp_record = true,
            .sec_mask = ESP_SPP_SEC_NONE,
            .role = ESP_SPP_ROLE_SLAVE,
            .name = APP_BLUETOOTH_SERVICE_NAME,
        };
        esp_err_t err = esp_spp_start_srv_with_cfg(&srv_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao iniciar servidor SPP: %s", esp_err_to_name(err));
        }
        break;
    }

    case ESP_SPP_START_EVT:
        ESP_LOGI(TAG, "Bluetooth pronto. Pareie com: %s", APP_BLUETOOTH_DEVICE_NAME);
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        /* Um cliente abriu a porta serial Bluetooth. A partir daqui as chamadas
         * bluetooth_serial_printf passam a ser transmitidas pelo SPP.
         */
        s_spp_handle = param->srv_open.handle;
        s_connected = true;
        s_congested = false;
        ESP_LOGI(TAG, "Cliente Bluetooth conectado");
        bluetooth_serial_printf("Bluetooth conectado ao %s\r\n", APP_BLUETOOTH_DEVICE_NAME);
        break;

    case ESP_SPP_CLOSE_EVT:
        /* Cliente desconectado: preserva o sistema funcionando, apenas descarta
         * novas mensagens Bluetooth ate uma nova conexao abrir.
         */
        s_connected = false;
        s_congested = false;
        s_spp_handle = 0;
        ESP_LOGI(TAG, "Cliente Bluetooth desconectado");
        break;

    case ESP_SPP_CONG_EVT:
        /* Congestionamento indica que o buffer de transmissao do SPP esta cheio.
         * Enquanto isso estiver true, a task descarta mensagens para nao travar
         * leitura de peso nem maquina de estados.
         */
        s_congested = param->cong.cong;
        break;

    default:
        break;
    }
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    (void)param;

    if (event == ESP_BT_GAP_AUTH_CMPL_EVT) {
        ESP_LOGI(TAG, "Pareamento Bluetooth concluido");
    }
}
#endif

esp_err_t bluetooth_serial_start(void)
{
    /* A fila e criada mesmo se o Bluetooth for desabilitado por configuracao.
     * Assim bluetooth_serial_printf pode ser chamado sem precisar de ifdefs
     * espalhados pelo restante do projeto.
     */
    if (s_tx_queue == NULL) {
        s_tx_queue = xQueueCreate(APP_BLUETOOTH_TX_QUEUE_LEN, sizeof(bt_tx_msg_t));
        if (s_tx_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

#if !APP_BLUETOOTH_SERIAL_ENABLED
    ESP_LOGI(TAG, "Bluetooth serial desabilitado por configuracao");
    return ESP_OK;
#else
    /* Cria a task que efetivamente escreve no link SPP. */
    BaseType_t task_ok = xTaskCreate(
        bt_tx_task,
        "bt_tx_task",
        APP_BLUETOOTH_TX_TASK_STACK,
        NULL,
        APP_BLUETOOTH_TX_TASK_PRIO,
        NULL);
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Sequencia padrao ESP-IDF para Bluetooth Classic:
     * controlador -> Bluedroid -> GAP -> SPP.
     */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(APP_BLUETOOTH_DEVICE_NAME));
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_callback));
    ESP_ERROR_CHECK(esp_spp_register_callback(spp_callback));

    /* Modo callback: esp_spp_write envia buffers diretamente pelo handle SPP.
     * Nao usamos VFS porque a fila propria ja resolve o fluxo de saida.
     */
    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };

    err = esp_spp_enhanced_init(&spp_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Inicializacao Bluetooth SPP solicitada");
    }

    return err;
#endif
}

bool bluetooth_serial_is_connected(void)
{
#if APP_BLUETOOTH_SERIAL_ENABLED
    return s_connected;
#else
    return false;
#endif
}

void bluetooth_serial_printf(const char *fmt, ...)
{
    /* Funcao nao bloqueante para o resto do firmware.
     * Se a fila estiver cheia, a linha e descartada; isso e aceitavel para
     * logs de calibracao e evita atrasar leituras/valvulas.
     */
    if (s_tx_queue == NULL) {
        return;
    }

    bt_tx_msg_t msg = {0};

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(msg.text, sizeof(msg.text), fmt, args);
    va_end(args);

    if (written <= 0) {
        return;
    }

    if ((size_t)written >= sizeof(msg.text)) {
        msg.len = sizeof(msg.text) - 1;
        msg.text[msg.len - 2] = '\r';
        msg.text[msg.len - 1] = '\n';
    } else {
        msg.len = (size_t)written;
    }

    (void)xQueueSend(s_tx_queue, &msg, 0);
}
