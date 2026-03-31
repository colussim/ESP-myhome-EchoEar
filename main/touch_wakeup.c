#include "touch_wakeup.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/i2c_master.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"

#include "board.h"

#define TAG "touch"

// handle tactile
static esp_lcd_touch_handle_t tp = NULL;

// fonctions venant de main.c
extern void main_touch_wake_request(void);
extern void main_notify_user_activity(void);
extern bool main_is_screen_sleeping(void);
extern bool main_is_touch_wake_armed(void);

// I2C handle partagé (déjà créé dans main.c)
extern i2c_master_bus_handle_t s_i2c_bus;

static void touch_task(void *arg)
{
    int stable_touch_count = 0;

    while (1) {
        if (!main_is_touch_wake_armed()) {
            stable_touch_count = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        esp_lcd_touch_point_data_t points[1];
        uint8_t count = 0;

        esp_lcd_touch_read_data(tp);
        esp_lcd_touch_get_data(tp, points, &count, 1);

        if (count > 0) {
            stable_touch_count++;
        } else {
            stable_touch_count = 0;
        }

        // Wake seulement si le touch est stable sur plusieurs lectures
        if (stable_touch_count >= 3) {
            ESP_LOGI(TAG, "Touch wake detected");
            main_touch_wake_request();
            stable_touch_count = 0;
            vTaskDelay(pdMS_TO_TICKS(1000)); // debounce fort
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t touch_wakeup_init(void)
{
    ESP_LOGI(TAG, "Init CST816S touch");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 360,
        .y_max = 360,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t io_handle = NULL;

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = 0x15,          // adresse CST816S
    .scl_speed_hz = 400000,    // IMPORTANT
    .control_phase_bytes = 1,
    .dc_bit_offset = 0,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_i2c(
            s_i2c_bus,
            &io_config,
            &io_handle
        )
    );

    ESP_ERROR_CHECK(
        esp_lcd_touch_new_i2c_cst816s(
            io_handle,
            &tp_cfg,
            &tp
        )
    );

    xTaskCreatePinnedToCore(
        touch_task,
        "touch_task",
        4096,
        NULL,
        4,
        NULL,
        0
    );

    ESP_LOGI(TAG, "Touch ready (CST816S)");

    return ESP_OK;
}