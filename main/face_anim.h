#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t face_anim_init(void);

void face_anim_start_talking(void);
void face_anim_stop_talking(void);
void face_anim_set_amplitude(uint8_t a);

// réglage runtime position bouche
void face_anim_set_position(int x, int y);
void face_anim_get_position(int *x, int *y);

#ifdef __cplusplus
}
#endif