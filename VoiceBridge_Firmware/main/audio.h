#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Initialise ES8311 codec (I2C config + I2S audio).
   Must be called once in app_main before any other audio function.          */
bool audio_init(void);

/* Microphone: read `max_samples` PCM-16 samples. Returns samples read.     */
size_t audio_mic_read(int16_t *buf, size_t max_samples, uint32_t timeout_ms);

/* Speaker: write `samples` PCM-16 samples to DAC. Non-blocking (DMA).     */
size_t audio_spk_write(const int16_t *buf, size_t samples);

/* Volume control (0–100). Persisted to NVS on change.                     */
void   audio_vol_up(void);
void   audio_vol_down(void);
uint8_t audio_vol_get(void);

/* Play a 16 kHz 16-bit mono WAV file from SPIFFS. Blocks until done.
   Safe to call if file missing — logs a warning and returns.               */
void audio_play_wav(const char *spiffs_path);

/* Synthesised sounds (blocking)                                            */
void audio_play_live_alert(void);    /* C5-E5-G5 + sustained G5            */
void audio_play_warning_beep(void);  /* 3 short + 1 long beep              */
void audio_play_sent_chime(void);    /* G5→C6 confirmation chime           */

/* Stop any in-progress playback immediately.                               */
void audio_stop(void);
