#include <stdbool.h>
#include "gesture.h"
#include "config.h"
#include "iot_button.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char    *TAG   = "GESTURE";
static gesture_cb_t   s_cb  = NULL;

/* SOS hold tracking (set when BUTTON_LONG_PRESS_START fires) */
static volatile bool     s_sos_armed = false;
static volatile uint32_t s_sos_ms    = 0;

/* ════════════════════════════════════════════════════════════════════════════
   PTT / SOS BUTTON  (GPIO10)

   Mapping to iot_button events:
     BUTTON_PRESS_DOWN      → GESTURE_PTT_PRESS      (recording starts)
     BUTTON_PRESS_UP        → GESTURE_PTT_RELEASE     (recording stops / send)
     BUTTON_LONG_PRESS_START→ GESTURE_SOS_ARMED       (held 5 sec → arm SOS)
     BUTTON_PRESS_UP after  → GESTURE_SOS_CANCEL      (released < confirm win)

   The SOS confirm window (3 sec) is checked in main_task via check_sos_window().
   ════════════════════════════════════════════════════════════════════════════ */
static button_handle_t s_ptt = NULL;

static void init_lunara(void);

static void ptt_press_cb(void *arg, void *data) {
    /* Ignore short presses while SOS is armed — only cancel on release */
    if (s_sos_armed) return;
    if (s_cb) s_cb(GESTURE_PTT_PRESS);
}

static void ptt_release_cb(void *arg, void *data) {
    if (s_sos_armed) {
        s_sos_armed = false;
        if (s_cb) s_cb(GESTURE_SOS_CANCEL);
        return;
    }
    if (s_cb) s_cb(GESTURE_PTT_RELEASE);
}

static void ptt_long_start_cb(void *arg, void *data) {
    /* iot_button fires LONG_PRESS_START after long_press_time ms            */
    s_sos_armed = true;
    s_sos_ms    = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_cb) s_cb(GESTURE_SOS_ARMED);
}

static void init_ptt(void) {
    button_config_t cfg = {
        .type            = BUTTON_TYPE_GPIO,
        .long_press_time = CFG_SOS_HOLD_MS,
        .short_press_time = CFG_TOUCH_DEBOUNCE_MS,
        .gpio_button_config = {
            .gpio_num    = PIN_BUTTON,
            .active_level = 0,   /* active LOW */
        },
    };
    s_ptt = iot_button_create(&cfg);
    if (!s_ptt) { ESP_LOGE(TAG, "PTT button create failed"); return; }

    iot_button_register_cb(s_ptt, BUTTON_PRESS_DOWN,       ptt_press_cb,      NULL);
    iot_button_register_cb(s_ptt, BUTTON_PRESS_UP,         ptt_release_cb,    NULL);
    iot_button_register_cb(s_ptt, BUTTON_LONG_PRESS_START, ptt_long_start_cb, NULL);

    ESP_LOGI(TAG, "PTT/SOS on GPIO%d  (long=%u ms)", PIN_BUTTON, CFG_SOS_HOLD_MS);
}

/* ════════════════════════════════════════════════════════════════════════════
   VOLUME UP  (GPIO7)
   ════════════════════════════════════════════════════════════════════════════ */
static button_handle_t s_vol_up = NULL;

static void vol_up_cb(void *arg, void *data) {
    if (s_cb) s_cb(GESTURE_VOL_UP);
}

static void init_vol_up(void) {
    button_config_t cfg = {
        .type             = BUTTON_TYPE_GPIO,
        .long_press_time  = 1000,
        .short_press_time = CFG_TOUCH_DEBOUNCE_MS,
        .gpio_button_config = {
            .gpio_num    = PIN_VOL_UP,
            .active_level = 0,
        },
    };
    s_vol_up = iot_button_create(&cfg);
    if (!s_vol_up) { ESP_LOGE(TAG, "Vol Up button create failed"); return; }

    /* Single press: one step up */
    iot_button_register_cb(s_vol_up, BUTTON_PRESS_DOWN,      vol_up_cb, NULL);
    iot_button_register_cb(s_vol_up, BUTTON_LONG_PRESS_HOLD, vol_up_cb, NULL);

    ESP_LOGI(TAG, "Vol Up on GPIO%d", PIN_VOL_UP);
}

/* ════════════════════════════════════════════════════════════════════════════
   VOLUME DOWN  (GPIO8)
   ════════════════════════════════════════════════════════════════════════════ */
static button_handle_t s_vol_dn = NULL;

static void vol_dn_cb(void *arg, void *data) {
    if (s_cb) s_cb(GESTURE_VOL_DOWN);
}

static void init_vol_down(void) {
    button_config_t cfg = {
        .type             = BUTTON_TYPE_GPIO,
        .long_press_time  = 1000,
        .short_press_time = CFG_TOUCH_DEBOUNCE_MS,
        .gpio_button_config = {
            .gpio_num    = PIN_VOL_DOWN,
            .active_level = 0,
        },
    };
    s_vol_dn = iot_button_create(&cfg);
    if (!s_vol_dn) { ESP_LOGE(TAG, "Vol Down button create failed"); return; }

    iot_button_register_cb(s_vol_dn, BUTTON_PRESS_DOWN,      vol_dn_cb, NULL);
    iot_button_register_cb(s_vol_dn, BUTTON_LONG_PRESS_HOLD, vol_dn_cb, NULL);

    ESP_LOGI(TAG, "Vol Down on GPIO%d", PIN_VOL_DOWN);
}

/* ════════════════════════════════════════════════════════════════════════════
   RESET WIFI  (GPIO9) — hold CFG_RESET_WIFI_HOLD_MS → erase NVS & reboot
   ════════════════════════════════════════════════════════════════════════════ */
static button_handle_t s_reset = NULL;

static void reset_wifi_cb(void *arg, void *data) {
    if (s_cb) s_cb(GESTURE_RESET_WIFI);
}

static void init_reset_wifi(void) {
    button_config_t cfg = {
        .type             = BUTTON_TYPE_GPIO,
        .long_press_time  = CFG_RESET_WIFI_HOLD_MS,
        .short_press_time = CFG_TOUCH_DEBOUNCE_MS,
        .gpio_button_config = {
            .gpio_num    = PIN_RESET_WIFI,
            .active_level = 0,
        },
    };
    s_reset = iot_button_create(&cfg);
    if (!s_reset) { ESP_LOGE(TAG, "Reset WiFi button create failed"); return; }

    iot_button_register_cb(s_reset, BUTTON_LONG_PRESS_START, reset_wifi_cb, NULL);

    ESP_LOGI(TAG, "Reset WiFi on GPIO%d  (hold=%u ms)",
             PIN_RESET_WIFI, CFG_RESET_WIFI_HOLD_MS);
}

/* ════════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ════════════════════════════════════════════════════════════════════════════ */
void gesture_init(gesture_cb_t cb) {
    s_cb = cb;
    init_ptt();
    init_vol_up();
    init_vol_down();
    init_reset_wifi();
    init_lunara();
    ESP_LOGI(TAG, "gesture init complete");
}

uint32_t gesture_sos_hold_ms(void) { return s_sos_ms; }

/* ════════════════════════════════════════════════════════════════════════════
   LUNARA AI BUTTON  (GPIO16) — triple tap to call Lunara
   ════════════════════════════════════════════════════════════════════════════ */
static button_handle_t s_lunara = NULL;
static volatile uint8_t  s_lunara_tap_count = 0;
static volatile uint32_t s_lunara_last_tap  = 0;

#define LUNARA_TAP_WINDOW_MS  800   /* window to count taps                 */
#define LUNARA_MIN_TAPS       3     /* 3 or more taps = Lunara              */

static void lunara_tap_cb(void *arg, void *data) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* Reset counter if tap window expired */
    if (now - s_lunara_last_tap > LUNARA_TAP_WINDOW_MS) {
        s_lunara_tap_count = 0;
    }
    s_lunara_tap_count++;
    s_lunara_last_tap = now;

    if (s_lunara_tap_count >= LUNARA_MIN_TAPS) {
        s_lunara_tap_count = 0;
        if (s_cb) s_cb(GESTURE_LUNARA);
    }
}

static void init_lunara(void) {
    button_config_t cfg = {
        .type             = BUTTON_TYPE_GPIO,
        .long_press_time  = 1000,
        .short_press_time = CFG_TOUCH_DEBOUNCE_MS,
        .gpio_button_config = {
            .gpio_num     = PIN_LUNARA,
            .active_level = 0,
        },
    };
    s_lunara = iot_button_create(&cfg);
    if (!s_lunara) { ESP_LOGE(TAG, "Lunara button create failed"); return; }

    iot_button_register_cb(s_lunara, BUTTON_PRESS_DOWN, lunara_tap_cb, NULL);
    ESP_LOGI(TAG, "Lunara on GPIO%d  (triple tap)", PIN_LUNARA);
}
