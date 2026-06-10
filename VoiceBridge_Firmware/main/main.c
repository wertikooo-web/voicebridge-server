#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_system.h"

#include "config.h"
#include "led.h"
#include "gesture.h"
#include "audio.h"
#include "recorder.h"
#include "ws_client.h"
#include "protocol.h"
#include "esp_task_wdt.h"

static const char *TAG = "MAIN";

/* ── Shared volatile state (read by multiple tasks) ──────────────────────── */
volatile bool     g_live_active   = false;
volatile bool     g_sos_active    = false;
volatile uint32_t g_live_start_ms = 0;

static volatile bool     s_sos_armed      = false;
static volatile uint32_t s_sos_armed_ms   = 0;
static volatile bool     s_record_blocked = false;

/* ── Gesture event queue ─────────────────────────────────────────────────── */
static QueueHandle_t s_gesture_q = NULL;

static void on_gesture(gesture_event_t ev) {
    /* Called from iot_button (esp_timer context) — must not block */
    xQueueSend(s_gesture_q, &ev, 0);
}

/* ── Process gesture in main_task (blocking audio calls safe here) ────────── */
static void process_gesture(gesture_event_t ev) {
    switch (ev) {

    case GESTURE_PTT_PRESS:
        if (g_live_active) { s_record_blocked = true; break; }
        s_record_blocked = false;
        if (recorder_is_active()) break;
        if (recorder_start()) {
            led_set(LED_RECORDING);
            audio_play_wav(TTS_ZAPIS);
        }
        break;

    case GESTURE_PTT_RELEASE:
        if (s_record_blocked) { s_record_blocked = false; break; }
        if (!recorder_is_active()) break;
        recorder_stop();
        {
            uint32_t dur = recorder_get_dur_ms();
            if (dur < 500) {
                ESP_LOGI(TAG, "recording too short — discarded");
                recorder_release();
            } else {
                const char *b64 = recorder_get_b64();
                if (b64) {
                    protocol_send_dad_voice(b64, recorder_get_b64_len(), dur);
                    audio_play_wav(TTS_OTPRAVLENO);
                }
                recorder_release();
            }
        }
        led_set(LED_IDLE);
        break;

    case GESTURE_SOS_ARMED:
        if (g_sos_active) break;
        s_sos_armed    = true;
        s_sos_armed_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        led_set(LED_SOS_ARMING);
        audio_play_wav(TTS_VYZYVAJU);
        break;

    case GESTURE_SOS_CANCEL:
        s_sos_armed = false;
        ESP_LOGI(TAG, "SOS cancelled");
        led_set(LED_IDLE);
        break;

    case GESTURE_VOL_UP:
        audio_vol_up();
        break;

    case GESTURE_VOL_DOWN:
        audio_vol_down();
        break;

    case GESTURE_RESET_WIFI:
        ESP_LOGI(TAG, "WiFi reset triggered");
        audio_play_wav(TTS_WIFI_RESET);
        vTaskDelay(pdMS_TO_TICKS(1500));
        nvs_flash_erase();
        esp_restart();
        break;

    case GESTURE_LUNARA:
        ESP_LOGI(TAG, "Lunara called");
        protocol_send_lunara_start();
        audio_play_wav(TTS_SVYAZHETSA);
        led_set(LED_IDLE);
        break;
    }
}

/* ── SOS confirm window (runs in main_task, 100 Hz) ──────────────────────── */
static void check_sos_window(void) {
    if (!s_sos_armed) return;
    __sync_synchronize();
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - s_sos_armed_ms >= CFG_SOS_CONFIRM_MS) {
        s_sos_armed  = false;
        if (!g_sos_active) {   /* send only if not already sent */
            g_sos_active = true;
            protocol_send_help_request("ptt");
            led_set(LED_SOS_ACTIVE);
            ESP_LOGI(TAG, "SOS confirmed and sent");
        }
    }
}

/* ── Wi-Fi ───────────────────────────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_eg;
static int s_wifi_retries = 0;

static void wifi_handler(void *arg, esp_event_base_t base,
                         int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retries < CFG_WIFI_RETRIES) {
            esp_wifi_connect();
            s_wifi_retries++;
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_retries = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void) {
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_handler, NULL, &h2));
    wifi_config_t wc = {
        .sta = {
            .ssid     = CFG_WIFI_SSID,
            .password = CFG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();   /* initiate first connection */
    xEventGroupWaitBits(s_wifi_eg,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
}

/* ── SPIFFS ──────────────────────────────────────────────────────────────── */
static void spiffs_init(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs", .partition_label = NULL,
        .max_files = 8,  .format_if_mount_failed = false,
    };
    if (esp_vfs_spiffs_register(&conf) != ESP_OK)
        ESP_LOGE(TAG, "SPIFFS mount failed — TTS unavailable");
}

/* ── WebSocket callbacks ─────────────────────────────────────────────────── */
static void on_ws_text(const char *data, int len) {
    protocol_handle_incoming(data, len);
}
static void on_ws_event(bool connected) {
    if (connected) { protocol_send_register(); led_set(LED_IDLE); }
    else           { led_set(LED_OFFLINE); }
}

/* ── audio_task  (Core 1, prio 7) ───────────────────────────────────────── */
static void audio_task(void *arg) {
    static int16_t mic_buf[CFG_CHUNK_SAMPLES];
    static uint8_t spk_buf[CFG_CHUNK_BYTES * 2];
    bool warn_sent = false;

    for (;;) {
        if (g_live_active) {
            /* Capture mic → send */
            size_t got = audio_mic_read(mic_buf, CFG_CHUNK_SAMPLES, 50);
            if (got > 0) protocol_send_live_chunk(mic_buf, got);

            /* Play incoming chunks */
            size_t spk_len = 0;
            if (protocol_live_queue_pop(spk_buf, &spk_len) && spk_len > 0)
                audio_spk_write((const int16_t *)spk_buf,
                                spk_len / sizeof(int16_t));

            /* Timeout */
            uint32_t elapsed = xTaskGetTickCount() * portTICK_PERIOD_MS
                               - g_live_start_ms;
            if (elapsed >= CFG_LIVE_TIMEOUT_MS) {
                ESP_LOGI(TAG, "live timeout");
                g_live_active = false;
                warn_sent     = false;
                protocol_send_live_end("timeout");
                audio_stop();
                audio_play_wav(TTS_ZAVERSHENO);
                led_set(LED_IDLE);
                continue;
            }

            /* 5-sec warning */
            if (!warn_sent &&
                (CFG_LIVE_TIMEOUT_MS - elapsed) <=
                (uint32_t)(CFG_LIVE_WARN_SECS * 1000))
            {
                warn_sent = true;
                led_set(LED_LIVE_WARNING);
                audio_play_warning_beep();
                vTaskDelay(pdMS_TO_TICKS(2800));
                if (g_live_active) led_set(LED_LIVE_LINE);
            }

        } else if (recorder_is_active()) {
            recorder_feed();
        } else {
            if (warn_sent) warn_sent = false;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* ── main_task  (Core 0, prio 5) ────────────────────────────────────────── */
static void main_task(void *arg) {
    gesture_event_t ev;
    esp_err_t wdt_add_err = esp_task_wdt_add(NULL);
    if (wdt_add_err != ESP_OK && wdt_add_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Task WDT add failed: %s", esp_err_to_name(wdt_add_err));
    }
    for (;;) {
        esp_task_wdt_reset();   /* feed watchdog — prove task is alive */
        led_update();
        check_sos_window();
        while (xQueueReceive(s_gesture_q, &ev, 0) == pdTRUE)
            process_gesture(ev);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ── app_main ────────────────────────────────────────────────────────────── */
void app_main(void) {
    ESP_LOGI(TAG, "VoiceBridge Lunara Care — boot");

    /* NVS */
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ── Task Watchdog: auto-reboot if any task hangs > 30 sec ── */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 30000,   /* 30 seconds                          */
        .idle_core_mask = 0,       /* don't watch idle tasks              */
        .trigger_panic  = true,    /* reboot on timeout                   */
    };
    /* Try reconfigure first (if WDT already init via sdkconfig),
       fall back to init if not yet initialized                            */
    if (esp_task_wdt_reconfigure(&wdt_cfg) != ESP_OK) {
        esp_task_wdt_init(&wdt_cfg);
    }

    spiffs_init();
    led_init();
    led_set(LED_OFFLINE);

    /* Audio: I2C + I2S + ES8311 — single call */
    if (!audio_init())
        ESP_LOGE(TAG, "Audio init failed — device will have no sound");

    /* Gesture queue must exist before gesture_init() */
    s_gesture_q = xQueueCreate(16, sizeof(gesture_event_t));
    configASSERT(s_gesture_q);
    gesture_init(on_gesture);

    wifi_init();
    protocol_live_queue_init();
    ws_init(on_ws_text, on_ws_event);

    xTaskCreatePinnedToCore(audio_task, "audio",   12288, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(main_task,  "main_loop", 6144, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "all tasks started");
}
