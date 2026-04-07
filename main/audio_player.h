#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/i2s_types.h" 
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

i2s_chan_handle_t audio_player_get_rx_handle(void);



/**
 * @brief Initialize the audio equipment (I2C, ES8311, I2S, GPIOs ampli/codec)
 */
esp_err_t audio_player_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Play a pre-recorded MP3 file by ID (1..6)
 */
esp_err_t audio_player_play_file(int id);

/**
 * @brief Play an MP3 buffer from PSRAM (e.g., ElevenLabs TTS)
 * @note The buffer will be automatically freed after playback
 */
esp_err_t audio_player_play_buffer(uint8_t *buf, size_t len);

/**
 * @brief Stop the current playback
 */
void audio_player_stop(void);

/**
 * @brief Set the volume (0.0 to 1.0)

 */
void audio_player_set_volume(float vol);

/**
 * @brief Check if playback is in progress
 */
bool audio_player_is_playing(void);

/**
 * @brief Get the current volume
 */
float audio_player_get_volume(void);

/**
 * @brief Get the name of the currently playing file
 */
const char *audio_player_current_file(void);

/**
 * @brief Get the last error

 */
const char *audio_player_last_error(void);

// ===== IDs file MP3 for dialogue =====
#define AUDIO_MSG_COMMENT_AIDER  7
#define AUDIO_MSG_FAIT           8
#define AUDIO_MSG_BONNE_JOURNEE  9
#define AUDIO_MSG_OUI           10
#define AUDIO_MSG_ERROR         13

// ===== Microphone capture API =====

/**
 * @brief Activate the microphone (ES8311 ADC via I2S RX, 16kHz)
 */
esp_err_t audio_player_mic_start(void);

/**
 * @brief Deactivate the microphone
 */
esp_err_t audio_player_mic_stop(void);

/**
 * @brief Read 16-bit mono samples from the microphone
 */
esp_err_t audio_player_mic_read(int16_t *buf, size_t max_samples,
                                 size_t *out_samples, TickType_t timeout);

/**
 * @brief Check if the microphone is active
 */
bool audio_player_mic_is_active(void);
