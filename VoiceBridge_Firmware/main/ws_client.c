#include <stdbool.h>
#include <string.h>
#include "ws_client.h"
#include "config.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "WS";

static esp_websocket_client_handle_t s_client    = NULL;
static bool                          s_connected = false;
static ws_text_cb_t                  s_on_text   = NULL;
static ws_event_cb_t                 s_on_event  = NULL;

/* Reconnect state */
static uint32_t s_reconnect_delay = CFG_WS_RECONNECT_MS;
static uint32_t s_disconnected_ms = 0;
static bool     s_need_reconnect  = false;

/* ── Fragment reassembly buffer ─────────────────────────────────────────────
 * esp_websocket_client ^1.7.x fires WEBSOCKET_EVENT_DATA multiple times for
 * large frames.  Each event carries:
 *   ev->payload_len   — total frame size (same in every fragment event)
 *   ev->payload_offset — byte offset of this chunk inside the full frame
 *   ev->data_ptr / ev->data_len — this chunk's data and size
 *
 * We allocate a heap buffer on the first fragment, copy each chunk in, and
 * dispatch to s_on_text only when the last byte has arrived.
 * Buffer is freed immediately after dispatch.
 *
 * Max JSON we expect: voice_message with base64 audio.
 * 60 s × 16000 Hz × 2 bytes × 4/3 (base64) + ~100 overhead ≈ 2.6 MB.
 * We cap at CFG_WS_MAX_JSON_BYTES to avoid runaway allocations.
 * ──────────────────────────────────────────────────────────────────────────── */
#define CFG_WS_MAX_JSON_BYTES   (3 * 1024 * 1024)   /* 3 MB hard cap */

static char  *s_frag_buf     = NULL;   /* PSRAM heap, NULL when idle */
static size_t s_frag_total   = 0;      /* payload_len of current frame */
static size_t s_frag_written = 0;      /* bytes copied so far          */

/* Release reassembly buffer and reset state */
static void frag_reset(void)
{
    if (s_frag_buf) {
        heap_caps_free(s_frag_buf);
        s_frag_buf = NULL;
    }
    s_frag_total   = 0;
    s_frag_written = 0;
}

static void ws_event_handler(void *arg,
                              esp_event_base_t base,
                              int32_t event_id,
                              void *event_data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {

    case WEBSOCKET_EVENT_CONNECTED:
        s_connected       = true;
        s_need_reconnect  = false;
        s_reconnect_delay = CFG_WS_RECONNECT_MS;   /* reset backoff */
        frag_reset();                               /* discard any stale buffer */
        ESP_LOGI(TAG, "connected");
        if (s_on_event) s_on_event(true);
        /* Send immediate keepalive so Railway proxy sees data right away */
        if (s_client) {
            esp_websocket_client_send_text(s_client,
                "{\"type\":\"keepalive\"}", 20, pdMS_TO_TICKS(10000));
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (s_connected) {
            s_connected       = false;
            s_need_reconnect  = true;
            s_disconnected_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            frag_reset();                           /* discard incomplete frame */
            ESP_LOGI(TAG, "disconnected; retry in %u ms",
                     (unsigned)s_reconnect_delay);
            if (s_on_event) s_on_event(false);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
    {
        /* Only process TEXT frames (op_code 0x01).
         * Binary frames (0x02) are not used in this protocol — ignore them.
         * Continuation frames (0x00) share the op_code of their first fragment,
         * so filtering on 0x01 here is correct for the initial fragment only.
         * For subsequent fragments the library keeps op_code = 0x00; we rely on
         * s_frag_buf != NULL to know we are mid-reassembly. */

        bool is_text_frame       = (ev->op_code == 0x01);
        bool is_continuation     = (ev->op_code == 0x00);
        bool reassembly_active   = (s_frag_buf != NULL);

        /* Drop data that is not part of a text frame */
        if (!is_text_frame && !(is_continuation && reassembly_active)) {
            break;
        }

        if (!s_on_text || ev->data_len <= 0) break;

        /* ── Single-fragment fast path ──────────────────────────────────────
         * payload_len == data_len means the entire frame arrived at once.
         * Skip heap allocation and dispatch directly.                        */
        if ((size_t)ev->data_len == (size_t)ev->payload_len) {
            s_on_text(ev->data_ptr, ev->data_len);
            break;
        }

        /* ── Multi-fragment reassembly ──────────────────────────────────────
         * First fragment: payload_offset == 0, allocate buffer.             */
        if (ev->payload_offset == 0) {
            frag_reset();   /* safety: discard any leftover from previous frame */

            if (ev->payload_len == 0 || ev->payload_len > CFG_WS_MAX_JSON_BYTES) {
                ESP_LOGW(TAG, "frame too large or zero (%u bytes) — dropped",
                         (unsigned)ev->payload_len);
                break;
            }

            /* +1 for null terminator we add after the last fragment */
            s_frag_buf = heap_caps_malloc(ev->payload_len + 1, MALLOC_CAP_SPIRAM);
            if (!s_frag_buf) {
                ESP_LOGE(TAG, "frag malloc failed (%u bytes)", (unsigned)ev->payload_len);
                break;
            }
            s_frag_total   = ev->payload_len;
            s_frag_written = 0;
            ESP_LOGD(TAG, "frag start: total=%u", (unsigned)s_frag_total);
        }

        /* Sanity: buffer must exist and incoming chunk must fit */
        if (!s_frag_buf) {
            ESP_LOGW(TAG, "continuation without buffer — dropped");
            break;
        }
        if (ev->payload_offset + (size_t)ev->data_len > s_frag_total) {
            ESP_LOGW(TAG, "frag overflow — dropped (offset=%u len=%d total=%u)",
                     (unsigned)ev->payload_offset, ev->data_len,
                     (unsigned)s_frag_total);
            frag_reset();
            break;
        }

        /* Copy this chunk into the right position */
        memcpy(s_frag_buf + ev->payload_offset, ev->data_ptr, ev->data_len);
        s_frag_written = ev->payload_offset + ev->data_len;

        ESP_LOGD(TAG, "frag chunk: offset=%u len=%d written=%u/%u",
                 (unsigned)ev->payload_offset, ev->data_len,
                 (unsigned)s_frag_written, (unsigned)s_frag_total);

        /* Last fragment: all bytes received */
        if (s_frag_written >= s_frag_total) {
            s_frag_buf[s_frag_total] = '\0';   /* null-terminate for cJSON */
            ESP_LOGD(TAG, "frag complete: %u bytes", (unsigned)s_frag_total);
            s_on_text(s_frag_buf, (int)s_frag_total);
            frag_reset();
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "transport error");
        frag_reset();   /* discard incomplete frame on error */
        break;

    default:
        break;
    }
}

/* Called from a monitoring task */
static void ws_monitor_task(void *arg) {
    uint32_t s_heartbeat_ms = 0;
    for (;;) {
        /* Send keepalive JSON every 10 sec — Railway proxy needs real data */
        if (s_connected && s_client) {
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now_ms - s_heartbeat_ms >= 15000) {  /* every 15 sec — 1.6.1 fix handles stability */
                s_heartbeat_ms = now_ms;
                ws_send_text("{\"type\":\"keepalive\"}");
            }
        }
                if (s_need_reconnect) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - s_disconnected_ms >= s_reconnect_delay) {
                s_need_reconnect = false;
                /* Exponential backoff */
                s_reconnect_delay *= 2;
                if (s_reconnect_delay > CFG_WS_RECONNECT_MAX_MS)
                    s_reconnect_delay = CFG_WS_RECONNECT_MAX_MS;

                ESP_LOGI(TAG, "reconnecting…");
                /* Destroy and recreate client — stop/start fails after TCP close */
                esp_websocket_client_destroy(s_client);
                s_client    = NULL;
                s_connected = false;

                esp_websocket_client_config_t ws_cfg = {
                    .uri                    = CFG_WS_URI,
                    .disable_auto_reconnect = true,
                    .buffer_size            = 8192,
                    .task_stack             = 12288,
                    .task_prio              = 5,
                    .keep_alive_enable      = true,
                    .keep_alive_idle        = 20,
                    .keep_alive_interval    = 5,
                    .keep_alive_count       = 3,
                    .ping_interval_sec      = 5,
                    .network_timeout_ms     = 15000,
                    .crt_bundle_attach      = esp_crt_bundle_attach,
                };
                s_client = esp_websocket_client_init(&ws_cfg);
                if (s_client) {
                    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, NULL);
                    esp_websocket_client_start(s_client);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void ws_init(ws_text_cb_t on_text, ws_event_cb_t on_event) {
    s_on_text  = on_text;
    s_on_event = on_event;

    esp_websocket_client_config_t ws_cfg = {
        .uri                    = CFG_WS_URI,
        .disable_auto_reconnect = true,
        .task_prio              = 5,
        .keep_alive_enable      = true,
        .keep_alive_idle        = 20,
        .keep_alive_interval    = 5,
        .keep_alive_count       = 3,
        .ping_interval_sec      = 5,
        .crt_bundle_attach      = esp_crt_bundle_attach,
        .buffer_size            = 8192,
        .task_stack             = 12288,
        .network_timeout_ms     = 15000,
    };

    s_client = esp_websocket_client_init(&ws_cfg);
    ESP_ERROR_CHECK(esp_websocket_register_events(
        s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_client));

    xTaskCreate(ws_monitor_task, "ws_mon", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "init OK -> %s", CFG_WS_URI);
}

bool ws_send_text(const char *json) {
    if (!s_connected || !s_client) return false;
    int r = esp_websocket_client_send_text(s_client, json, strlen(json),
                                           pdMS_TO_TICKS(10000));
    return r >= 0;
}

bool ws_send_text_len(const char *json, size_t len) {
    if (!s_connected || !s_client) return false;
    int r = esp_websocket_client_send_text(s_client, json, (int)len,
                                           pdMS_TO_TICKS(10000));
    return r >= 0;
}

bool ws_is_connected(void) { return s_connected; }
