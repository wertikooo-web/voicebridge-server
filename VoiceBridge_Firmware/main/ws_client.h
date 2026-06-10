#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef void (*ws_text_cb_t)(const char *data, int len);
typedef void (*ws_event_cb_t)(bool connected);

void ws_init(ws_text_cb_t on_text, ws_event_cb_t on_event);
bool ws_send_text(const char *json);
bool ws_send_text_len(const char *json, size_t len);
bool ws_is_connected(void);
