#include <stdbool.h>
#include "recorder.h"
#include "audio.h"
#include "config.h"
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "REC";

static int16_t  *s_buf      = NULL;
static size_t    s_samples  = 0;
static volatile bool s_active = false;
static uint32_t  s_start_ms = 0;
static char     *s_b64      = NULL;
static size_t    s_b64_len  = 0;

bool recorder_start(void) {
    if (s_active) return false;
    recorder_release();
    s_buf = heap_caps_malloc(CFG_RECORD_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_buf) { ESP_LOGE(TAG, "PSRAM malloc failed"); return false; }
    s_samples  = 0;
    s_active   = true;
    s_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "started");
    return true;
}

void recorder_feed(void) {
    if (!s_active || !s_buf) return;
    if (s_samples * sizeof(int16_t) >= CFG_RECORD_MAX_BYTES) {
        s_active = false;
        ESP_LOGI(TAG, "auto-stop: max length");
        return;
    }
    static int16_t chunk[CFG_CHUNK_SAMPLES];
    size_t got = audio_mic_read(chunk, CFG_CHUNK_SAMPLES, 50);
    if (!got) return;
    size_t space = CFG_RECORD_MAX_BYTES / sizeof(int16_t) - s_samples;
    size_t n = got < space ? got : space;
    memcpy(s_buf + s_samples, chunk, n * sizeof(int16_t));
    s_samples += n;
}

void recorder_stop(void) {
    s_active = false;
    ESP_LOGI(TAG, "stopped: %u samples = %.1f s",
             (unsigned)s_samples,
             (float)s_samples / CFG_SAMPLE_RATE);
}

bool recorder_is_active(void) { return s_active; }

uint32_t recorder_get_dur_ms(void) {
    return (uint32_t)((float)s_samples / CFG_SAMPLE_RATE * 1000.0f);
}

const char *recorder_get_b64(void) {
    if (!s_buf || s_samples == 0) return NULL;
    if (s_b64) return s_b64;  /* already encoded */
    size_t raw_bytes = s_samples * sizeof(int16_t);
    size_t b64_max   = ((raw_bytes + 2) / 3) * 4 + 4;
    s_b64 = heap_caps_malloc(b64_max, MALLOC_CAP_SPIRAM);
    if (!s_b64) { ESP_LOGE(TAG, "b64 malloc failed"); return NULL; }
    int r = mbedtls_base64_encode(
        (unsigned char *)s_b64, b64_max, &s_b64_len,
        (const unsigned char *)s_buf, raw_bytes);
    if (r != 0) {
        ESP_LOGE(TAG, "b64 encode error %d", r);
        heap_caps_free(s_b64); s_b64 = NULL; s_b64_len = 0;
        return NULL;
    }
    s_b64[s_b64_len] = '\0';
    ESP_LOGI(TAG, "encoded: %u bytes", (unsigned)s_b64_len);
    return s_b64;
}

size_t recorder_get_b64_len(void) { return s_b64_len; }

void recorder_release(void) {
    s_active = false;
    if (s_b64) { heap_caps_free(s_b64); s_b64 = NULL; s_b64_len = 0; }
    if (s_buf) { heap_caps_free(s_buf); s_buf = NULL; s_samples = 0; }
}
