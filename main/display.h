#pragma once

#include "esp_err.h"
#include <stdint.h>

#define DISP_W 360
#define DISP_H 360

esp_err_t display_init(void);
void display_flush(void);
uint16_t *display_get_fb(void);
