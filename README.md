# Sistema Embarcado GLP - PlatformIO + ESP-IDF

Projeto para ESP32 Wemos Lolin D32 usando PlatformIO no VSCode, ESP-IDF e FreeRTOS.

## Estrutura

- `src/main.c`: inicializacao geral.
- `src/app_state_machine.c`: maquina de estados principal.
- `src/hx711.c`: leitura dos dois HX711 com media movel.
- `src/inputs.c`: leitura dos fins de curso com debounce.
- `src/valves.c`: controle exclusivo das valvulas.
- `src/leds.c`: painel de LEDs.
- `src/storage_nvs.c`: persistencia do ultimo botijao ativo.
- `include/app_config.h`: pinos, niveis ativos, temporizacoes e calibracao.

## Como usar no VSCode

1. Abra esta pasta no VSCode.
2. Instale/abra a extensao PlatformIO.
3. Aguarde a extensao detectar o `platformio.ini`.
4. Use `PlatformIO: Build`, `PlatformIO: Upload` e `PlatformIO: Monitor`.

## Ajustes obrigatorios antes do teste real

Edite `include/app_config.h`:

- Confirme os GPIOs usados na sua placa.
- Ajuste `APP_RELAY_ACTIVE_LEVEL` se seu modulo rele for ativo em nivel baixo.
- Ajuste `APP_INPUT_ACTIVE_LEVEL` conforme a ligacao dos fins de curso.
- Calibre `APP_HX711_B1_OFFSET`, `APP_HX711_B2_OFFSET`, `APP_HX711_B1_SCALE` e `APP_HX711_B2_SCALE`.
- Ajuste `APP_LIMIAR_VAZIO_KG` com o valor real que define botijao vazio.

## Logica principal

O modo manual tem prioridade absoluta. Quando qualquer fim de curso e acionado, a maquina fecha as duas valvulas, apaga os LEDs verdes e bloqueia a troca automatica.

No modo automatico, o sistema mantem apenas uma valvula aberta por vez. A troca entre B1 e B2 acontece somente quando o botijao ativo fica vazio. A NVS guarda o ultimo botijao ativo para recuperacao apos falta de energia.
