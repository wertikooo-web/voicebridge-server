#include "led.h"
#include "config.h"
#include <math.h>
#include <string.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LED";

/* ── WS2812B RMT timings (in ns) ─────────────────────────────────────────── */
#define WS2812_T0H_NS  350
#define WS2812_T0L_NS  900
#define WS2812_T1H_NS  900
#define WS2812_T1L_NS  350
#define WS2812_RESET_NS 50000

static rmt_channel_handle_t  s_rmt_chan  = NULL;
static rmt_encoder_handle_t  s_encoder   = NULL;
static led_state_t           s_state     = LED_OFFLINE;
static uint32_t              s_state_ms  = 0;

/* GRB byte order for WS2812B */
typedef struct { uint8_t g, r, b; } rgb_t;
static rgb_t s_pixels[LED_COUNT];

/* ── RMT bytes encoder for WS2812B ───────────────────────────────────────── */
typedef struct {
    rmt_encoder_t   base;
    rmt_encoder_t  *copy_encoder;
    rmt_encoder_t  *bytes_encoder;
    int             state;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *data, size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *e = __containerof(encoder, ws2812_encoder_t, base);
    size_t encoded = 0;
    rmt_encode_state_t sess = RMT_ENCODING_RESET, res;

    if (e->state == 0) {
        encoded += e->bytes_encoder->encode(e->bytes_encoder, channel,
                                            data, data_size, &sess);
        if (sess & RMT_ENCODING_COMPLETE) e->state = 1;
        if (sess & RMT_ENCODING_MEM_FULL) { *ret_state = RMT_ENCODING_MEM_FULL; return encoded; }
    }
    if (e->state == 1) {
        /* Reset pulse */
        static const rmt_symbol_word_t reset = {
            .level0 = 0, .duration0 = (WS2812_RESET_NS / 25),
            .level1 = 0, .duration1 = (WS2812_RESET_NS / 25)
        };
        encoded += e->copy_encoder->encode(e->copy_encoder, channel,
                                           &reset, sizeof(reset), &res);
        if (res & RMT_ENCODING_COMPLETE) {
            e->state = 0;
            *ret_state = RMT_ENCODING_COMPLETE;
        }
        if (res & RMT_ENCODING_MEM_FULL) *ret_state = RMT_ENCODING_MEM_FULL;
    }
    return encoded;
}

static esp_err_t ws2812_reset(rmt_encoder_t *encoder) {
    ws2812_encoder_t *e = __containerof(encoder, ws2812_encoder_t, base);
    e->bytes_encoder->reset(e->bytes_encoder);
    e->copy_encoder->reset(e->copy_encoder);
    e->state = 0;
    return ESP_OK;
}

static esp_err_t ws2812_del(rmt_encoder_t *encoder) {
    ws2812_encoder_t *e = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(e->bytes_encoder);
    rmt_del_encoder(e->copy_encoder);
    free(e);
    return ESP_OK;
}

static void create_ws2812_encoder(void) {
    ws2812_encoder_t *e = calloc(1, sizeof(*e));
    e->base.encode = ws2812_encode;
    e->base.reset  = ws2812_reset;
    e->base.del    = ws2812_del;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = { .level0 = 1, .duration0 = WS2812_T0H_NS / 25,
                  .level1 = 0, .duration1 = WS2812_T0L_NS / 25 },
        .bit1 = { .level0 = 1, .duration0 = WS2812_T1H_NS / 25,
                  .level1 = 0, .duration1 = WS2812_T1L_NS / 25 },
        .flags.msb_first = 1,
    };
    rmt_new_bytes_encoder(&bytes_cfg, &e->bytes_encoder);

    rmt_copy_encoder_config_t copy_cfg = {};
    rmt_new_copy_encoder(&copy_cfg, &e->copy_encoder);

    s_encoder = &e->base;
}

static void pixels_show(void) {
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s_rmt_chan, s_encoder, s_pixels,
                 sizeof(rgb_t) * LED_COUNT, &tx_cfg);
    rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(50));
}

static void set_all(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < LED_COUNT; i++) {
        s_pixels[i].r = r; s_pixels[i].g = g; s_pixels[i].b = b;
    }
    pixels_show();
}

/* ── Pulse helper ────────────────────────────────────────────────────────── */
static uint8_t pulse_val(uint32_t ms, float hz, uint8_t mn, uint8_t mx) {
    float v = (sinf(2.0f * (float)M_PI * hz * ms / 1000.0f) + 1.0f) / 2.0f;
    return (uint8_t)(mn + v * (mx - mn));
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void led_init(void) {
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num        = PIN_LED,
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .resolution_hz   = 40000000,   /* 40 MHz → 25 ns per tick             */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &s_rmt_chan));
    create_ws2812_encoder();
    ESP_ERROR_CHECK(rmt_enable(s_rmt_chan));

    s_state_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    set_all(0, 0, 0);
    ESP_LOGI(TAG, "init OK");
}

void led_set(led_state_t state) {
    s_state    = state;
    s_state_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

led_state_t led_get(void) { return s_state; }

void led_update(void) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t t   = now - s_state_ms;
    uint8_t  b;

    switch (s_state) {
    case LED_OFFLINE:
        b = pulse_val(now, 0.3f, 5, 30);
        set_all(b/3, b/3, b/3);
        break;

    case LED_IDLE:
        b = pulse_val(now, 1.1f, 55, 190);
        set_all(b, (uint8_t)(b*0.92f), (uint8_t)(b*0.55f)); /* warm white */
        break;

    case LED_WANTS_TO_TALK:
        if (t < 2000) {
            b = (uint8_t)(240 * (1.0f - (float)t / 2000.0f));
            set_all(b, (uint8_t)(b*0.92f), (uint8_t)(b*0.55f));
        } else {
            led_set(LED_IDLE);
        }
        break;

    case LED_AUDIO_INCOMING:
        if (t < 4500) {
            b = pulse_val(now, 1.0f, 40, 220);
            set_all(25, 80, b);
        } else {
            led_set(LED_IDLE);
        }
        break;

    case LED_LIVE_LINE:
        b = pulse_val(now, 1.1f, 70, 230);
        set_all(10, b, 45);   /* green */
        break;

    case LED_LIVE_WARNING:
        b = pulse_val(now, 4.0f, 100, 240);
        set_all(b, (uint8_t)(b*0.65f), 0);   /* yellow-orange */
        break;

    case LED_RECORDING:
        b = pulse_val(now, 1.0f, 55, 220);
        set_all(20, 65, b);   /* blue */
        break;

    case LED_REMIND_LATER:
        if (t < 6000) {
            b = pulse_val(now, 1.5f, 70, 210);
            set_all(b, (uint8_t)(b*0.82f), 0);   /* warm yellow */
        } else {
            led_set(LED_IDLE);
        }
        break;

    case LED_SOS_ARMING:
        b = pulse_val(now, 2.0f, 90, 255);
        set_all(b, 5, 5);
        break;

    case LED_SOS_ACTIVE: {
        int on = ((now / 180) % 2) == 0;
        set_all(on ? 255 : 20, on ? 5 : 0, on ? 5 : 0);
        break;
    }
    }
}
