#pragma once
#include <stdint.h>

typedef enum {
    LED_OFFLINE = 0,
    LED_IDLE,
    LED_WANTS_TO_TALK,   /* brief warm flash */
    LED_AUDIO_INCOMING,  /* blue pulse       */
    LED_LIVE_LINE,       /* green continuous */
    LED_LIVE_WARNING,    /* yellow fast      */
    LED_RECORDING,       /* blue 1 Hz        */
    LED_REMIND_LATER,    /* warm yellow 6 s  */
    LED_SOS_ARMING,      /* red filling      */
    LED_SOS_ACTIVE,      /* red fast blink   */
} led_state_t;

void led_init(void);
void led_set(led_state_t state);
led_state_t led_get(void);
/* Call from a dedicated task or timer every ~10 ms */
void led_update(void);
