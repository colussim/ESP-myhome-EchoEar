#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char text[1024];   // contains the raw JSON returned by the backend
    bool success;
} ha_stt_result_t;

esp_err_t ha_client_init(void);

/**
 * @brief Sends a raw WAV to the backend and retrieves the raw JSON.
 *
 * The backend returns, for example:
 * {
 *   "status": "success",
 *   "heard": "Kira",
 *   "kiraactive": true,
 *   "reply": "Oui ?",
 *   "ha_ack": "no_action",
 *   "category": "WAKE"
 * }
 */
esp_err_t ha_stt_recognize(const uint8_t *wav_data, size_t wav_len,
                           ha_stt_result_t *result);

/**
 * @brief Sends text to the TTS backend and returns raw WAV bytes in PSRAM.
 *        The caller must free *out_buf with heap_caps_free() when done,
 *        UNLESS passed directly to audio_player_play_buffer() which frees it.
 */
esp_err_t ha_tts_speak(const char *text, uint8_t **out_buf, size_t *out_len);