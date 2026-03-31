#include "led_anim.h"
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "led_anim";

#define LED_LEDC_CHANNEL LEDC_CHANNEL_0
#define LED_LEDC_TIMER   LEDC_TIMER_0

static TaskHandle_t s_anim_task = NULL;
static volatile bool s_anim_running = false;

static void led_breathe_task(void *arg)
{
    int duty = 0;
    int step = 5;

    while (s_anim_running) {
        duty += step;
        if (duty >= 255) { duty = 255; step = -5; }
        if (duty <= 0)   { duty = 0;   step = 5;  }

        ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Éteindre proprement
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL);
    s_anim_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t led_anim_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LED_LEDC_TIMER,
        .freq_hz         = 1000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "LEDC timer");

    ledc_channel_config_t ch_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LED_LEDC_CHANNEL,
        .timer_sel  = LED_LEDC_TIMER,
        .gpio_num   = BOARD_LED_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "LEDC channel");

    ESP_LOGI(TAG, "LED animation initialisée sur GPIO%d", BOARD_LED_GPIO);
    return ESP_OK;
}

void led_anim_start(void)
{
    if (s_anim_running) return;
    s_anim_running = true;
    xTaskCreate(led_breathe_task, "led_anim", 2048, NULL, 5, &s_anim_task);
}

void led_anim_stop(void)
{
    if (!s_anim_running) return;
    s_anim_running = false;
    // La task se termine d'elle-même après la prochaine itération du fade
}
