#include "touch_wakeup.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"

#include "driver/i2c_master.h"

// Making the global I2C bus accessible
extern i2c_master_bus_handle_t s_i2c_bus;
#include "touch_wakeup.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"

#define TAG "touch"


#define POLL_DELAY_MS 50
#define DEBOUNCE_MS 500

static esp_lcd_touch_handle_t tp = NULL;

extern void main_touch_wake_request(void);
extern void main_notify_user_activity(void);




static void touch_task(void *arg)
{
    (void)arg;
    TickType_t last_trigger = 0;
    int log_count = 0;

    while (1) {
        esp_lcd_touch_read_data(tp);
        esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
        uint8_t point_num = 0;
        esp_err_t err = esp_lcd_touch_get_data(tp, points, &point_num, CONFIG_ESP_LCD_TOUCH_MAX_POINTS);

        log_count++;
        // DEBUG
       /* if ((log_count % 20) == 0) {
            ESP_LOGI(TAG, "touch points: %d", point_num);
        }*/

        if (err == ESP_OK && point_num > 0) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_trigger) > pdMS_TO_TICKS(DEBOUNCE_MS)) {
                ESP_LOGI(TAG, "Touch detected: x=%u y=%u", points[0].x, points[0].y);
                main_notify_user_activity();
                main_touch_wake_request();
                last_trigger = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_DELAY_MS));
    }
}
esp_err_t touch_wakeup_init(void)
{
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = 0x15,
        .scl_speed_hz = 400000,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 0,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 1,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle));

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
        .interrupt_callback = NULL,
        .user_data = NULL,
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &tp));

    BaseType_t ok = xTaskCreatePinnedToCore(
        touch_task,
        "touch_task",
        4096,
        NULL,
        4,
        NULL,
        0
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Touch ready (CST816S)");
    return ESP_OK;
}