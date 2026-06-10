# VoiceBridge Lunara Care — ESP-IDF Firmware

Чистый ESP-IDF проект (no Arduino, no PlatformIO).
Протестировано на ESP-IDF v5.x.

## Структура проекта

```
CMakeLists.txt          — верхний CMake
sdkconfig.defaults      — дефолтные Kconfig опции (PSRAM, Flash, FreeRTOS)
partitions.csv          — таблица разделов (8MB Flash + SPIFFS)
main/
  CMakeLists.txt        — регистрация исходников
  idf_component.yml     — внешний компонент: esp_websocket_client
  config.h              — ВСЕ настройки (Wi-Fi, пины, тайминги)
  main.c                — app_main, Wi-Fi, FreeRTOS задачи
  led.c / led.h         — WS2812B via RMT, все состояния
  gesture.c / gesture.h — capacitive touch FSM + кнопка + браслет
  audio.c / audio.h     — I2S mic + speaker + WAV + тоны
  recorder.c / recorder.h — запись в PSRAM + base64 mbedtls
  ws_client.c / ws_client.h — WebSocket + автореконнект
  protocol.c / protocol.h   — обработка событий + исходящие сообщения
```

## Быстрый старт

### 1. Настрой config.h

```c
#define CFG_WIFI_SSID       "ИМЯ_СЕТИ"
#define CFG_WIFI_PASSWORD   "ПАРОЛЬ"
#define CFG_WS_URI          "ws://192.168.0.9:3000"  // или wss:// для ngrok
#define CFG_DEVICE_NAME     "Папа"

// Пины — откорректируй под свою плату
#define PIN_MIC_SCK   12
#define PIN_MIC_WS    13
#define PIN_MIC_DATA  11
#define PIN_SPK_BCLK   5
#define PIN_SPK_LRCLK  6
#define PIN_SPK_DIN    7
#define PIN_LED        48
#define PIN_TOUCH       4
#define PIN_BUTTON      0
```

### 2. Собери и прошей

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

### 3. Сгенерируй TTS файлы

```bash
pip install gTTS pydub
python tools/gen_phrases.py    # создаёт data/tts/*.wav
idf.py spiffs_image            # или скопируй вручную на SPIFFS
```

Либо залей через `idf.py -p /dev/ttyUSBx erase_otadata`
и потом через `mkspiffs` / `esptool`.

## Жесты

| Жест | Действие |
|---|---|
| 1 касание сферы | wants_to_talk |
| 3 касания (<650 мс) | lunara_start |
| Удержание 5 сек | SOS (с голосовым предупреждением + 3 сек отмена) |
| Физкнопка нажать | начать запись голосового |
| Физкнопка отпустить | остановить и отправить |
| Браслет (PIN_BRACELET≥0) | SOS немедленно |

## Цвета сферы

| Цвет | Состояние |
|---|---|
| Серый (dim) | offline |
| Тёплый белый (дыхание) | idle |
| Синий (пульс 4 сек) | входящее голосовое |
| Зелёный (непрерывный) | Live Line |
| Жёлтый (быстрый) | предупреждение 5 сек до конца |
| Синий 1Hz | запись голосового |
| Тёплый жёлтый 6 сек | remind_later |
| Красный (мигание) | SOS активен |

## Архитектура задач

```
Core 0                      Core 1
────────────────────         ────────────────────────────
main_loop (5 prio)           audio_task (7 prio)
  gesture_update()             if live_active:
  led_update()                   mic_read() → send_chunk()
  check_sos_window()             queue_pop() → spk_write()
                               elif recording:
ws_monitor_task (3 prio)         recorder_feed()
  reconnect backoff
```

## Калибровка touch threshold (CFG_TOUCH_THRESHOLD)

В `gesture.c` временно добавь в `gesture_update()`:

```c
uint32_t val = 0;
touch_pad_read_raw_data(PIN_TOUCH, &val);
ESP_LOGI("CALIB", "touch raw: %u", (unsigned)val);
```

Замерь без касания и с касанием. Выбери порог посередине.

## Известные ограничения MVP

- `audio_url` (HTTP download) не реализован — нужен `esp_http_client`.
- TTS фраза с именем и числом минут статичная (один WAV файл).
- BLE-браслет не реализован — только wired GPIO.
- Эхо-подавление (AEC) не реализовано.
