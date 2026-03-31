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

// ===== ES8311 Register Definitions =====
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

// ===== Pre-recorded MP3 files =====
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

// ===== Player state =====
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

// ES7210 I2C address (4-channel ADC for microphones)
#define ES7210_I2C_ADDR    0x40

#define MIC_FRAME_MAX 800
static int16_t s_mic_stereo_buf[MIC_FRAME_MAX * 4];   // TDM 4 channels
static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_play_task = NULL;

static volatile bool s_playing = false;
static volatile bool s_stop_requested = false;
static float s_volume = 0.7f;
static int s_current_sample_rate = 0;
static char s_current_file[64] = "";
static char s_last_error[128] = "";

// ===== ES8311 I2C helpers =====
static esp_err_t es8311_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_es8311_dev, buf, sizeof(buf), -1);
}

static esp_err_t es8311_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_es8311_dev, &reg, 1, val, 1, -1);
}

// ===== ES8311 Initialisation (Slave mode, DAC output) =====
static esp_err_t es8311_codec_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t chip_id = 0;

    // Vérifier la présence du codec
    ret = es8311_read(0xFD, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 not detected on I2C addr 0x%02X", ES8311_I2C_ADDR);
        return ret;
    }
    ESP_LOGI(TAG, "ES8311 detected, chip ID: 0x%02X", chip_id);

    // Reset the codec
    ret |= es8311_write(ES8311_RESET_REG00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(20));
    ret |= es8311_write(ES8311_RESET_REG00, 0x00);
    vTaskDelay(pdMS_TO_TICKS(20));

    // lock configuration - slave mode, MCLK from I2S master
    ret |= es8311_write(ES8311_CLK_MANAGER_REG01, 0x30); // MCLK from pad, multiplexer active
    ret |= es8311_write(ES8311_CLK_MANAGER_REG02, 0x00); // MCLK divider = 1
    ret |= es8311_write(ES8311_CLK_MANAGER_REG03, 0x10); // ADC OSR = 256
    ret |= es8311_write(ES8311_CLK_MANAGER_REG04, 0x10); // DAC OSR = 256
    ret |= es8311_write(ES8311_CLK_MANAGER_REG05, 0x00); // BCLK divider
    ret |= es8311_write(ES8311_CLK_MANAGER_REG06, 0x03); // BCLK/LRCK slave mode
    ret |= es8311_write(ES8311_CLK_MANAGER_REG07, 0x00); // LRCK low counter
    ret |= es8311_write(ES8311_CLK_MANAGER_REG08, 0xFF); // LRCK high counter

    // Format I2S 16-bit Philips standard
    ret |= es8311_write(ES8311_SDPIN_REG09, 0x00);  // SDP_IN:  I2S, 16-bit (0x0C = 24-bit!)
    ret |= es8311_write(ES8311_SDPOUT_REG0A, 0x00); // SDP_OUT: I2S, 16-bit

    // Power-up system analog + digital
    ret |= es8311_write(ES8311_SYSTEM_REG0B, 0x00); // Power up analog
    ret |= es8311_write(ES8311_SYSTEM_REG0C, 0x00); // Power up analog
    ret |= es8311_write(ES8311_SYSTEM_REG10, 0x1F); // DAC power on, VMID, IBIAS, DAC ref
    ret |= es8311_write(ES8311_SYSTEM_REG11, 0x7F); // Full power on

    // VMID & analog reference
    ret |= es8311_write(ES8311_SYSTEM_REG0F, 0x44); // VMID reference select

    // DAC enable
    ret |= es8311_write(ES8311_SYSTEM_REG0D, 0x01); // DAC digital enable
    ret |= es8311_write(ES8311_SYSTEM_REG0E, 0x03); // ADC + DAC dig enable
    ret |= es8311_write(ES8311_SYSTEM_REG12, 0x28); // ADC power on (PGA + modulator)
    ret |= es8311_write(ES8311_SYSTEM_REG13, 0x10); // ADC digital ref + HP filter
    ret |= es8311_write(ES8311_SYSTEM_REG14, 0x1A); // MIC input selected
    ret |= es8311_write(ES8311_ADC_REG17, 0xBF);    // ADC volume 0dB

    // Volume DAC - 0xBF = 0dB, 0x00 = -95.5dB (muet!), 0xFF = +32dB
    ret |= es8311_write(ES8311_DAC_REG32, 0xBF);

    // Activate all clocks
    ret |= es8311_write(ES8311_CLK_MANAGER_REG01, 0x3F);

    // CSM power up
    ret |= es8311_write(ES8311_RESET_REG00, 0x80);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 initialization error");
    } else {
        ESP_LOGI(TAG, "ES8311 initialized in DAC slave mode");
    }
    return ret;
}

// ===== ES7210 (ADC micro array) =====

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

    /* Probe : soft reset */
    ret = es7210_write(0x00, 0xFF);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ES7210 not detected (0x%02X)", ES7210_I2C_ADDR);
        s_es7210_dev = NULL;
        return ESP_OK;
    }
    ESP_LOGI(TAG, "ES7210 detected, initializing...");
    vTaskDelay(pdMS_TO_TICKS(50));

    /*
     * Initialization sequence based on the official Espressif driver
     * (esp-bsp/components/es7210/es7210.c  →  es7210_config_codec)
     */

    /* 1. Software reset + release */
    ret  = es7210_write(0x00, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(20));
    ret |= es7210_write(0x00, 0x32);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 2. Timing init (power-up settling time) */
    ret |= es7210_write(0x09, 0x30);
    ret |= es7210_write(0x0A, 0x30);

    /* 3. HPF for ADC1-4 (remove DC offset) */
    ret |= es7210_write(0x23, 0x2A);
    ret |= es7210_write(0x22, 0x0A);
    ret |= es7210_write(0x21, 0x2A);
    ret |= es7210_write(0x20, 0x0A);

    /* 4. I2S format : Philips I2S, 16-bit, TDM enable
     *    REG11 = format | bit_width (I2S=0x00, 16bit=0x60)
     *    REG12 = 0x02 → TDM 1xFS enable for I2S/LJ mode */
    ret |= es7210_write(0x11, 0x60);
    ret |= es7210_write(0x12, 0x02);

    /* 5. Analog power & VMID */
    ret |= es7210_write(0x40, 0xC3);

    /* 6. MIC bias 2.87 V */
    ret |= es7210_write(0x41, 0x70);
    ret |= es7210_write(0x42, 0x70);

    /* 7. MIC PGA gain 30 dB (enum 0x0E | 0x10 flag) */
    ret |= es7210_write(0x43, 0x1E);
    ret |= es7210_write(0x44, 0x1E);
    ret |= es7210_write(0x45, 0x1E);
    ret |= es7210_write(0x46, 0x1E);

    /* 8. MIC power on */
    ret |= es7210_write(0x47, 0x08);
    ret |= es7210_write(0x48, 0x08);
    ret |= es7210_write(0x49, 0x08);
    ret |= es7210_write(0x4A, 0x08);

    /* 9. Sample rate = 16 kHz, MCLK = 16000 × 256 = 4096000 Hz
     *    Coefficient table lookup: mclk=4096000 lrck=16000
     *    → osr=0x20, adc_div=0x01, dll=1, doubler=1, lrck_h=0x01, lrck_l=0x00 */
    ret |= es7210_write(0x07, 0x20);   /* OSR */
    ret |= es7210_write(0x02, 0xC1);   /* adc_div=1 | doubler<<6=0x40 | dll<<7=0x80 */
    ret |= es7210_write(0x04, 0x01);   /* LRCK divider high */
    ret |= es7210_write(0x05, 0x00);   /* LRCK divider low  */

    /* 10. Power down DLL */
    ret |= es7210_write(0x06, 0x04);

    /* 11. ADC + MIC power ON (0x0F = bias + ADC + PGA powered) */
    ret |= es7210_write(0x4B, 0x0F);
    ret |= es7210_write(0x4C, 0x0F);

    /* 12. Final enable sequence */
    ret |= es7210_write(0x00, 0x71);
    ret |= es7210_write(0x00, 0x41);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 initialization error");
    } else {
        ESP_LOGI(TAG, "ES7210 initiali (4ch ADC, TDM, 16-bit, 16kHz, gain 30dB)");
    }
    return ret;
}

// ===== I2S Initialisation =====
// I2S0 = ES8311 DAC (speaker) - Standard mode TX only
static esp_err_t i2s_speaker_init(uint32_t sample_rate)
{
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_NUM, I2S_ROLE_MASTER);
    tx_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_cfg, &s_i2s_tx, NULL), TAG, "i2s_new_channel TX");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws   = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din  = -1,  // TX only, pas de DIN
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "i2s_init_std TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "i2s_enable TX");

    s_current_sample_rate = (int)sample_rate;
    ESP_LOGI(TAG, "I2S0 speaker (STD TX) @ %lu Hz", (unsigned long)sample_rate);
    return ESP_OK;
}

// I2S1 = ES7210 ADC (micros) - TDM mode RX only, 16 kHz
static esp_err_t i2s_mic_init(void)
{
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_NUM_MIC, I2S_ROLE_MASTER);
    rx_cfg.auto_clear = true;
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
            .mclk = -1,           // MCLK beyond supplied by I2S0
            .bclk = BOARD_I2S_BCLK,
            .ws   = BOARD_I2S_WS,
            .dout = -1,           // ES7210 = ADC, no DOUT
            .din  = BOARD_I2S_DIN,
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_i2s_rx, &tdm_cfg), TAG, "i2s_init_tdm RX");
    // RX not activated here — voice_cmd activates it via audio_player_mic_start()

    ESP_LOGI(TAG, "I2S1 mic (TDM RX) @ 16000 Hz, 4 slots");
    return ESP_OK;
}

static esp_err_t i2s_set_sample_rate(int sample_rate)
{
    if (sample_rate == s_current_sample_rate) return ESP_OK;

    i2s_channel_disable(s_i2s_tx);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    esp_err_t ret = i2s_channel_reconfig_std_clock(s_i2s_tx, &clk_cfg);
    i2s_channel_enable(s_i2s_tx);

    if (ret == ESP_OK) {
        s_current_sample_rate = sample_rate;
        ESP_LOGI(TAG, "I2S sample rate => %d Hz", sample_rate);
    }
    return ret;
}

// ===== GPIO init for amp and codec =====
static void gpio_amp_init(void)
{
    // CODEC_PWR_CTRL - powers the codecs
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_CODEC_PWR) | (1ULL << BOARD_PA_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Turn on the codec
    gpio_set_level(BOARD_CODEC_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Turn on the PA amplifier
    gpio_set_level(BOARD_PA_CTRL, 1);
}

static void amp_enable(bool on)
{
    gpio_set_level(BOARD_PA_CTRL, on ? 1 : 0);
}

// Static buffers to avoid stack overflow (14KB+ on the stack)
static int16_t s_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t s_stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];

// ===== MP3 decoding and playback =====
static void play_mp3_from_buffer(const uint8_t *mp3_buf, size_t mp3_len, const char *file_label)
{
    s_playing = true;
    s_stop_requested = false;
    snprintf(s_current_file, sizeof(s_current_file), "%s", file_label);
    s_last_error[0] = '\0';

    // Mute the microphone during playback AND release the BCLK/WS pins
    // (I2S1 master takes the BCLK/WS GPIOs, preventing I2S0 from driving them)
    s_mic_was_active = s_mic_active;
    bool need_speaker_reinit = false;
    if (s_i2s_rx) {
        i2s_channel_disable(s_i2s_rx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
        s_mic_active = false;
        need_speaker_reinit = true;
        ESP_LOGI(TAG, "I2S1 mic removed (releases BCLK/WS for speaker)");
    }

    // Recreate I2S0 TX to force GPIO BCLK/WS/MCLK rerouting
    if (need_speaker_reinit) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        s_current_sample_rate = 0;
        esp_err_t ret = i2s_speaker_init(44100);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Impossible to recreate I2S0 speaker: %s", esp_err_to_name(ret));
            s_playing = false;
            return;
        }
        ESP_LOGI(TAG, "I2S0 speaker recreated (GPIO rerouted)");
    }

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

        // Amplitude for face animation
        {
            int32_t peak = 0;
            int total = samples * info.channels;
            for (int i = 0; i < total; i++) {
                int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > peak) peak = v;
            }
            face_anim_set_amplitude((uint8_t)(peak * 255 / 32768));
        }

        // Reconfigure I2S if the sample rate changes
        if (first_frame || info.hz != s_current_sample_rate) {
            i2s_set_sample_rate(info.hz);
            first_frame = false;
            ESP_LOGI(TAG, "MP3: %d Hz, %d ch, %d samples", info.hz, info.channels, samples);
        }

        // Convert to interleaved stereo (L+R) with volume
        if (info.channels == 2) {
            // Already stereo, apply volume
            for (int i = 0; i < samples * 2; i++) {
                stereo[i] = (int16_t)(pcm[i] * s_volume);
            }
        } else {
            // Mono → duplicate on L and R
            for (int i = 0; i < samples; i++) {
                int16_t s = (int16_t)(pcm[i] * s_volume);
                stereo[i * 2]     = s; // L
                stereo[i * 2 + 1] = s; // R
            }
        }

        // Write to I2S (interleaved stereo)
        size_t bytes_written = 0;
        size_t bytes_to_write = samples * 2 * sizeof(int16_t); // L+R per sample
        i2s_channel_write(s_i2s_tx, stereo, bytes_to_write, &bytes_written, pdMS_TO_TICKS(1000));
    }

    // End of playback - flush I2S
    vTaskDelay(pdMS_TO_TICKS(100));
    led_anim_stop();
    face_anim_stop_talking();
    s_playing = false;
    s_current_file[0] = '\0';

    // Reactivate the microphone if necessary (recreate I2S1 TDM)
    if (s_mic_was_active) {
        if (i2s_mic_init() == ESP_OK) {
            i2s_channel_enable(s_i2s_rx);
            s_mic_active = true;
            ESP_LOGI(TAG, "Microphone reactivated (I2S1 recreated)");
        }
        s_mic_was_active = false;
    }
}

// ===== Audio playback task =====
static void audio_play_task(void *arg)
{
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
            case CMD_PLAY_BUFFER: {
                if (cmd.buffer && cmd.buf_len > 0) {
                    ESP_LOGI(TAG, "Reading TTS buffer (%zu bytes)", cmd.buf_len);
                    play_mp3_from_buffer(cmd.buffer, cmd.buf_len, "(elevenlabs TTS)");
                    heap_caps_free(cmd.buffer);
                }
                break;
            }
            case CMD_STOP:
                s_stop_requested = true;
                break;
            }
        }
    }
}

// ===== Public API =====
esp_err_t audio_player_init(i2c_master_bus_handle_t i2c_bus)
{
    // GPIOs ampli & codec power
    gpio_amp_init();

    // Utilise le bus I2C partagé
    s_i2c_bus = i2c_bus;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_es8311_dev), TAG, "I2C ES8311");

    // Initialize the ES8311 codec (DAC → speaker)
    ESP_RETURN_ON_ERROR(es8311_codec_init(), TAG, "ES8311 init");

    // Initialize the ES7210 codec (ADC → microphones) if present
    es7210_codec_init();

    // Initialize I2S speaker (44100 Hz by default, will be reconfigured dynamically)
    ESP_RETURN_ON_ERROR(i2s_speaker_init(44100), TAG, "I2S speaker init");

    // I2S mic (TDM) will be initialized lazily on the first mic_start()
    // to save internal DMA memory at startup (display needs it)

    // Command queue & playback task
    s_cmd_queue = xQueueCreate(4, sizeof(audio_cmd_t));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;

    BaseType_t ret = xTaskCreatePinnedToCore(audio_play_task, "audio_play", 16384, NULL, 10, &s_play_task, 1);
    if (ret != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Audio player initialized");
    return ESP_OK;
}

esp_err_t audio_player_play_file(int id)
{
    // Stop current playback
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
    // Wait for playback to finish
    int timeout = 200; // 2 seconds max
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

// ===== API Microphone =====

esp_err_t audio_player_mic_start(void)
{
    if (s_playing) return ESP_ERR_INVALID_STATE;
    if (s_mic_active) return ESP_OK;
    if (!s_es7210_dev) return ESP_ERR_INVALID_STATE;

    // Lazy init : créer I2S1 TDM RX au premier appel
    if (!s_i2s_rx) {
        esp_err_t ret = i2s_mic_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_mic_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // I2S1 TDM RX fonctionne indépendamment de I2S0 TX
    i2s_channel_enable(s_i2s_rx);
    s_mic_active = true;

    ESP_LOGI(TAG, "Microphone activated (TDM 16kHz)");
    return ESP_OK;
}

esp_err_t audio_player_mic_stop(void)
{
    if (!s_mic_active && !s_i2s_rx) return ESP_OK;
    if (s_i2s_rx) {
        i2s_channel_disable(s_i2s_rx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }
    s_mic_active = false;
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

    /* TDM 4 slots × 16-bit = 8 bytes per sample period
     * Reading to_read sample periods → to_read × 4 × sizeof(int16_t) bytes */
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_i2s_rx, s_mic_stereo_buf,
                                      to_read * 4 * sizeof(int16_t),
                                      &bytes_read, timeout);
  

    /* Extract channel 0 (MIC1) from TDM 4-channel interleaved:
     * [CH0, CH1, CH2, CH3, CH0, CH1, CH2, CH3, ...] */
    size_t total_samples = bytes_read / sizeof(int16_t);
    size_t mono_samples = total_samples / 4;
    for (size_t i = 0; i < mono_samples; i++) {
        buf[i] = s_mic_stereo_buf[i * 4];  // Channel 0 = MIC1
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
    // Lecture I2S TDM : 4 canaux * 16 bits = 8 octets par période
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_i2s_rx, s_mic_stereo_buf,
                                     to_read * 4 * sizeof(int16_t),
                                     &bytes_read, timeout);
    if (ret != ESP_OK) return ret;
    // Nombre total d'échantillons lus (tous canaux confondus)
    size_t total_samples = bytes_read / sizeof(int16_t);
    size_t mono_samples = total_samples / 4;
    // Calcul d’énergie pour chaque canal (détection du canal actif)
    int64_t energy[4] = {0};
    for (size_t i = 0; i < mono_samples; i++) {
        for (int ch = 0; ch < 4; ch++) {
            int32_t s = s_mic_stereo_buf[i * 4 + ch];
            energy[ch] += (s * s);
        }
    }
    // Detect channel with the highest energy
    int best_ch = 0;
    for (int ch = 1; ch < 4; ch++) {
        if (energy[ch] > energy[best_ch])
            best_ch = ch;
    }
    // Debug
   /*ESP_LOGI(TAG,
             "Mic energy [CH0..CH3] = %" PRIi64 " %" PRIi64 " %" PRIi64 " %" PRIi64
             " (selected CH%d)",
             energy[0], energy[1], energy[2], energy[3], best_ch);*/

    // Conversion in mono (choice of channel or mixing as needed)
    for (size_t i = 0; i < mono_samples; i++) {
        // Simple extraction of the selected channel:
        buf[i] = s_mic_stereo_buf[i * 4 + best_ch];
        // Option for soft mixing if two microphones are close in energy:
        /* 
        int32_t mixed = s_mic_stereo_buf[i * 4 + 0] + s_mic_stereo_buf[i * 4 + 2];
        buf[i] = mixed / 2;
        */
    }
    *out_samples = mono_samples;
    return ESP_OK;
}



bool audio_player_mic_is_active(void)
{
    return s_mic_active;
}
