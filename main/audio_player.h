#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

/**
 * @brief Initialise le matériel audio (I2C, ES8311, I2S, GPIOs ampli/codec)
 */
esp_err_t audio_player_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Joue un fichier MP3 pré-enregistré par ID (1..6)
 */
esp_err_t audio_player_play_file(int id);

/**
 * @brief Joue un buffer MP3 depuis la PSRAM (ex: ElevenLabs TTS)
 * @note Le buffer sera libéré automatiquement après lecture
 */
esp_err_t audio_player_play_buffer(uint8_t *buf, size_t len);

/**
 * @brief Arrête la lecture en cours
 */
void audio_player_stop(void);

/**
 * @brief Règle le volume (0.0 à 1.0)
 */
void audio_player_set_volume(float vol);

/**
 * @brief Vérifie si une lecture est en cours
 */
bool audio_player_is_playing(void);

/**
 * @brief Retourne le volume actuel
 */
float audio_player_get_volume(void);

/**
 * @brief Retourne le nom du fichier en cours de lecture
 */
const char *audio_player_current_file(void);

/**
 * @brief Retourne la dernière erreur
 */
const char *audio_player_last_error(void);

// ===== IDs fichiers MP3 de dialogue =====
#define AUDIO_MSG_COMMENT_AIDER  7
#define AUDIO_MSG_FAIT           8
#define AUDIO_MSG_BONNE_JOURNEE  9
#define AUDIO_MSG_OUI           10
#define AUDIO_MSG_ERROR         13

// ===== Microphone capture API =====

/**
 * @brief Active le micro (ES8311 ADC via I2S RX, 16kHz)
 */
esp_err_t audio_player_mic_start(void);

/**
 * @brief Désactive le micro
 */
esp_err_t audio_player_mic_stop(void);

/**
 * @brief Lit des échantillons mono 16-bit depuis le micro
 */
esp_err_t audio_player_mic_read(int16_t *buf, size_t max_samples,
                                 size_t *out_samples, TickType_t timeout);

/**
 * @brief Vérifie si le micro est actif
 */
bool audio_player_mic_is_active(void);
