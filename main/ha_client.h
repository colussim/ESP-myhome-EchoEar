#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char text[1024];   // contient le JSON brut renvoyé par le backend
    bool success;
} ha_stt_result_t;

esp_err_t ha_client_init(void);

/**
 * @brief Envoie un WAV brut au backend et récupère le JSON brut.
 *
 * Le backend renvoie par exemple :
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