#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GESTURE_PTT_PRESS,        /* Short press PTT button — start recording   */
    GESTURE_PTT_RELEASE,      /* PTT released — stop recording              */
    GESTURE_SOS_ARMED,        /* PTT held 5 sec — SOS armed, can cancel     */
    GESTURE_SOS_CANCEL,       /* Released before confirm — SOS cancelled    */
    GESTURE_VOL_UP,           /* Volume Up button                           */
    GESTURE_VOL_DOWN,         /* Volume Down button                         */
    GESTURE_RESET_WIFI,       /* Reset WiFi button held 3 sec               */
    GESTURE_LUNARA,           /* Lunara button triple-tap                   */
} gesture_event_t;

typedef void (*gesture_cb_t)(gesture_event_t ev);

void     gesture_init(gesture_cb_t cb);
/* No gesture_update() needed — all handled via iot_button callbacks        */
uint32_t gesture_sos_hold_ms(void);
