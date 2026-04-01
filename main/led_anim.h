#pragma once

#include "esp_err.h"

/**
 * @brief Initializes the LED (LEDC PWM on GPIO43)
 */
esp_err_t led_anim_init(void);

/**
 * @brief Starts the breathing animation during audio playback
 */
void led_anim_start(void);

/**
 * @brief Stops the animation and turns off the LED
 */
void led_anim_stop(void);
