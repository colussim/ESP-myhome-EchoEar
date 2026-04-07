#include "audio_player.h"
#include "board.h"
#include "led_anim.h"
#include "face_anim.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

static const char *TAG = "audio";

#define ES8311_RESET_REG00        0x00
#define ES8311_CLK_MANAGER_REG01  0x01
#define ES8311_CLK_MANAGER_REG02  0x02
#define ES8311_CLK_MANAGER_REG03  0x03
#define ES8311_CLK_MANAGER_REG04  0x04
#define ES8311_CLK_MANAGER_REG05  0x05
#define ES8311_CLK_MANAGER_REG06  0x06
#define ES8311_CLK_MANAGER_REG07  0x07
#define ES8311_CLK_MANAGER_REG08  0x08
#define ES8311_SDPIN_REG09        0x09
#define ES8311_SDPOUT_REG0A       0x0A
#define ES8311_SYSTEM_REG0B       0x0B
#define ES8311_SYSTEM_REG0C       0x0C
#define ES8311_SYSTEM_REG0D       0x0D
#define ES8311_SYSTEM_REG0E       0x0E
#define ES8311_SYSTEM_REG0F       0x0F
#define ES8311_SYSTEM_REG10       0x10
#define ES8311_SYSTEM_REG11       0x11
#define ES8311_SYSTEM_REG12       0x12
#define ES8311_SYSTEM_REG13       0x13
#define ES8311_SYSTEM_REG14       0x14
#define ES8311_DAC_REG32          0x32
#define ES8311_ADC_REG17          0x17

static const char *kFiles[] = {
    "/spiffs/msg/Bonsoir.mp3",
    "/spiffs/msg/Bonjour.mp3",
    "/spiffs/msg/Bonjour_Emmanuel.mp3",
    "/spiffs/msg/Bonsoir_Emmanuel.mp3",
    "/spiffs/msg/Bonjour_Veronique.mp3",
    "/spiffs/msg/Bonsoir_Veronique.mp3",
    "/spiffs/msg/comment_je_peux_aider.mp3",
    "/spiffs/msg/fait.mp3",
    "/spiffs/msg/bonne_journee.mp3",
    "/spiffs/msg/oui.mp3",
    "/spiffs/msg/Bonjour-je-suis-kira.mp3",
    "/spiffs/msg/Bonsoir-je-suis-kira.mp3",
    "/spiffs/msg/Desoler.mp3"
};
#define NUM_AUDIO_FILES (sizeof(kFiles) / sizeof(kFiles[0]))

typedef enum {
    CMD_PLAY_FILE,
    CMD_PLAY_BUFFER,
    CMD_STOP,
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t type;
    int file_id;
    uint8_t *buffer;
    size_t buf_len;
} audio_cmd_t;

static i2c_master_dev_handle_t s_es8311_dev = NULL;
static i2c_master_dev_handle_t s_es7210_dev = NULL;
static i2c_master_bus_handle_t s_i2c_bus  = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;
static volatile bool s_mic_active = false;
static volatile bool s_mic_was_active = false;

#define ES7210_I2C_ADDR    0x40

#define MIC_FRAME_MAX 800
static int16_t s_mic_stereo_buf[MIC_FRAME_MAX * 4];
static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_play_task = NULL;

static volatile bool s_playing = false;
static volatile bool s_stop_requested = false;
static float s_volume = 0.7f;
static int s_current_sample_rate = 0;
static char s_current_file[64] = "";
static char s_last_error[128] = "";

static void log_dma_heap(const char *where)
{
    ESP_LOGI(TAG, "%s: free_dma=%u largest_dma=%u free_8bit=%u largest_8bit=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

i2s_chan_handle_t audio_player_get_rx_handle(void)
{
    return s_i2s_rx;
}

static void i2s_destroy_tx(void)
{
    if (s_i2s_tx) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        s_current_sample_rate = 0;
        ESP_LOGI(TAG, "I2S TX destroyed");
    }
}

static void i2s_destroy_rx(void)
{
    if (s_i2s_rx) {
        i2s_channel_disable(s_i2s_rx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
        ESP_LOGI(TAG, "I2S RX destroyed");
    }
    s_mic_active = false;
}

static esp_err_t es8311_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_es8311_dev, buf, sizeof(buf), -1);
}

static esp_err_t es8311_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_es8311_dev, &reg, 1, val, 1, -1);
}

static esp_err_t es8311_codec_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t chip_id = 0;

    ret = es8311_read(0xFD, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 not detected on I2C addr 0x%02X", ES8311_I2C_ADDR);
        return ret;
    }
    ESP_LOGI(TAG, "ES8311 detected, chip ID: 0x%02X", chip_id);

    ret |= es8311_write(ES8311_RESET_REG00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(20));
    ret |= es8311_write(ES8311_RESET_REG00, 0x00);
    vTaskDelay(pdMS_TO_TICKS(20));

    ret |= es8311_write(ES8311_CLK_MANAGER_REG01, 0x30);
    ret |= es8311_write(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= es8311_write(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= es8311_write(ES8311_CLK_MANAGER_REG04, 0x10);
    ret |= es8311_write(ES8311_CLK_MANAGER_REG05, 0x00);
    ret |= es8311_write(ES8311_CLK_MANAGER_REG06, 0x03);
    ret |= es8311_write(ES8311_CLK_MANAGER_REG07, 0x00);
    ret |= es8311_write(ES8311_CLK_MANAGER_REG08, 0xFF);

    ret |= es8311_write(ES8311_SDPIN_REG09, 0x00);
    ret |= es8311_write(ES8311_SDPOUT_REG0A, 0x00);

    ret |= es8311_write(ES8311_SYSTEM_REG0B, 0x00);
    ret |= es8311_write(ES8311_SYSTEM_REG0C, 0x00);
    ret |= es8311_write(ES8311_SYSTEM_REG10, 0x1F);
    ret |= es8311_write(ES8311_SYSTEM_REG11, 0x7F);
    ret |= es8311_write(ES8311_SYSTEM_REG0F, 0x44);
    ret |= es8311_write(ES8311_SYSTEM_REG0D, 0x01);
    ret |= es8311_write(ES8311_SYSTEM_REG0E, 0x03);
    ret |= es8311_write(ES8311_SYSTEM_REG12, 0x28);
    ret |= es8311_write(ES8311_SYSTEM_REG13, 0x10);
    ret |= es8311_write(ES8311_SYSTEM_REG14, 0x1A);
    ret |= es8311_write(ES8311_ADC_REG17, 0xBF);
    ret |= es8311_write(ES8311_DAC_REG32, 0xBF);
    ret |= es8311_write(ES8311_CLK_MANAGER_REG01, 0x3F);
    ret |= es8311_write(ES8311_RESET_REG00, 0x80);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 initialization error");
    } else {
        ESP_LOGI(TAG, "ES8311 initialized in DAC slave mode");
    }
    return ret;
}

static esp_err_t es7210_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_es7210_dev, buf, sizeof(buf), -1);
}

static esp_err_t es7210_codec_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES7210_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_es7210_dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add ES7210 to I2C bus");
        return ESP_OK;
    }

    ret = es7210_write(0x00, 0xFF);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ES7210 not detected (0x%02X)", ES7210_I2C_ADDR);
        s_es7210_dev = NULL;
        return ESP_OK;
    }
    ESP_LOGI(TAG, "ES7210 detected, initializing...");
    vTaskDelay(pdMS_TO_TICKS(50));

    ret  = es7210_write(0x00, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(20));
    ret |= es7210_write(0x00, 0x32);
    vTaskDelay(pdMS_TO_TICKS(20));
    ret |= es7210_write(0x09, 0x30);
    ret |= es7210_write(0x0A, 0x30);
    ret |= es7210_write(0x23, 0x2A);
    ret |= es7210_write(0x22, 0x0A);
    ret |= es7210_write(0x21, 0x2A);
    ret |= es7210_write(0x20, 0x0A);
    ret |= es7210_write(0x11, 0x60);
    ret |= es7210_write(0x12, 0x02);
    ret |= es7210_write(0x40, 0xC3);
    ret |= es7210_write(0x41, 0x70);
    ret |= es7210_write(0x42, 0x70);
    ret |= es7210_write(0x43, 0x1E);
    ret |= es7210_write(0x44, 0x1E);
    ret |= es7210_write(0x45, 0x1E);
    ret |= es7210_write(0x46, 0x1E);
    ret |= es7210_write(0x47, 0x08);
    ret |= es7210_write(0x48, 0x08);
    ret |= es7210_write(0x49, 0x08);
    ret |= es7210_write(0x4A, 0x08);
    ret |= es7210_write(0x07, 0x20);
    ret |= es7210_write(0x02, 0xC1);
    ret |= es7210_write(0x04, 0x01);
    ret |= es7210_write(0x05, 0x00);
    ret |= es7210_write(0x06, 0x04);
    ret |= es7210_write(0x4B, 0x0F);
    ret |= es7210_write(0x4C, 0x0F);
    ret |= es7210_write(0x00, 0x71);
    ret |= es7210_write(0x00, 0x41);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 initialization error");
    } else {
        ESP_LOGI(TAG, "ES7210 initiali (4ch ADC, TDM, 16-bit, 16kHz, gain 30dB)");
    }
    return ret;
}

static esp_err_t i2s_speaker_init(uint32_t sample_rate)
{
    log_dma_heap("before TX init");

    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_NUM, I2S_ROLE_MASTER);
    tx_cfg.auto_clear    = true;
    tx_cfg.dma_desc_num  = 6;   // 6 × 256 × 2ch × 2B = 6144 bytes — avoids DMA underrun crackle
    tx_cfg.dma_frame_num = 256; // larger ring buffer = smoother playback
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_cfg, &s_i2s_tx, NULL), TAG, "i2s_new_channel TX");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws   = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din  = -1,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    esp_err_t ret = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (ret != ESP_OK) {
        i2s_destroy_tx();
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_tx);
    if (ret != ESP_OK) {
        i2s_destroy_tx();
        return ret;
    }

    s_current_sample_rate = (int)sample_rate;
    log_dma_heap("after TX init");
    ESP_LOGI(TAG, "I2S0 speaker (STD TX) @ %lu Hz", (unsigned long)sample_rate);
    return ESP_OK;
}

static esp_err_t i2s_mic_init(void)
{
    log_dma_heap("before RX init");

    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_NUM_MIC, I2S_ROLE_MASTER);
    rx_cfg.auto_clear = true;
    rx_cfg.dma_desc_num  = 6;   // 6 × 128 × 4ch × 2B = 6144 bytes DMA
    rx_cfg.dma_frame_num = 128; // ring = 768 samples — enough for AFE feed at 16kHz
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_cfg, NULL, &s_i2s_rx), TAG, "i2s_new_channel RX");

    i2s_tdm_config_t tdm_cfg = {
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .clk_cfg = {
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .sample_rate_hz = 16000,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .gpio_cfg = {
            .mclk = -1,
            .bclk = BOARD_I2S_BCLK,
            .ws   = BOARD_I2S_WS,
            .dout = -1,
            .din  = BOARD_I2S_DIN,
        },
    };

    esp_err_t ret = i2s_channel_init_tdm_mode(s_i2s_rx, &tdm_cfg);
    if (ret != ESP_OK) {
        i2s_destroy_rx();
        return ret;
    }

    log_dma_heap("after RX init");
    ESP_LOGI(TAG, "I2S1 mic (TDM RX) @ 16000 Hz, 4 slots");
    return ESP_OK;
}

static esp_err_t i2s_set_sample_rate(int sample_rate)
{
    if (!s_i2s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate == s_current_sample_rate) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_channel_disable(s_i2s_tx);
    if (ret != ESP_OK) {
        return ret;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ret = i2s_channel_reconfig_std_clock(s_i2s_tx, &clk_cfg);
    if (ret != ESP_OK) {
        (void)i2s_channel_enable(s_i2s_tx);
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_tx);
    if (ret == ESP_OK) {
        s_current_sample_rate = sample_rate;
        ESP_LOGI(TAG, "I2S sample rate => %d Hz", sample_rate);
    }
    return ret;
}

static void gpio_amp_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_CODEC_PWR) | (1ULL << BOARD_PA_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(BOARD_CODEC_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BOARD_PA_CTRL, 1);
}

static void amp_enable(bool on)
{
    gpio_set_level(BOARD_PA_CTRL, on ? 1 : 0);
}

static int16_t s_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t s_stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];

#define WAV_CHUNK_SAMPLES 512
static int16_t s_wav_stereo[WAV_CHUNK_SAMPLES * 2];

// ===== WAV PCM playback — reads sample rate from header (Piper TTS = 22050 Hz mono 16-bit) =====
static void play_wav_from_buffer(const uint8_t *buf, size_t len, const char *label)
{
    if (len < 44 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "WAV: invalid header");
        return;
    }

    uint16_t num_channels    = 0;
    uint32_t sample_rate     = 0;
    uint16_t bits_per_sample = 0;
    memcpy(&num_channels,    buf + 22, 2);
    memcpy(&sample_rate,     buf + 24, 4);
    memcpy(&bits_per_sample, buf + 34, 2);

    if (bits_per_sample != 16) {
        ESP_LOGE(TAG, "WAV: only 16-bit PCM supported (got %d)", bits_per_sample);
        return;
    }

    // Scan for "data" chunk (handles non-standard fmt sizes)
    size_t pos = 12, data_offset = 0;
    uint32_t data_size = 0;
    while (pos + 8 <= len) {
        uint32_t csz = 0;
        memcpy(&csz, buf + pos + 4, 4);
        if (memcmp(buf + pos, "data", 4) == 0) {
            data_offset = pos + 8;
            data_size   = csz;
            break;
        }
        pos += 8 + csz;
    }
    if (!data_size || !data_offset || data_offset >= len) {
        ESP_LOGE(TAG, "WAV: data chunk not found");
        return;
    }

    ESP_LOGI(TAG, "WAV: %lu Hz, %d ch, %lu bytes",
             (unsigned long)sample_rate, num_channels, (unsigned long)data_size);

    s_playing = true;
    s_stop_requested = false;
    snprintf(s_current_file, sizeof(s_current_file), "%s", label);
    s_last_error[0] = '\0';

    s_mic_was_active = s_mic_active;
    if (s_i2s_rx) {
        i2s_destroy_rx();
        ESP_LOGI(TAG, "I2S1 mic removed");
    }

    // Always recreate TX to force GPIO re-registration of BCLK/WS (stolen by I2S1 mic)
    i2s_destroy_tx();
    {
        esp_err_t ret = i2s_speaker_init(sample_rate);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WAV: speaker init failed: %s", esp_err_to_name(ret));
            s_playing = false;
            if (s_mic_was_active) { audio_player_mic_start(); s_mic_was_active = false; }
            return;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // let DMA ring fill with silence before amp opens

    amp_enable(true);
    led_anim_start();
    face_anim_start_talking();

    const int16_t *pcm  = (const int16_t *)(buf + data_offset);
    size_t total_frames = data_size / (num_channels * sizeof(int16_t));

    for (size_t off = 0; off < total_frames && !s_stop_requested; ) {
        size_t chunk = total_frames - off;
        if (chunk > WAV_CHUNK_SAMPLES) chunk = WAV_CHUNK_SAMPLES;

        if (num_channels == 1) {
            for (size_t i = 0; i < chunk; i++) {
                int16_t s = (int16_t)(pcm[off + i] * s_volume);
                s_wav_stereo[i * 2]     = s;
                s_wav_stereo[i * 2 + 1] = s;
            }
        } else {
            for (size_t i = 0; i < chunk; i++) {
                s_wav_stereo[i * 2]     = (int16_t)(pcm[(off + i) * 2]     * s_volume);
                s_wav_stereo[i * 2 + 1] = (int16_t)(pcm[(off + i) * 2 + 1] * s_volume);
            }
        }
        size_t written = 0;
        i2s_channel_write(s_i2s_tx, s_wav_stereo, chunk * 2 * sizeof(int16_t),
                          &written, pdMS_TO_TICKS(1000));
        off += chunk;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    amp_enable(false);
    led_anim_stop();
    face_anim_stop_talking();
    s_playing = false;
    s_current_file[0] = '\0';

    if (s_mic_was_active) {
        audio_player_mic_start();
        s_mic_was_active = false;
    }
}

static void play_mp3_from_buffer(const uint8_t *mp3_buf, size_t mp3_len, const char *file_label)
{
    s_playing = true;
    s_stop_requested = false;
    snprintf(s_current_file, sizeof(s_current_file), "%s", file_label);
    s_last_error[0] = '\0';

    s_mic_was_active = s_mic_active;
    if (s_i2s_rx) {
        // Destroy mic RX first — releases BCLK/WS from I2S1's GPIO matrix routing
        i2s_destroy_rx();
        ESP_LOGI(TAG, "I2S1 mic removed");
    }

    // Always recreate TX: when I2S1 was active it stole BCLK/WS (GPIO39/40) from
    // I2S0 in the GPIO matrix. Even though TX handle still exists, those pins are
    // no longer wired to I2S0 — we must destroy+recreate to force GPIO re-registration.
    i2s_destroy_tx();
    {
        esp_err_t ret = i2s_speaker_init(44100);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Impossible to create I2S0 speaker: %s", esp_err_to_name(ret));
            snprintf(s_last_error, sizeof(s_last_error), "speaker init failed: %s", esp_err_to_name(ret));
            s_playing = false;
            return;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // let DMA ring fill with silence before amp opens

    amp_enable(true);
    led_anim_start();
    face_anim_start_talking();

    static mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    int16_t *pcm = s_pcm;
    int16_t *stereo = s_stereo;
    mp3dec_frame_info_t info;
    size_t offset = 0;
    bool first_frame = true;

    while (offset < mp3_len && !s_stop_requested) {
        int samples = mp3dec_decode_frame(&mp3d, mp3_buf + offset, mp3_len - offset, pcm, &info);

        if (info.frame_bytes == 0) {
            offset++;
            if (offset >= mp3_len) break;
            continue;
        }
        offset += info.frame_bytes;

        if (samples <= 0) continue;

        int32_t peak = 0;
        int total = samples * info.channels;
        for (int i = 0; i < total; i++) {
            int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (v > peak) peak = v;
        }
        face_anim_set_amplitude((uint8_t)(peak * 255 / 32768));

        if (first_frame || info.hz != s_current_sample_rate) {
            if (i2s_set_sample_rate(info.hz) != ESP_OK) {
                snprintf(s_last_error, sizeof(s_last_error), "sample rate switch failed");
                break;
            }
            first_frame = false;
            ESP_LOGI(TAG, "MP3: %d Hz, %d ch, %d samples", info.hz, info.channels, samples);
        }

        if (info.channels == 2) {
            for (int i = 0; i < samples * 2; i++) {
                stereo[i] = (int16_t)(pcm[i] * s_volume);
            }
        } else {
            for (int i = 0; i < samples; i++) {
                int16_t s = (int16_t)(pcm[i] * s_volume);
                stereo[i * 2] = s;
                stereo[i * 2 + 1] = s;
            }
        }

        size_t bytes_written = 0;
        size_t bytes_to_write = samples * 2 * sizeof(int16_t);
        esp_err_t ret = i2s_channel_write(s_i2s_tx, stereo, bytes_to_write, &bytes_written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            snprintf(s_last_error, sizeof(s_last_error), "i2s write failed: %s", esp_err_to_name(ret));
            ESP_LOGE(TAG, "%s", s_last_error);
            break;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    led_anim_stop();
    face_anim_stop_talking();
    s_playing = false;
    s_current_file[0] = '\0';

    if (s_mic_was_active) {
        esp_err_t ret = audio_player_mic_start();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Microphone reactivated");
        } else {
            ESP_LOGE(TAG, "Microphone reactivation failed: %s", esp_err_to_name(ret));
        }
        s_mic_was_active = false;
    }
}

static void audio_play_task(void *arg)
{
    (void)arg;
    audio_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch (cmd.type) {
            case CMD_PLAY_FILE: {
                if (cmd.file_id < 1 || (size_t)cmd.file_id > NUM_AUDIO_FILES) {
                    snprintf(s_last_error, sizeof(s_last_error), "Invalid ID: %d", cmd.file_id);
                    break;
                }
                const char *path = kFiles[cmd.file_id - 1];
                FILE *f = fopen(path, "rb");
                if (!f) {
                    snprintf(s_last_error, sizeof(s_last_error), "File not found: %s", path);
                    ESP_LOGE(TAG, "%s", s_last_error);
                    break;
                }
                fseek(f, 0, SEEK_END);
                size_t fsize = ftell(f);
                fseek(f, 0, SEEK_SET);

                uint8_t *buf = heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!buf) {
                    snprintf(s_last_error, sizeof(s_last_error), "PSRAM alloc failed: %zu bytes", fsize);
                    fclose(f);
                    break;
                }

                size_t read = fread(buf, 1, fsize, f);
                fclose(f);
                ESP_LOGI(TAG, "Reading %s (%zu bytes)", path, read);
                play_mp3_from_buffer(buf, read, path);
                heap_caps_free(buf);
                break;
            }
            case CMD_PLAY_BUFFER:
                if (cmd.buffer && cmd.buf_len > 0) {
                    bool is_wav = cmd.buf_len >= 12 &&
                                  memcmp(cmd.buffer,     "RIFF", 4) == 0 &&
                                  memcmp(cmd.buffer + 8, "WAVE", 4) == 0;
                    if (is_wav) {
                        ESP_LOGI(TAG, "Playing WAV TTS (%zu bytes)", cmd.buf_len);
                        play_wav_from_buffer(cmd.buffer, cmd.buf_len, "(piper TTS)");
                    } else {
                        ESP_LOGI(TAG, "Playing MP3 TTS (%zu bytes)", cmd.buf_len);
                        play_mp3_from_buffer(cmd.buffer, cmd.buf_len, "(TTS)");
                    }
                    heap_caps_free(cmd.buffer);
                }
                break;
            case CMD_STOP:
                s_stop_requested = true;
                break;
            }
        }
    }
}

esp_err_t audio_player_init(i2c_master_bus_handle_t i2c_bus)
{
    gpio_amp_init();
    s_i2c_bus = i2c_bus;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_es8311_dev), TAG, "I2C ES8311");
    ESP_RETURN_ON_ERROR(es8311_codec_init(), TAG, "ES8311 init");

    es7210_codec_init();

    // Initialize I2S0 TX at boot — REQUIRED to drive MCLK (GPIO42) for ES7210 ADC.
    // Without MCLK, the ES7210 has no clock source and the microphone is silent.
    // Using minimal DMA (512 bytes) to preserve DMA budget for the mic RX channel.
    ESP_RETURN_ON_ERROR(i2s_speaker_init(44100), TAG, "I2S speaker init (MCLK)");

    s_cmd_queue = xQueueCreate(4, sizeof(audio_cmd_t));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;

    BaseType_t ret = xTaskCreatePinnedToCore(audio_play_task, "audio_play", 16384, NULL, 10, &s_play_task, 1);
    if (ret != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Audio player initialized");
    return ESP_OK;
}

esp_err_t audio_player_play_file(int id)
{
    if (!s_cmd_queue) return ESP_ERR_INVALID_STATE;
    if (s_playing) {
        audio_player_stop();
    }
    audio_cmd_t cmd = { .type = CMD_PLAY_FILE, .file_id = id };
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_player_play_buffer(uint8_t *buf, size_t len)
{
    if (!s_cmd_queue) return ESP_ERR_INVALID_STATE;
    if (s_playing) {
        audio_player_stop();
    }
    audio_cmd_t cmd = { .type = CMD_PLAY_BUFFER, .buffer = buf, .buf_len = len };
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void audio_player_stop(void)
{
    if (!s_playing) return;
    s_stop_requested = true;
    int timeout = 200;
    while (s_playing && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void audio_player_set_volume(float vol)
{
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_volume = vol;
}

bool audio_player_is_playing(void)
{
    return s_playing;
}

float audio_player_get_volume(void)
{
    return s_volume;
}

const char *audio_player_current_file(void)
{
    return s_current_file;
}

const char *audio_player_last_error(void)
{
    return s_last_error;
}

esp_err_t audio_player_mic_start(void)
{
    if (s_playing) return ESP_ERR_INVALID_STATE;
    if (s_mic_active) return ESP_OK;
    if (!s_es7210_dev) return ESP_ERR_INVALID_STATE;

    // Ensure TX is alive to provide MCLK (GPIO42) to the ES7210 ADC.
    // TX must be running before RX is created — ES7210 needs MCLK to latch its clock.
    if (!s_i2s_tx) {
        esp_err_t ret = i2s_speaker_init(44100);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "mic_start: cannot init TX for MCLK: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "TX recreated for MCLK before mic init");
    }

    if (!s_i2s_rx) {
        esp_err_t ret = i2s_mic_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_mic_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    esp_err_t ret = i2s_channel_enable(s_i2s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable RX failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_mic_active = true;
    ESP_LOGI(TAG, "Microphone activated (TDM 16kHz)");
    return ESP_OK;
}

esp_err_t audio_player_mic_stop(void)
{
    if (!s_mic_active && !s_i2s_rx) return ESP_OK;
    i2s_destroy_rx();
    ESP_LOGI(TAG, "Microphone deactivated (I2S1 deleted)");
    return ESP_OK;
}

esp_err_t audio_player_mic_read2(int16_t *buf, size_t max_samples,
                                 size_t *out_samples, TickType_t timeout)
{
    *out_samples = 0;
    if (!s_mic_active || s_playing) return ESP_ERR_INVALID_STATE;

    size_t to_read = max_samples;
    if (to_read > MIC_FRAME_MAX) to_read = MIC_FRAME_MAX;

    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_i2s_rx, s_mic_stereo_buf,
                                     to_read * 4 * sizeof(int16_t),
                                     &bytes_read, timeout);
    if (ret != ESP_OK) return ret;

    size_t total_samples = bytes_read / sizeof(int16_t);
    size_t mono_samples = total_samples / 4;
    for (size_t i = 0; i < mono_samples; i++) {
        buf[i] = s_mic_stereo_buf[i * 4];
    }
    *out_samples = mono_samples;
    return ESP_OK;
}

esp_err_t audio_player_mic_read(int16_t *buf, size_t max_samples,
                                size_t *out_samples, TickType_t timeout)
{
    *out_samples = 0;
    if (!s_mic_active || s_playing) return ESP_ERR_INVALID_STATE;

    size_t to_read = max_samples;
    if (to_read > MIC_FRAME_MAX) to_read = MIC_FRAME_MAX;

    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_i2s_rx, s_mic_stereo_buf,
                                     to_read * 4 * sizeof(int16_t),
                                     &bytes_read, timeout);
    if (ret != ESP_OK) return ret;

    size_t total_samples = bytes_read / sizeof(int16_t);
    size_t mono_samples = total_samples / 4;

    int64_t energy[4] = {0};
    for (size_t i = 0; i < mono_samples; i++) {
        for (int ch = 0; ch < 4; ch++) {
            int32_t s = s_mic_stereo_buf[i * 4 + ch];
            energy[ch] += (s * s);
        }
    }

    int best_ch = 0;
    for (int ch = 1; ch < 4; ch++) {
        if (energy[ch] > energy[best_ch]) {
            best_ch = ch;
        }
    }

    for (size_t i = 0; i < mono_samples; i++) {
        buf[i] = s_mic_stereo_buf[i * 4 + best_ch];
    }
    *out_samples = mono_samples;
    return ESP_OK;
}

bool audio_player_mic_is_active(void)
{
    return s_mic_active;
}
