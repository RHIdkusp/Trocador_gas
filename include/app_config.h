#pragma once

#include "driver/gpio.h"

/*
 * Ajuste este arquivo conforme a sua montagem real.
 * Os pinos abaixo evitam GPIOs de boot strap sempre que possivel.
 */

/* HX711 - botijao B1 */
#define APP_HX711_B1_DOUT_GPIO GPIO_NUM_34
#define APP_HX711_B1_SCK_GPIO  GPIO_NUM_5

/* HX711 - botijao B2 */
#define APP_HX711_B2_DOUT_GPIO GPIO_NUM_39
#define APP_HX711_B2_SCK_GPIO  GPIO_NUM_14

/* Fins de curso. Padrao: chave fechando para GND com pull-up interno. */
#define APP_INPUT_FIM_CURSO_B1_GPIO GPIO_NUM_25
#define APP_INPUT_FIM_CURSO_B2_GPIO GPIO_NUM_26
#define APP_INPUT_ACTIVE_LEVEL      0

/* Rele acionado abre a valvula Thermoval NF. Ajuste se seu modulo for ativo em nivel baixo. */
#define APP_RELAY_B1_GPIO      GPIO_NUM_18
#define APP_RELAY_B2_GPIO      GPIO_NUM_19
#define APP_RELAY_ACTIVE_LEVEL 1

/* LEDs do painel */
#define APP_LED_AMARELO_B1_GPIO GPIO_NUM_21
#define APP_LED_AMARELO_B2_GPIO GPIO_NUM_22
#define APP_LED_VERDE_B1_GPIO   GPIO_NUM_23
#define APP_LED_VERDE_B2_GPIO   GPIO_NUM_16
#define APP_LED_AZUL_B1_GPIO    GPIO_NUM_17
#define APP_LED_AZUL_B2_GPIO    GPIO_NUM_13
#define APP_LED_VERMELHO_GPIO   GPIO_NUM_27
#define APP_LED_ACTIVE_LEVEL    1

/* Calibracao das celulas de carga.
 * peso_kg = (leitura_bruta - OFFSET) / SCALE
 *
 * Faca a tara e a calibracao com pesos conhecidos antes de usar em campo.
 */
#define APP_HX711_B1_OFFSET -74889
#define APP_HX711_B2_OFFSET -1891839
#define APP_HX711_B1_SCALE  -15800.0f
#define APP_HX711_B2_SCALE  -22400.0f

/* Durante a calibracao, deixe 1 para ver a leitura bruta media no monitor serial.
 * Depois de calibrar, volte para 0 para reduzir logs.
 */
#define APP_CALIBRATION_LOG_RAW 0

/* Use somente em bancada, com as plataformas SEM PESO ao ligar o ESP32.
 * O firmware usa a leitura estabilizada inicial como zero/tara.
 * Em operacao real com botijao em cima, deixe 0.
 */
#define APP_CALIBRATION_AUTO_TARE_ON_STARTUP 0
#define APP_CALIBRATION_TARE_SAMPLES         20

/* Opcional: depois da tara, coloque esse peso conhecido na celula.
 * Se for 0.0f, o firmware nao calcula sugestao de SCALE.
 * Exemplo: para um peso de 10 kg, use 10.0f.
 */
#define APP_CALIBRATION_KNOWN_WEIGHT_KG 0.0f

/* Bluetooth Classic SPP: aparece no computador como uma porta serial Bluetooth.
 * Use para receber as leituras de calibracao sem cabo USB.
 */
#define APP_BLUETOOTH_SERIAL_ENABLED 1
#define APP_BLUETOOTH_DEVICE_NAME    "GLP-Wemos-D32"
#define APP_BLUETOOTH_SERVICE_NAME   "GLP-Calibracao"
#define APP_BLUETOOTH_TX_QUEUE_LEN   16
#define APP_BLUETOOTH_TX_LINE_MAX    160
#define APP_BLUETOOTH_TX_TASK_STACK  4096
#define APP_BLUETOOTH_TX_TASK_PRIO   4

/* Limiar em kg abaixo do qual o botijao e considerado vazio. */
#define APP_LIMIAR_VAZIO_KG 1.0f

/* Temporizacoes FreeRTOS */
#define APP_INPUT_TASK_PERIOD_MS     10
#define APP_INPUT_DEBOUNCE_MS        60
#define APP_WEIGHT_TASK_PERIOD_MS    250
#define APP_HX711_READY_TIMEOUT_MS   80
#define APP_HX711_MOVING_AVG_SAMPLES 32

/* Ao religar o ESP32/HX711, as primeiras amostras podem vir deslocadas.
 * O firmware descarta essas leituras e so publica peso com o filtro cheio.
 */
#define APP_HX711_STARTUP_DISCARD_SAMPLES 16
#define APP_HX711_REQUIRE_FULL_FILTER     1

/* Protecao contra leituras espurias: se uma unica amostra saltar muitos kg,
 * ela e ignorada. Se o novo valor persistir, passa a ser aceito como mudanca real.
 */
#define APP_HX711_OUTLIER_REJECT_ENABLED       1
#define APP_HX711_MAX_SINGLE_SAMPLE_STEP_KG    5.0f
#define APP_HX711_OUTLIER_ACCEPT_AFTER_SAMPLES 6

/* Filas e tasks */
#define APP_EVENT_QUEUE_LENGTH 10
#define APP_STATE_TASK_STACK   4096
#define APP_INPUT_TASK_STACK   3072
#define APP_WEIGHT_TASK_STACK  4096
#define APP_STATE_TASK_PRIO    6
#define APP_INPUT_TASK_PRIO    7
#define APP_WEIGHT_TASK_PRIO   5
