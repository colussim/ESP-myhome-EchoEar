#pragma once

#include "esp_err.h"

/**
 * @brief Initialize the voice command module
 *        (microphone capture + VAD + sending to Home Assistant)
 */
esp_err_t voice_cmd_init(void);
