#pragma once

/* ═══════════════════════════════════════════════════════════════════════════
   VoiceBridge Lunara Care — Configuration
   Edit ONLY this file. All other modules include it.
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── Wi-Fi ─────────────────────────────────────────────────────────────── */
#define CFG_WIFI_SSID           "YOUR_WIFI_SSID"
#define CFG_WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"
#define CFG_WIFI_RETRIES        10

/* ── WebSocket server ───────────────────────────────────────────────────── */
/* Local:  ws://192.168.0.9:3000                                            */
/* Ngrok:  wss://xxxx.ngrok-free.app:443                                    */
#define CFG_WS_URI              "ws://192.168.0.9:3000"
#define CFG_WS_RECONNECT_MS     3000
#define CFG_WS_RECONNECT_MAX_MS 30000

/* ── Device display name (dad_voice_message.displayName) ───────────────── */
#define CFG_DEVICE_NAME         "Папа"

/* ═══════════════════════════════════════════════════════════════════════════
   PIN ASSIGNMENTS  (from schematic, do not change without updating hardware)
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── ES8311 codec — I2C control bus ────────────────────────────────────── */
#define PIN_I2C_SDA     1
#define PIN_I2C_SCL     2
#define ES8311_ADDR     0x18    /* ES8311 default I2C address               */
#define ES8311_I2C_PORT I2C_NUM_0

/* ── ES8311 codec — I2S audio bus ──────────────────────────────────────── */
#define PIN_I2S_MCLK    11      /* Master clock — important: must be ≥256×Fs */
#define PIN_I2S_BCLK    3       /* Bit clock                                */
#define PIN_I2S_WS      4       /* Word select (LRCLK)                      */
#define PIN_I2S_DOUT    5       /* ESP32 → ES8311  (playback / DAC)         */
#define PIN_I2S_DIN     6       /* ES8311 → ESP32  (recording / ADC)        */

/* ── Capacitive touch — sphere surface ─────────────────────────────────── */
/* PTT / SOS button: GPIO10, active-LOW via iot_button                      */
#define PIN_BUTTON      10      /* PTT recording + long-hold SOS            */
/* Note: no separate capacitive touch sphere in this hardware rev.          */
/* All gestures are on the PTT button: short=PTT, long=SOS.                */

/* ── Volume buttons ────────────────────────────────────────────────────── */
#define PIN_VOL_UP      7       /* Active-LOW, internal pull-up             */
#define PIN_VOL_DOWN    8       /* Active-LOW, internal pull-up             */

/* ── Lunara AI button ──────────────────────────────────────────────────── */
#define PIN_LUNARA      16      /* Triple tap = call Lunara AI              */

/* ── Reset WiFi button ─────────────────────────────────────────────────── */
#define PIN_RESET_WIFI  9       /* Hold 3 sec to erase NVS & reboot        */

/* ── Amplifier mute control ────────────────────────────────────────────── */
#define PIN_AMP_ENABLE  15      /* HIGH = speaker ON, LOW = muted           */

/* ── WS2812B LED (sphere) ──────────────────────────────────────────────── */
#define PIN_LED         48
#define LED_COUNT       1

/* ═══════════════════════════════════════════════════════════════════════════
   AUDIO PARAMETERS
   ═══════════════════════════════════════════════════════════════════════════ */
#define CFG_SAMPLE_RATE         16000
#define CFG_BITS_PER_SAMPLE     16
#define CFG_CHANNELS            1       /* Mono                             */

/* ES8311 gain settings (0–100) */
#define CFG_MIC_GAIN            60      /* ADC gain — tune for mic distance */
#define CFG_SPK_VOLUME          80      /* DAC output volume 0..100         */

/* Live-stream chunk: 40 ms PCM */
#define CFG_CHUNK_SAMPLES       640     /* 16000 × 0.040                    */
#define CFG_CHUNK_BYTES         1280    /* 640 × 2 bytes/sample             */

/* Voice recording limit */
#define CFG_RECORD_MAX_SECS     60
#define CFG_RECORD_MAX_BYTES    (CFG_SAMPLE_RATE * (CFG_BITS_PER_SAMPLE/8) \
                                 * CFG_RECORD_MAX_SECS)

/* ── I2S DMA ────────────────────────────────────────────────────────────── */
#define CFG_DMA_BUF_COUNT       8
#define CFG_DMA_BUF_LEN         512

/* ═══════════════════════════════════════════════════════════════════════════
   GESTURE TIMING
   ═══════════════════════════════════════════════════════════════════════════ */
#define CFG_TOUCH_DEBOUNCE_MS   50
#define CFG_MULTI_TAP_WINDOW_MS 650
#define CFG_SOS_HOLD_MS         5000    /* hold PTT button for SOS          */
#define CFG_SOS_CONFIRM_MS      3000    /* cancel window after SOS armed    */
#define CFG_VOL_REPEAT_MS       300     /* volume auto-repeat interval      */
#define CFG_RESET_WIFI_HOLD_MS  3000    /* hold to reset WiFi credentials   */

/* ── Live line ──────────────────────────────────────────────────────────── */
#define CFG_LIVE_TIMEOUT_MS     (3 * 60 * 1000)
#define CFG_LIVE_WARN_SECS      5

/* ═══════════════════════════════════════════════════════════════════════════
   TTS FILE PATHS  (SPIFFS)
   ═══════════════════════════════════════════════════════════════════════════ */
#define TTS_GOVORYITE   "/spiffs/tts/govoryite.wav"
#define TTS_ZAVERSHENO  "/spiffs/tts/zaversheno.wav"
#define TTS_ZAPIS       "/spiffs/tts/zapis.wav"
#define TTS_OTPRAVLENO  "/spiffs/tts/otpravleno.wav"
#define TTS_VYZYVAJU    "/spiffs/tts/vyzyvaju.wav"
#define TTS_SVYAZHETSA  "/spiffs/tts/svyazhetsa.wav"
#define TTS_WIFI_RESET  "/spiffs/tts/wifi_reset.wav"   /* "Настройки WiFi сброшены." */
