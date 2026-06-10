#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool   recorder_start(void);
void   recorder_feed(void);        /* call repeatedly while recording   */
void   recorder_stop(void);
bool   recorder_is_active(void);

const char *recorder_get_b64(void);
size_t      recorder_get_b64_len(void);
uint32_t    recorder_get_dur_ms(void);
void        recorder_release(void);
