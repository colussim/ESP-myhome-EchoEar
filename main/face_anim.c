#include "face_anim.h"
#include "display.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "jpeg_decoder.h"

static const char *TAG = "face";

// -----------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------
#define W DISP_W
#define H DISP_H
#define FB_SIZE_BYTES ((size_t)W * (size_t)H * sizeof(uint16_t))

// -----------------------------------------------------------------------------
// Mouth patch only
// -----------------------------------------------------------------------------
#define MOUTH_W 115 //112
#define MOUTH_H 95 //88

//#define MOUTH_Y 155

static volatile int s_mouth_x = 114;
static volatile int s_mouth_y = 155;


#define MOUTH_FRAME_COUNT 4

// -----------------------------------------------------------------------------
// Runtime
// -----------------------------------------------------------------------------
static TaskHandle_t s_task = NULL;
static volatile bool s_talking = false;
static volatile uint8_t s_target_amplitude = 0;

// -----------------------------------------------------------------------------
// Assets
// -----------------------------------------------------------------------------
static uint16_t *s_base_jpg = NULL;
static uint16_t *s_bg_mouth = NULL;
static uint16_t *s_mouth[MOUTH_FRAME_COUNT] = {NULL};

// -----------------------------------------------------------------------------
// Utils
// -----------------------------------------------------------------------------
static inline size_t rgb565_buf_size(int w, int h)
{
    return (size_t)w * (size_t)h * sizeof(uint16_t);
}

static uint16_t *alloc_rgb565_buf(int w, int h)
{
    return (uint16_t *)heap_caps_malloc(rgb565_buf_size(w, h), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static esp_err_t load_raw_rgb565(const char *path, int w, int h, uint16_t **out_buf)
{
    if (!out_buf) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open fail %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    const size_t expected = rgb565_buf_size(w, h);
    uint16_t *buf = alloc_rgb565_buf(w, h);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "alloc fail %s", path);
        return ESP_ERR_NO_MEM;
    }

    const size_t rd = fread(buf, 1, expected, f);
    fclose(f);

    if (rd != expected) {
        ESP_LOGE(TAG, "read fail %s: got %u expected %u",
                 path, (unsigned)rd, (unsigned)expected);
        free(buf);
        return ESP_FAIL;
    }

    *out_buf = buf;
    ESP_LOGI(TAG, "%s loaded (%u bytes)", path, (unsigned)expected);
    return ESP_OK;
}

static esp_err_t load_base_jpg(void)
{
    FILE *f = fopen("/spiffs/kira_face_360.jpg", "rb");
    if (!f) {
        ESP_LOGE(TAG, "open fail /spiffs/kira_face_360.jpg");
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        ESP_LOGE(TAG, "fseek end fail");
        return ESP_FAIL;
    }

    long jpg_size = ftell(f);
    if (jpg_size <= 0) {
        fclose(f);
        ESP_LOGE(TAG, "jpg size invalid");
        return ESP_FAIL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        ESP_LOGE(TAG, "fseek set fail");
        return ESP_FAIL;
    }

    uint8_t *jpg_buf = (uint8_t *)heap_caps_malloc((size_t)jpg_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg_buf) {
        fclose(f);
        ESP_LOGE(TAG, "alloc jpg buffer fail");
        return ESP_ERR_NO_MEM;
    }

    size_t rd = fread(jpg_buf, 1, (size_t)jpg_size, f);
    fclose(f);

    if (rd != (size_t)jpg_size) {
        free(jpg_buf);
        ESP_LOGE(TAG, "jpg read fail: got %u expected %u", (unsigned)rd, (unsigned)jpg_size);
        return ESP_FAIL;
    }

    s_base_jpg = alloc_rgb565_buf(W, H);
    if (!s_base_jpg) {
        free(jpg_buf);
        ESP_LOGE(TAG, "alloc base jpg rgb565 fail");
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = jpg_buf,
        .indata_size = (size_t)jpg_size,
        .outbuf = (uint8_t *)s_base_jpg,
        .outbuf_size = FB_SIZE_BYTES,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {
            .swap_color_bytes = 1,
        },
    };

    esp_jpeg_image_output_t out = {0};
    esp_err_t ret = esp_jpeg_decode(&cfg, &out);
    free(jpg_buf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg decode fail: %s", esp_err_to_name(ret));
        free(s_base_jpg);
        s_base_jpg = NULL;
        return ret;
    }

    if (out.width != W || out.height != H) {
        ESP_LOGW(TAG, "jpg decoded as %dx%d, expected %dx%d", out.width, out.height, W, H);
    }

    ESP_LOGI(TAG, "loaded JPG background: %dx%d", out.width, out.height);
    return ESP_OK;
}

static void copy_rect_from_base(uint16_t *dst, int x, int y, int w, int h)
{
    for (int row = 0; row < h; row++) {
        memcpy(&dst[row * w], &s_base_jpg[(y + row) * W + x], (size_t)w * sizeof(uint16_t));
    }
}

static void blit_patch(uint16_t *dst_fb, const uint16_t *src, int x, int y, int w, int h)
{
    for (int row = 0; row < h; row++) {
        memcpy(&dst_fb[(y + row) * W + x], &src[row * w], (size_t)w * sizeof(uint16_t));
    }
}

static int mouth_from_amplitude(uint8_t a)
{
    if (a < 20) return 0;  // small
    if (a < 40) return 1;  // medium
    if (a < 70) return 3;  // small2 variation
    return 2;                // wide
}

static void free_assets(void)
{
    if (s_base_jpg) {
        free(s_base_jpg);
        s_base_jpg = NULL;
    }

    if (s_bg_mouth) {
        free(s_bg_mouth);
        s_bg_mouth = NULL;
    }

    for (int i = 0; i < MOUTH_FRAME_COUNT; i++) {
        if (s_mouth[i]) {
            free(s_mouth[i]);
            s_mouth[i] = NULL;
        }
    }
}

static esp_err_t load_assets(void)
{
    esp_err_t ret = load_base_jpg();
    if (ret != ESP_OK) {
        return ret;
    }

    // load prepared mouth background
    ret = load_raw_rgb565("/spiffs/bg_mouth.raw", MOUTH_W, MOUTH_H, &s_bg_mouth);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bg_mouth missing");
        free_assets();
        return ret;
    }

    const char *paths[MOUTH_FRAME_COUNT] = {
        "/spiffs/mouth_1_small.raw",
        "/spiffs/mouth_2_medium.raw",
        "/spiffs/mouth_3_smile.raw",
        "/spiffs/mouth_4_small2.raw",
    };

    for (int i = 0; i < MOUTH_FRAME_COUNT; i++) {
        ESP_LOGI(TAG, "try open: '%s' len=%u", paths[i], (unsigned)strlen(paths[i]));

        ret = load_raw_rgb565(paths[i], MOUTH_W, MOUTH_H, &s_mouth[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mouth patch missing index=%d", i);
            free_assets();
            return ret;
        }
    }

    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Task
// -----------------------------------------------------------------------------
static void face_anim_task(void *arg)
{
    (void)arg;

    uint16_t *fb = display_get_fb();
    if (!fb) {
        ESP_LOGE(TAG, "display_get_fb() returned NULL");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ret = load_assets();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "load_assets failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    memcpy(fb, s_base_jpg, FB_SIZE_BYTES);
    display_flush();

    float smoothed_amp = 0.0f;
    int last_mouth = -1;
    uint32_t frame = 0;

    while (1) {
        frame++;

        // restore background under mouth patch
        blit_patch(fb, s_bg_mouth, s_mouth_x ,s_mouth_y, MOUTH_W, MOUTH_H);

        // smooth amplitude — réponse rapide pour suivre les variations de la voix
        float target = s_talking ? (float)s_target_amplitude : 0.0f;
        smoothed_amp = (smoothed_amp * 0.4f) + (target * 0.6f);

        int mouth_idx = mouth_from_amplitude((uint8_t)smoothed_amp);

        // when not talking, stay closed
        if (!s_talking) {
            blit_patch(fb, s_bg_mouth, s_mouth_x, s_mouth_y, MOUTH_W, MOUTH_H);
    display_flush();
    vTaskDelay(pdMS_TO_TICKS(30));
    continue;
        }

        // Oscillation syllabique : alterne 1 frame vers le bas toutes les ~120 ms
        // pour donner un mouvement visible même quand l'amplitude est stable
        if (mouth_idx > 0 && (frame % 8) >= 4) {
            mouth_idx--;
        }

        if (mouth_idx < 0 || mouth_idx >= MOUTH_FRAME_COUNT) {
            mouth_idx = 0;
        }

        if (s_mouth[mouth_idx] != NULL) {
            blit_patch(fb, s_mouth[mouth_idx], s_mouth_x, s_mouth_y, MOUTH_W, MOUTH_H);
        } else {
            ESP_LOGE(TAG, "s_mouth[%d] NULL", mouth_idx);
        }

        display_flush();

        if (mouth_idx != last_mouth || (frame % 60) == 0) {
            ESP_LOGI(TAG, "frame=%lu talking=%d amp=%u smooth=%u mouth=%d",
                     (unsigned long)frame,
                     s_talking,
                     s_target_amplitude,
                     (unsigned)smoothed_amp,
                     mouth_idx);
            last_mouth = mouth_idx;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
esp_err_t face_anim_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        face_anim_task,
        "face_anim",
        8192,
        NULL,
        3,
        &s_task,
        0
    );

    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Face animation started");
    return ESP_OK;
}

void face_anim_start_talking(void)
{
    s_talking = true;
}

void face_anim_stop_talking(void)
{
    s_talking = false;
    s_target_amplitude = 0;
}

void face_anim_set_amplitude(uint8_t a)
{
    s_target_amplitude = a;
}

void face_anim_set_position(int x, int y)
{
    s_mouth_x = x;
    s_mouth_y = y;

    // If the background is loaded, recalculate the background patch at the correct position
    if (s_base_jpg && s_bg_mouth) {
        copy_rect_from_base(s_bg_mouth, s_mouth_x, s_mouth_y, MOUTH_W, MOUTH_H);
    }

    ESP_LOGI(TAG, "mouth pos set x=%d y=%d", s_mouth_x, s_mouth_y);
}

void face_anim_get_position(int *x, int *y)
{
    if (x) *x = s_mouth_x;
    if (y) *y = s_mouth_y;
}