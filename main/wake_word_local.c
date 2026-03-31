#include "wake_word_local.h"

#include "audio_player.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

/*
 * NOTE IMPORTANT
 * --------------
 * Ce module est conçu pour ESP-SR / WakeNet sur ESP32-S3.
 *
 * Selon la version exacte de esp-sr que tu installes, certains noms de types
 * ou de fonctions peuvent varier légèrement. La structure du module, elle,
 * est la bonne.
 *
 * Il faudra vérifier :
 * - les headers exacts fournis par ta version de esp-sr
 * - le nom du modèle WakeNet custom "Kira"
 * - le chemin des modèles ("model")
 */

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"

static const char *TAG = "wake_word_local";

/* -------------------------------------------------------------------------- */
/* Configuration locale                                                        */
/* -------------------------------------------------------------------------- */

#define WW_SAMPLE_RATE              16000
#define WW_TASK_STACK               8192
#define WW_TASK_PRIO                5
#define WW_TASK_CORE                1

#define WW_EVENT_TRIGGERED          BIT0

/*
 * À adapter si besoin selon le nom exact de ton modèle custom Kira.
 * Exemple possible plus tard :
 *   "wn9_kira"
 *   "wn_custom_kira"
 */
#ifndef WAKE_WORD_MODEL_NAME
#define WAKE_WORD_MODEL_NAME        "wn9_heykira_tts3" //kira
#endif

/*
 * Dossier contenant les modèles ESP-SR embarqués.
 * Adapte si tu les places ailleurs.
 */
#ifndef WAKE_WORD_MODEL_PATH
#define WAKE_WORD_MODEL_PATH        "model"
#endif

/* -------------------------------------------------------------------------- */
/* État interne                                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool initialized;
    bool running;

    TaskHandle_t task;
    EventGroupHandle_t events;
    SemaphoreHandle_t lock;

    srmodel_list_t *models;

    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe_data;

    int feed_chunksize;
    int feed_nch;

    int16_t *mic_frame_mono;
    int16_t *afe_feed_buffer;
} wake_word_ctx_t;

static wake_word_ctx_t s_ctx = {0};

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static void ww_clear_trigger(void)
{
    if (s_ctx.events) {
        xEventGroupClearBits(s_ctx.events, WW_EVENT_TRIGGERED);
    }
}

static void ww_set_trigger(void)
{
    if (s_ctx.events) {
        xEventGroupSetBits(s_ctx.events, WW_EVENT_TRIGGERED);
    }
}

static bool ww_fill_feed_buffer_from_mono(const int16_t *mono, size_t samples)
{
    if (!mono || !s_ctx.afe_feed_buffer || samples != (size_t)s_ctx.feed_chunksize) {
        return false;
    }

    /*
     * audio_player_mic_read() nous donne actuellement un flux mono.
     * L’AFE peut demander plusieurs canaux d’entrée.
     *
     * En première approche, on duplique le mono sur tous les canaux attendus.
     * Ce n’est pas l’optimisation finale idéale, mais c’est une base propre.
     *
     * Plus tard, si tu veux exploiter le multi-mic natif de l’ESP-VoCat,
     * on pourra alimenter directement l’AFE avec les canaux réels.
     */
    for (int i = 0; i < s_ctx.feed_chunksize; i++) {
        for (int ch = 0; ch < s_ctx.feed_nch; ch++) {
            s_ctx.afe_feed_buffer[i * s_ctx.feed_nch + ch] = mono[i];
        }
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* Tâche de détection                                                          */
/* -------------------------------------------------------------------------- */

static void wake_word_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "wake_word task started");

    while (1) {
        if (!s_ctx.running) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!audio_player_mic_is_active()) {
            esp_err_t ret = audio_player_mic_start();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "audio_player_mic_start failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        size_t got = 0;
        esp_err_t ret = audio_player_mic_read(
            s_ctx.mic_frame_mono,
            (size_t)s_ctx.feed_chunksize,
            &got,
            pdMS_TO_TICKS(200)
        );

        if (ret != ESP_OK || got != (size_t)s_ctx.feed_chunksize) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!ww_fill_feed_buffer_from_mono(s_ctx.mic_frame_mono, got)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        s_ctx.afe_iface->feed(s_ctx.afe_data, s_ctx.afe_feed_buffer);

        afe_fetch_result_t *res = s_ctx.afe_iface->fetch(s_ctx.afe_data);
        if (!res) {
            continue;
        }

        /*
         * Selon la version esp-sr, les enums peuvent légèrement varier.
         * Le principe reste :
         * - si wake word détecté, on pose le flag événement.
         */
        if (res->wakeup_state) {
            ESP_LOGI(TAG, "Wake word detected locally");
            ww_set_trigger();

            /*
             * Petit anti-retrigger : on laisse respirer un peu.
             * On stoppera proprement depuis voice_cmd quand le wake word
             * aura été consommé.
             */
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}

/* -------------------------------------------------------------------------- */
/* API publique                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t wake_word_local_init(void)
{
    if (s_ctx.initialized) {
        return ESP_OK;
    }

    s_ctx.events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_ctx.events != NULL, ESP_ERR_NO_MEM, TAG, "events alloc failed");

    s_ctx.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    /*
     * Charge la liste des modèles ESP-SR présents dans WAKE_WORD_MODEL_PATH.
     * Il faudra y placer ton modèle custom Kira.
     */
    s_ctx.models = esp_srmodel_init(WAKE_WORD_MODEL_PATH);
    ESP_RETURN_ON_FALSE(s_ctx.models != NULL, ESP_FAIL, TAG, "esp_srmodel_init failed");

    afe_config_t *afe_cfg = afe_config_init(WAKE_WORD_MODEL_PATH, s_ctx.models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    ESP_RETURN_ON_FALSE(afe_cfg != NULL, ESP_ERR_NO_MEM, TAG, "afe_config_init failed");

    /*
     * Configuration recommandée pour du wake word local simple.
     * À ajuster plus tard selon ton besoin exact.
     */
    afe_cfg->aec_init = false;
    afe_cfg->se_init = true;
    afe_cfg->vad_init = true;
    afe_cfg->wakenet_init = true;
 

    /*
     * Modèle wake word.
     * IMPORTANT: remplace WAKE_WORD_MODEL_NAME par le vrai nom du modèle Kira
     * une fois celui-ci généré/intégré.
     */
    afe_cfg->wakenet_model_name = WAKE_WORD_MODEL_NAME;

    s_ctx.afe_iface = esp_afe_handle_from_config(afe_cfg);
    ESP_RETURN_ON_FALSE(s_ctx.afe_iface != NULL, ESP_FAIL, TAG, "esp_afe_handle_from_config failed");

    s_ctx.afe_data = s_ctx.afe_iface->create_from_config(afe_cfg);
    ESP_RETURN_ON_FALSE(s_ctx.afe_data != NULL, ESP_FAIL, TAG, "afe create_from_config failed");

    s_ctx.feed_chunksize = s_ctx.afe_iface->get_feed_chunksize(s_ctx.afe_data);
    s_ctx.feed_nch = s_ctx.afe_iface->get_feed_channel_num(s_ctx.afe_data);

    ESP_LOGI(TAG, "AFE ready: chunksize=%d, feed_nch=%d", s_ctx.feed_chunksize, s_ctx.feed_nch);

    s_ctx.mic_frame_mono = heap_caps_calloc(
        (size_t)s_ctx.feed_chunksize,
        sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    ESP_RETURN_ON_FALSE(s_ctx.mic_frame_mono != NULL, ESP_ERR_NO_MEM, TAG, "mic_frame alloc failed");

    s_ctx.afe_feed_buffer = heap_caps_calloc(
        (size_t)(s_ctx.feed_chunksize * s_ctx.feed_nch),
        sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    ESP_RETURN_ON_FALSE(s_ctx.afe_feed_buffer != NULL, ESP_ERR_NO_MEM, TAG, "afe feed buffer alloc failed");

    BaseType_t ok = xTaskCreatePinnedToCore(
        wake_word_task,
        "wake_word_task",
        WW_TASK_STACK,
        NULL,
        WW_TASK_PRIO,
        &s_ctx.task,
        WW_TASK_CORE
    );
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "wake task create failed");

    s_ctx.initialized = true;
    s_ctx.running = false;

    ESP_LOGI(TAG, "wake_word_local initialized");
    return ESP_OK;
}

esp_err_t wake_word_local_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    if (xSemaphoreTake(s_ctx.lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ww_clear_trigger();
    s_ctx.running = true;

    xSemaphoreGive(s_ctx.lock);

    ESP_LOGI(TAG, "wake_word_local started");
    return ESP_OK;
}

void wake_word_local_stop(void)
{
    if (!s_ctx.initialized) {
        return;
    }

    if (xSemaphoreTake(s_ctx.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_ctx.running = false;
        ww_clear_trigger();
        xSemaphoreGive(s_ctx.lock);
    }

    ESP_LOGI(TAG, "wake_word_local stopped");
}

bool wake_word_local_wait_for_trigger(TickType_t timeout_ticks)
{
    if (!s_ctx.initialized || !s_ctx.events) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_ctx.events,
        WW_EVENT_TRIGGERED,
        pdTRUE,     /* clear on exit */
        pdFALSE,
        timeout_ticks
    );

    return (bits & WW_EVENT_TRIGGERED) != 0;
}

bool wake_word_local_is_running(void)
{
    return s_ctx.initialized && s_ctx.running;
}

void wake_word_local_deinit(void)
{
    if (!s_ctx.initialized) {
        return;
    }

    s_ctx.running = false;

    if (s_ctx.task) {
        vTaskDelete(s_ctx.task);
        s_ctx.task = NULL;
    }

    if (s_ctx.afe_iface && s_ctx.afe_data) {
        s_ctx.afe_iface->destroy(s_ctx.afe_data);
        s_ctx.afe_data = NULL;
    }

    if (s_ctx.mic_frame_mono) {
        heap_caps_free(s_ctx.mic_frame_mono);
        s_ctx.mic_frame_mono = NULL;
    }

    if (s_ctx.afe_feed_buffer) {
        heap_caps_free(s_ctx.afe_feed_buffer);
        s_ctx.afe_feed_buffer = NULL;
    }

    if (s_ctx.events) {
        vEventGroupDelete(s_ctx.events);
        s_ctx.events = NULL;
    }

    if (s_ctx.lock) {
        vSemaphoreDelete(s_ctx.lock);
        s_ctx.lock = NULL;
    }

    s_ctx.models = NULL;
    s_ctx.afe_iface = NULL;
    s_ctx.initialized = false;

    ESP_LOGI(TAG, "wake_word_local deinitialized");
}
