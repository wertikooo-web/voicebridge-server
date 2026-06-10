#include <stdbool.h>
#include "protocol.h"
#include "config.h"
#include "ws_client.h"
#include "led.h"
#include "audio.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "PROTO";

/* Shared state — set in main, read here */
extern volatile bool     g_live_active;
extern volatile bool     g_sos_active;
extern volatile uint32_t g_live_start_ms;

/* ── Live audio queue ────────────────────────────────────────────────────── */
#define LQ_SLOTS  8
#define LQ_BYTES  (CFG_CHUNK_BYTES * 2)

static uint8_t           s_lq_data[LQ_SLOTS][LQ_BYTES];
static size_t            s_lq_len [LQ_SLOTS];
static volatile size_t   s_lq_head = 0;
static volatile size_t   s_lq_tail = 0;
static SemaphoreHandle_t s_lq_mtx  = NULL;

void protocol_live_queue_init(void) {
    s_lq_mtx = xSemaphoreCreateMutex();
}

bool protocol_live_queue_push(const uint8_t *pcm, size_t len) {
    if (!s_lq_mtx || len > LQ_BYTES) return false;
    if (xSemaphoreTake(s_lq_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    size_t next = (s_lq_head + 1) % LQ_SLOTS;
    bool full = (next == s_lq_tail);
    if (!full) {
        memcpy(s_lq_data[s_lq_head], pcm, len);
        s_lq_len[s_lq_head] = len;
        s_lq_head = next;
    }
    xSemaphoreGive(s_lq_mtx);
    return !full;
}

bool protocol_live_queue_pop(uint8_t *out, size_t *out_len) {
    if (!s_lq_mtx || s_lq_head == s_lq_tail) return false;
    if (xSemaphoreTake(s_lq_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    bool ok = false;
    if (s_lq_head != s_lq_tail) {
        *out_len = s_lq_len[s_lq_tail];
        memcpy(out, s_lq_data[s_lq_tail], *out_len);
        s_lq_tail = (s_lq_tail + 1) % LQ_SLOTS;
        ok = true;
    }
    xSemaphoreGive(s_lq_mtx);
    return ok;
}

/* ── Decode base64 audio field → raw PCM → play or queue ─────────────────── */
static void play_b64_audio(const char *b64, bool queue_for_live) {
    if (!b64 || !*b64) return;
    size_t raw_max = (strlen(b64) * 3 / 4) + 8;
    uint8_t *raw = heap_caps_malloc(raw_max, MALLOC_CAP_SPIRAM);
    if (!raw) { ESP_LOGE(TAG, "malloc failed for audio decode"); return; }
    size_t raw_len = 0;
    int r = mbedtls_base64_decode(raw, raw_max, &raw_len,
                                  (const unsigned char *)b64, strlen(b64));
    if (r == 0 && raw_len > 0) {
        if (queue_for_live) {
            protocol_live_queue_push(raw, raw_len);
        } else {
            audio_spk_write((const int16_t *)raw, raw_len / sizeof(int16_t));
        }
    }
    heap_caps_free(raw);
}

/* ── Event handlers ──────────────────────────────────────────────────────── */
static void on_audio_message(cJSON *doc) {
    audio_stop();
    led_set(LED_AUDIO_INCOMING);
    audio_play_sent_chime();
    vTaskDelay(pdMS_TO_TICKS(300));
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(doc, "audio"));
    play_b64_audio(b64, false);
}

static void on_live_line_start(void) {
    if (g_sos_active) { ESP_LOGI(TAG, "live ignored — SOS active"); return; }
    ESP_LOGI(TAG, "live_line_start");
    led_set(LED_LIVE_LINE);
    /* FIX: play alert and TTS BEFORE enabling g_live_active.
       audio_task checks g_live_active to open mic; keeping it false here
       means speaker is free — no I2S write race between tasks.              */
    audio_play_live_alert();
    vTaskDelay(pdMS_TO_TICKS(200));
    audio_play_wav(TTS_GOVORYITE);
    /* Now enable: audio_task will start mic capture on next iteration       */
    g_live_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    g_live_active   = true;
}

static void on_live_line_ended(cJSON *doc) {
    if (!g_live_active) return;
    g_live_active = false;
    const char *r = cJSON_GetStringValue(cJSON_GetObjectItem(doc, "reason"));
    ESP_LOGI(TAG, "live_line_ended reason=%s", r ? r : "?");
    audio_stop();
    audio_play_wav(TTS_ZAVERSHENO);
    led_set(LED_IDLE);
}

static void on_live_audio_chunk(cJSON *doc) {
    if (!g_live_active) return;
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(doc, "audio"));
    play_b64_audio(b64, true);   /* push to live queue */
}

static void on_live_line_warning(void) {
    led_set(LED_LIVE_WARNING);
    audio_play_warning_beep();
    vTaskDelay(pdMS_TO_TICKS(2500));
    if (g_live_active) led_set(LED_LIVE_LINE);
}

static void on_remind_later_ack(cJSON *doc) {
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(doc, "senderName"));
    int mins = 0;
    cJSON *m = cJSON_GetObjectItem(doc, "minutes");
    if (m) mins = (int)m->valuedouble;
    ESP_LOGI(TAG, "remind_later: %s %d min", name ? name : "?", mins);
    audio_play_wav(TTS_SVYAZHETSA);
    led_set(LED_REMIND_LATER);
}

static void on_sos_acknowledged(void) {
    ESP_LOGI(TAG, "SOS acknowledged by family");
    g_sos_active = false;
    /* Play confirmation sound so elderly person knows help is coming */
    audio_play_sent_chime();
    led_set(LED_IDLE);
}

static void on_ping(cJSON *doc) {
    const char *pid = cJSON_GetStringValue(cJSON_GetObjectItem(doc, "ping_id"));
    protocol_send_pong(pid ? pid : "");
}

/* ── Main dispatcher ─────────────────────────────────────────────────────── */
void protocol_handle_incoming(const char *json, int len) {
    /* Guard: ignore empty, too short, or non-JSON messages */
    if (!json || len <= 0) return;
    if (json[0] != '{') {
        ESP_LOGD(TAG, "non-JSON frame ignored (len=%d, first=0x%02x)",
                 len, (unsigned char)json[0]);
        return;
    }
    cJSON *doc = cJSON_ParseWithLength(json, (size_t)len);
    if (!doc) { ESP_LOGW(TAG, "JSON parse error (len=%d)", len); return; }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(doc, "type"));
    if (!type) { cJSON_Delete(doc); return; }
    ESP_LOGD(TAG, "in: %s", type);

    if      (!strcmp(type, "audio_message"))     on_audio_message(doc);
    else if (!strcmp(type, "reply_message"))     on_audio_message(doc);
    else if (!strcmp(type, "voice_message"))     on_audio_message(doc);
    else if (!strcmp(type, "phrase_message"))    {
        audio_play_sent_chime();
        ESP_LOGI(TAG, "phrase: %s",
                 cJSON_GetStringValue(cJSON_GetObjectItem(doc, "text")));
    }
    else if (!strcmp(type, "live_line_start"))   on_live_line_start();
    else if (!strcmp(type, "live_line_ended"))   on_live_line_ended(doc);
    else if (!strcmp(type, "live_audio_chunk"))  on_live_audio_chunk(doc);
    else if (!strcmp(type, "live_line_warning")) on_live_line_warning();
    else if (!strcmp(type, "remind_later_ack"))  on_remind_later_ack(doc);
    else if (!strcmp(type, "sos_acknowledged"))  on_sos_acknowledged();
    else if (!strcmp(type, "direct_line_request")) audio_play_sent_chime();
    else if (!strcmp(type, "ping"))              on_ping(doc);
    else if (!strcmp(type, "connection_status")) { /* ignore */ }
    else ESP_LOGD(TAG, "unknown type: %s", type);

    cJSON_Delete(doc);
}

/* ── Outgoing message builders ───────────────────────────────────────────── */
void protocol_send_register(void) {
    ws_send_text("{\"type\":\"register\",\"role\":\"receiver\"}");
}

void protocol_send_wants_to_talk(void) {
    ws_send_text("{\"type\":\"wants_to_talk\"}");
    ESP_LOGI(TAG, "→ wants_to_talk");
}

void protocol_send_lunara_start(void) {
    ws_send_text("{\"type\":\"lunara_start\"}");
    ESP_LOGI(TAG, "→ lunara_start");
}

void protocol_send_help_request(const char *source) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"help_request\",\"source\":\"%s\"}", source);
    ws_send_text(buf);
    ESP_LOGI(TAG, "→ help_request source=%s", source);
}

void protocol_send_dad_voice(const char *b64, size_t b64len, uint32_t dur_ms) {
    /* Build:  {"type":"dad_voice_message","mimeType":"audio/wav",
                "duration":N,"displayName":"Папа","audio":"<b64>"} */
    const char *pfx = "{\"type\":\"dad_voice_message\","
                       "\"mimeType\":\"audio/wav\","
                       "\"displayName\":\"" CFG_DEVICE_NAME "\","
                       "\"duration\":";
    /* prefix + duration + sep + b64 + suffix */
    char dur_str[16];
    snprintf(dur_str, sizeof(dur_str), "%u", (unsigned)dur_ms);
    const char *mid = ",\"audio\":\"";
    const char *sfx = "\"}";
    size_t total = strlen(pfx) + strlen(dur_str) + strlen(mid)
                 + b64len + strlen(sfx) + 2;

    char *msg = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!msg) { ESP_LOGE(TAG, "dad_voice malloc fail"); return; }
    char *p = msg;
    p += sprintf(p, "%s%s%s", pfx, dur_str, mid);
    memcpy(p, b64, b64len); p += b64len;
    strcpy(p, sfx);
    ws_send_text_len(msg, total - 1);
    heap_caps_free(msg);
    ESP_LOGI(TAG, "→ dad_voice_message dur=%u ms", (unsigned)dur_ms);
}

void protocol_send_live_chunk(const int16_t *pcm, size_t samples) {
    size_t raw_bytes = samples * sizeof(int16_t);
    /* Base64: 4/3 * raw + padding */
    /* FIX: stack buffer — was static (not thread-safe) */
    char b64[(CFG_CHUNK_BYTES * 4 / 3) + 8];
    size_t b64_len = 0;
    int r = mbedtls_base64_encode(
        (unsigned char *)b64, sizeof(b64), &b64_len,
        (const unsigned char *)pcm, raw_bytes);
    if (r != 0) return;

    const char *pfx = "{\"type\":\"live_audio_chunk\",\"audio\":\"";
    const char *sfx = "\"}";
    char msg[sizeof(b64) + 64];
    int n = snprintf(msg, sizeof(msg), "%s%.*s%s",
                     pfx, (int)b64_len, b64, sfx);
    if (n > 0) ws_send_text(msg);
}

void protocol_send_live_end(const char *reason) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"live_line_end\",\"reason\":\"%s\"}", reason);
    ws_send_text(buf);
}

void protocol_send_pong(const char *ping_id) {
    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"pong\",\"ping_id\":\"%s\"}", ping_id);
    ws_send_text(buf);
}
