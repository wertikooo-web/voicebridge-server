#include <stdbool.h>
#include "audio.h"
#include "config.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG     = "AUDIO";
static const char *NVS_NS  = "audio";
static const char *NVS_VOL = "vol";

static i2s_chan_handle_t s_tx  = NULL;
static i2s_chan_handle_t s_rx  = NULL;
static es8311_handle_t   s_es  = NULL;
static volatile bool     s_stop = false;
static uint8_t           s_vol  = CFG_SPK_VOLUME;

/* ── NVS volume persistence ──────────────────────────────────────────────── */
static void nvs_load_vol(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_VOL, &s_vol);
        nvs_close(h);
    }
}
static void nvs_save_vol(uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_VOL, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ── ES8311 ADC gain via direct I2C register write ───────────────────────── */
/* ES8311 register 0x17 = ADC volume (0x00=0dB, 0xFF=max ~+24dB in steps)   */
/* CFG_MIC_GAIN 0..255 maps directly to this register.                       */
static void es8311_set_adc_gain_reg(uint8_t gain_val) {
    uint8_t data[2] = {0x17, gain_val};
    i2c_master_write_to_device(ES8311_I2C_PORT, ES8311_ADDR,
                               data, sizeof(data), pdMS_TO_TICKS(100));
}

/* ── I2C bus init ────────────────────────────────────────────────────────── */
static esp_err_t i2c_init(void) {
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t r = i2c_param_config(ES8311_I2C_PORT, &cfg);
    if (r != ESP_OK) return r;
    return i2c_driver_install(ES8311_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

/* ── I2S duplex init ─────────────────────────────────────────────────────── */
static esp_err_t i2s_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = CFG_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = CFG_DMA_BUF_LEN;
    chan_cfg.auto_clear    = true;

    esp_err_t r = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (r != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(r)); return r; }

    i2s_std_config_t std = {
        .clk_cfg = {
            .sample_rate_hz = CFG_SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,  /* MCLK = 256 × Fs  */
        },
        /* Philips I2S standard — ES8311 default slave mode                 */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,   /* ESP32 → ES8311 (DAC/playback)      */
            .din  = PIN_I2S_DIN,    /* ES8311 → ESP32 (ADC/recording)     */
            .invert_flags = {0, 0, 0},
        },
    };

    r = i2s_channel_init_std_mode(s_tx, &std);
    if (r != ESP_OK) { ESP_LOGE(TAG, "I2S TX init: %s", esp_err_to_name(r)); return r; }
    r = i2s_channel_init_std_mode(s_rx, &std);
    if (r != ESP_OK) { ESP_LOGE(TAG, "I2S RX init: %s", esp_err_to_name(r)); return r; }

    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    return ESP_OK;
}

/* ── ES8311 codec init ───────────────────────────────────────────────────── */
static esp_err_t codec_init(void) {
    /* es8311_create: create handle, binds to I2C port + address            */
    s_es = es8311_create(ES8311_I2C_PORT, ES8311_ADDR);
    if (!s_es) {
        ESP_LOGE(TAG, "es8311_create failed — check I2C wiring SDA=%d SCL=%d",
                 PIN_I2C_SDA, PIN_I2C_SCL);
        return ESP_FAIL;
    }

    /* es8311_clock_config_t: only clock/mclk parameters, no gain here     */
    es8311_clock_config_t clk = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,              /* MCLK from GPIO11      */
        .mclk_frequency     = CFG_SAMPLE_RATE * 256,  /* 256 * 16000 = 4096000 Hz */
        .sample_frequency   = CFG_SAMPLE_RATE,   /* 16000 Hz              */
    };

    /* es8311_init: configure clocks + ADC/DAC bit resolution               */
    esp_err_t r = es8311_init(s_es, &clk,
                              ES8311_RESOLUTION_16,   /* ADC (microphone)  */
                              ES8311_RESOLUTION_16);  /* DAC (speaker)     */
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "es8311_init: %s", esp_err_to_name(r));
        return r;
    }

    /* Microphone: single-ended (not differential)                          */
    es8311_microphone_config(s_es, false);

    /* Speaker volume: 0..100                                               */
    int vol_set = 0;
    es8311_voice_volume_set(s_es, (int)s_vol, &vol_set);

    /* ADC gain via direct register write (no API function for this)        */
    es8311_set_adc_gain_reg(CFG_MIC_GAIN);

    ESP_LOGI(TAG, "ES8311 OK  vol=%d mic_gain_reg=0x%02X", s_vol, CFG_MIC_GAIN);
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ════════════════════════════════════════════════════════════════════════════ */


static void amp_enable(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_AMP_ENABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_AMP_ENABLE, 1);
    ESP_LOGI(TAG, "Amplifier enabled GPIO%d", PIN_AMP_ENABLE);
}

bool audio_init(void) {
    nvs_load_vol();
    if (i2c_init()   != ESP_OK) { ESP_LOGE(TAG, "I2C init failed");   return false; }
    if (i2s_init()   != ESP_OK) { ESP_LOGE(TAG, "I2S init failed");   return false; }
    if (codec_init() != ESP_OK) { ESP_LOGE(TAG, "Codec init failed"); return false; }
    amp_enable();
    ESP_LOGI(TAG, "audio_init OK");
    return true;
}

size_t audio_mic_read(int16_t *buf, size_t max_samples, uint32_t timeout_ms) {
    if (!s_rx) return 0;
    size_t bytes_read = 0;
    esp_err_t r = i2s_channel_read(s_rx, buf,
                                   max_samples * sizeof(int16_t),
                                   &bytes_read,
                                   pdMS_TO_TICKS(timeout_ms));
    return (r == ESP_OK) ? bytes_read / sizeof(int16_t) : 0;
}

size_t audio_spk_write(const int16_t *buf, size_t samples) {
    if (!s_tx || !buf || samples == 0) return 0;
    size_t written = 0;
    i2s_channel_write(s_tx, buf, samples * sizeof(int16_t),
                      &written, pdMS_TO_TICKS(200));
    return written / sizeof(int16_t);
}

void audio_vol_up(void) {
    if (!s_es || s_vol >= 100) return;
    s_vol = (s_vol + 5 > 100) ? 100 : s_vol + 5;
    int vs = 0;
    es8311_voice_volume_set(s_es, (int)s_vol, &vs);
    nvs_save_vol(s_vol);
    ESP_LOGI(TAG, "vol → %d", s_vol);
}

void audio_vol_down(void) {
    if (!s_es || s_vol == 0) return;
    s_vol = (s_vol < 5) ? 0 : s_vol - 5;
    int vs = 0;
    es8311_voice_volume_set(s_es, (int)s_vol, &vs);
    nvs_save_vol(s_vol);
    ESP_LOGI(TAG, "vol → %d", s_vol);
}

uint8_t audio_vol_get(void) { return s_vol; }

void audio_stop(void) { s_stop = true; }

void audio_play_wav(const char *path) {
    if (!s_tx) return;
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "WAV not found: %s", path); return; }
    fseek(f, 44, SEEK_SET);
    s_stop = false;
    int16_t buf[256];
    size_t n;
    while (!s_stop && (n = fread(buf, sizeof(int16_t), 256, f)) > 0)
        audio_spk_write(buf, n);
    fclose(f);
}

/* ── Tone synthesis ──────────────────────────────────────────────────────── */
static void write_tone(float freq, uint32_t dur_ms, float amp) {
    int16_t  buf[256];
    uint32_t total  = (uint32_t)((CFG_SAMPLE_RATE / 1000.0f) * dur_ms);
    uint32_t done   = 0;
    float    ph     = 0.0f;
    float    ph_inc = 2.0f * (float)M_PI * freq / (float)CFG_SAMPLE_RATE;
    int16_t  a      = (int16_t)(amp * 32767.0f);
    while (!s_stop && done < total) {
        size_t n = (total - done < 256) ? (total - done) : 256;
        for (size_t i = 0; i < n; i++) {
            buf[i] = (int16_t)(sinf(ph) * (float)a);
            ph += ph_inc;
            if (ph >= 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
        }
        audio_spk_write(buf, n);
        done += n;
    }
}

static void write_silence(uint32_t dur_ms) {
    static const int16_t zeros[256] = {0};
    uint32_t total = (CFG_SAMPLE_RATE / 1000) * dur_ms;
    uint32_t done  = 0;
    while (!s_stop && done < total) {
        size_t n = (total - done < 256) ? (total - done) : 256;
        audio_spk_write(zeros, n);
        done += n;
    }
}

void audio_play_live_alert(void) {
    s_stop = false;
    const float notes[3] = {523.25f, 659.26f, 783.99f};
    for (int i = 0; i < 3; i++) { write_tone(notes[i], 380, 0.22f); write_silence(80); }
    write_silence(200);
    write_tone(783.99f, 900, 0.18f);
    write_silence(100);
}

void audio_play_warning_beep(void) {
    s_stop = false;
    for (int i = 0; i < 3; i++) { write_tone(880.0f, 180, 0.18f); write_silence(120); }
    write_silence(500);
    write_tone(660.0f, 900, 0.22f);
}

void audio_play_sent_chime(void) {
    s_stop = false;
    write_tone(783.99f, 120, 0.16f);
    write_silence(30);
    write_tone(1046.5f, 200, 0.14f);
}
