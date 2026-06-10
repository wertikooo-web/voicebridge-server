#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

void protocol_live_queue_init(void);
void protocol_handle_incoming(const char *json, int len);

void protocol_send_wants_to_talk(void);
void protocol_send_lunara_start(void);
void protocol_send_help_request(const char *source);   /* "sphere"|"bracelet" */
void protocol_send_dad_voice(const char *b64, size_t b64len, uint32_t dur_ms);
void protocol_send_live_chunk(const int16_t *pcm, size_t samples);
void protocol_send_live_end(const char *reason);
void protocol_send_register(void);
void protocol_send_pong(const char *ping_id);

/* Live audio queue (written by WS handler, read by audio task) */
bool protocol_live_queue_push(const uint8_t *pcm, size_t len);
bool protocol_live_queue_pop(uint8_t *out, size_t *out_len);
