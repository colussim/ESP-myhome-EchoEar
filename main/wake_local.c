// --- Wake word local with ESP-SR ---
#include "wake_local.h"
#include "esp_log.h"
#include "audio_player.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "driver/i2s_tdm.h"        // for i2s_channel_read
   

// External declarations for main
extern bool main_is_screen_sleeping(void);
extern void main_touch_wake_request(void);

#define TAG "wake_local"
#define WAKE_WORD_MODEL_PATH "model"
#define WAKE_WORD_MODEL_NAME "wn9_heykira_tts3"

static const esp_afe_sr_iface_t *afe_iface = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
static int16_t *mic_frame_mono = NULL;
static int16_t *afe_feed_buffer = NULL;
static int feed_chunksize = 0;
static int feed_nch = 0;


static volatile bool s_wake_enabled = true;
static TaskHandle_t s_feed_task_handle = NULL;



bool wake_local_init(void) {
    ESP_LOGI(TAG, "Initializing wake word (Hey, Kira)...");
 
    // 1. Load models
    srmodel_list_t *models = esp_srmodel_init(WAKE_WORD_MODEL_PATH);
    ESP_RETURN_ON_FALSE(models, false, TAG, "esp_srmodel_init() failed");

    // Debug
    for (int i = 0; i < models->num; i++) {
    ESP_LOGI(TAG, "Model found [%d]: %s", i, models->model_name[i]);
}
 
    // 2. AFE config
    afe_config_t *afe_cfg = afe_config_init("MMNN", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);

    


    ESP_RETURN_ON_FALSE(afe_cfg, false, TAG, "afe_config_init() failed");
 
    afe_cfg->aec_init          = false;
    afe_cfg->se_init           = true;
    afe_cfg->vad_init          = true;
    afe_cfg->wakenet_init      = true;
   // afe_cfg->wakenet_model_name = WAKE_WORD_MODEL_NAME;

    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
if (!wn_name) {
    ESP_LOGE(TAG, "No WakeNet model found in the partition");
    return false;
}
ESP_LOGI(TAG, "Selected WakeNet: %s", wn_name);
afe_cfg->wakenet_model_name = wn_name;
 
    // 3. Create AFE
    afe_iface = esp_afe_handle_from_config(afe_cfg);
    ESP_RETURN_ON_FALSE(afe_iface, false, TAG, "esp_afe_handle_from_config() failed");
 

    afe_data = afe_iface->create_from_config(afe_cfg);
    if (!afe_data) {
        ESP_LOGE(TAG, "create_from_config() returned NULL — check channel format and model");
        return false;
    }
 //   afe_data = afe_iface->create_from_config(afe_cfg);
 //   ESP_RETURN_ON_FALSE(afe_data, false, TAG, "afe create_from_config() failed");
 
    // 4. Allocation buffers in PSRAM
    feed_chunksize = afe_iface->get_feed_chunksize(afe_data);
    feed_nch       = afe_iface->get_feed_channel_num(afe_data);
 
    mic_frame_mono  = heap_caps_calloc(feed_chunksize,          sizeof(int16_t), MALLOC_CAP_SPIRAM);
    afe_feed_buffer = heap_caps_calloc(feed_chunksize * feed_nch, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(mic_frame_mono && afe_feed_buffer, false, TAG, "PSRAM allocation failed");
 
    ESP_LOGI(TAG, "Ready — model: %s, chunksize: %d, channels: %d", WAKE_WORD_MODEL_NAME, feed_chunksize, feed_nch);
    return true;
}



void wake_feed_task(void *arg)
{
    (void)arg;
    s_feed_task_handle = xTaskGetCurrentTaskHandle();

    int16_t *raw4ch = heap_caps_calloc(feed_chunksize * 4, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!raw4ch) {
        ESP_LOGE(TAG, "raw4ch allocation failed");
        vTaskDelete(NULL);
        return;
    }

    audio_player_mic_start();
    vTaskDelay(pdMS_TO_TICKS(100));

    while (1) {

        // If wake disabled — wait without touching I2S
        if (!s_wake_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        i2s_chan_handle_t rx = audio_player_get_rx_handle();
        if (!rx) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx,
                                         raw4ch,
                                         feed_chunksize * 4 * sizeof(int16_t),
                                         &bytes_read,
                                         pdMS_TO_TICKS(100)); // timeout court

        if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_INVALID_ARG) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ret == ESP_OK && bytes_read == feed_chunksize * 4 * sizeof(int16_t)) {
            afe_iface->feed(afe_data, raw4ch);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void wake_fetch_task(void *arg)
{
    (void)arg;

    while (1) {
        // fetch() runs continuously — never stop it
        afe_fetch_result_t *res = afe_iface->fetch(afe_data);

        if (!res) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

      if (s_wake_enabled && res->wakeup_state == WAKENET_DETECTED) {
    ESP_LOGI(TAG, "Wake word detected !");

    // 1. Disable feed via flag — feed_task finishes its current i2s_channel_read
    //    then waits on the flag
    s_wake_enabled = false;

    // 2. Wait for feed_task to be paused (max 300ms = 3 × timeout 100ms)
    vTaskDelay(pdMS_TO_TICKS(300));

    // 3. Notify voice_cmd — the I2S channel is now free
    xEventGroupSetBits(g_wake_event_group, WAKE_WORD_DETECTED_BIT);

    // 4. Wait for command to finish
    EventBits_t bits;
    do {
        bits = xEventGroupWaitBits(g_wake_event_group,
                                   WAKE_WORD_DONE_BIT,
                                   pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(500));
        taskYIELD();
    } while (!(bits & WAKE_WORD_DONE_BIT));

    ESP_LOGI(TAG, "Command finished, resuming listening");

    // 5. Wait for speaker to finish before resuming the mic (shared BCLK/WS pins)
    while (audio_player_is_playing()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    audio_player_mic_start();
    // Wait for the mic to be actually active (max 2 s)
    for (int i = 0; i < 40 && !audio_player_mic_is_active(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(100));  /* I2S stabilization */

    // 6. RReactivate the feed — wake_feed_task resumes automatically
    s_wake_enabled = true;
    ESP_LOGI(TAG, "Écoute wake word reprise");
}

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}