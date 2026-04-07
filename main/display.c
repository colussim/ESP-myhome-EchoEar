#include "display.h"
#include "board.h"
#include "disp_init_data.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_st77916.h"

static const char *TAG = "display";

#define LCD_HOST SPI2_HOST
#define STRIP_H  60
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;
static uint16_t *s_bounce = NULL;

esp_err_t display_init(void)
{
    // 1) Enable LCD power via LCD_EN (GPIO 9, active LOW)
    gpio_config_t en_cfg = {
        .pin_bit_mask = 1ULL << BOARD_LCD_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&en_cfg);
    gpio_set_level(BOARD_LCD_EN, 0);  // LOW = enable
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "LCD_EN GPIO %d set LOW (power enabled)", BOARD_LCD_EN);

    // 2) Backlight OFF pendant l'init
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(BOARD_LCD_BL, 0);

    // 3) QSPI bus (pins from official ESP-VoCat BSP)
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOARD_LCD_CLK,
        .data0_io_num = BOARD_LCD_D0,
        .data1_io_num = BOARD_LCD_D1,
        .data2_io_num = BOARD_LCD_D2,
        .data3_io_num = BOARD_LCD_D3,
        .max_transfer_sz = DISP_W * STRIP_H * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "SPI bus");
    ESP_LOGI(TAG, "SPI bus init OK (CLK=%d D0=%d D1=%d D2=%d D3=%d)",
             BOARD_LCD_CLK, BOARD_LCD_D0, BOARD_LCD_D1, BOARD_LCD_D2, BOARD_LCD_D3);

    // 4) Panel IO — DC=GPIO45, CS=GPIO14, 80MHz, quad mode (from BSP)
    //    trans_queue_depth=1 : synchrone mode, essential because we reuse
    //    a single bounce buffer — each draw_bitmap must finish before the next one.
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io), TAG, "Panel IO");
    ESP_LOGI(TAG, "Panel IO QSPI (DC=%d CS=%d 80MHz)", BOARD_LCD_DC, BOARD_LCD_CS);

    // 5) ST77916 panel with custom init commands and QSPI interface
    st77916_vendor_config_t vendor_cfg = {
        .init_cmds = disp_init_data,
        .init_cmds_size = sizeof(disp_init_data) / sizeof(disp_init_data[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
        .flags = { .reset_active_high = 1 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(io, &panel_cfg, &s_panel), TAG, "ST77916 create");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "Panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel init");
    ESP_LOGI(TAG, "Panel reset+init OK (reset_active_high, %zu custom cmds)",
             sizeof(disp_init_data) / sizeof(disp_init_data[0]));

    esp_lcd_panel_invert_color(s_panel, true);

    // 6) Framebuffer en PSRAM
    s_fb = heap_caps_malloc(DISP_W * DISP_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "FB PSRAM alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, DISP_W * DISP_H * sizeof(uint16_t));

    // 7) Bounce buffer en SRAM interne DMA-capable
    s_bounce = heap_caps_malloc(DISP_W * STRIP_H * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bounce) {
        ESP_LOGE(TAG, "Bounce buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    // 8) Display ON + écran blanc initial
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "Display on");
    for (int y = 0; y < DISP_H; y += STRIP_H) {
        int ye = y + STRIP_H;
        if (ye > DISP_H) ye = DISP_H;
        size_t bytes = DISP_W * (ye - y) * sizeof(uint16_t);
        memset(s_bounce, 0xFF, bytes);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, DISP_W, ye, s_bounce);
    }

    // 9) Backlight ON
    gpio_set_level(BOARD_LCD_BL, 1);
    ESP_LOGI(TAG, "ST77916 %dx%d QSPI ready, backlight ON", DISP_W, DISP_H);
    return ESP_OK;
}

void display_flush(void)
{
    if (!s_panel || !s_fb || !s_bounce) return;
    for (int y = 0; y < DISP_H; y += STRIP_H) {
        int ye = y + STRIP_H;
        if (ye > DISP_H) ye = DISP_H;
        size_t bytes = DISP_W * (ye - y) * sizeof(uint16_t);
        // Copier PSRAM → SRAM DMA-capable avant envoi SPI
        memcpy(s_bounce, &s_fb[y * DISP_W], bytes);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, DISP_W, ye, s_bounce);
    }
}

uint16_t *display_get_fb(void)
{
    return s_fb;
}
