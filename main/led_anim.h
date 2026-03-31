#pragma once

#include "esp_err.h"

/**
 * @brief Initialise la LED (LEDC PWM sur GPIO43)
 */
esp_err_t led_anim_init(void);

/**
 * @brief Démarre l'animation de respiration pendant la lecture audio
 */
void led_anim_start(void);

/**
 * @brief Arrête l'animation et éteint la LED
 */
void led_anim_stop(void);
