#include "voice_cmd.h"
#include "config.h"
#include "audio_player.h"
#include "ha_client.h"
#include "led_anim.h"
#include "face_anim.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "voice_cmd";

#define SAMPLE_RATE         16000
#define FRAME_MS            50
#define FRAME_SAMPLES       (SAMPLE_RATE * FRAME_MS / 1000)   /* 800 */
#define MAX_RECORD_S        3
#define MAX_SAMPLES         (SAMPLE_RATE * MAX_RECORD_S)

#define PREROLL_MS          500
#define POSTROLL_MS         100
#define PREROLL_SAMPLES     (SAMPLE_RATE * PREROLL_MS / 1000) /* 4000 */
#define POSTROLL_FRAMES     (POSTROLL_MS / FRAME_MS)          /* 2 */

#define SPEECH_FRAMES       4    /* 200 ms speech => start */
#define SILENCE_FRAMES      10   /* 500 ms silence => stop */

#define FOLLOWUP_TIMEOUT    200  /* 10 s before speech starts */
#define BACKEND_QUEUE_LEN   2

typedef enum {
    VOICE_STATE_WAIT_WAKE = 0,
    VOICE_STATE_WAIT_COMMAND
} voice_state_t;

typedef struct {
    int16_t *pcm;
    size_t samples;
    char *result;
    size_t result_len;
    bool ok;
    SemaphoreHandle_t done;
} backend_request_t;

typedef struct {
    bool valid;
    bool status_success;
    bool kiraactive;
    bool ha_ok;
    char status[32];
    char heard[256];
    char reply[256];
    char ha_ack[64];
    char category[32];
} backend_result_t;

static QueueHandle_t s_backend_queue = NULL;
static TaskHandle_t s_task = NULL;
static voice_state_t s_state = VOICE_STATE_WAIT_WAKE;

/* -------------------------------------------------------------------------- */
/* WAV helpers                                                                */
/* -------------------------------------------------------------------------- */

static void write_wav_header(uint8_t *h, uint32_t num_samples, uint32_t sr)
{
    uint32_t data_sz = num_samples * 2;
    uint32_t file_sz = 36 + data_sz;
    uint32_t fmt_sz  = 16;
    uint16_t pcm     = 1;
    uint16_t ch      = 1;
    uint32_t brate   = sr * 2;
    uint16_t balign  = 2;
    uint16_t bps     = 16;

    memcpy(h,      "RIFF", 4);  memcpy(h + 4,  &file_sz, 4);
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);  memcpy(h + 16, &fmt_sz, 4);
    memcpy(h + 20, &pcm, 2);    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &sr, 4);     memcpy(h + 28, &brate, 4);
    memcpy(h + 32, &balign, 2); memcpy(h + 34, &bps, 2);
    memcpy(h + 36, "data", 4);  memcpy(h + 40, &data_sz, 4);
}

/* -------------------------------------------------------------------------- */
/* Generic helpers                                                            */
/* -------------------------------------------------------------------------- */

static void play_and_wait(int file_id)
{
    audio_player_play_file(file_id);
    while (audio_player_is_playing()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void ensure_mic_running(void)
{
    while (audio_player_is_playing()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!audio_player_mic_is_active()) {
        esp_err_t ret = audio_player_mic_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio_player_mic_start failed: %s", esp_err_to_name(ret));
        }
    }
}

static void ensure_mic_stopped(void)
{
    if (audio_player_mic_is_active()) {
        audio_player_mic_stop();
    }
}

/* -------------------------------------------------------------------------- */
/* Backend JSON parsing                                                       */
/* -------------------------------------------------------------------------- */

static void backend_result_init(backend_result_t *r)
{
    if (!r) {
        return;
    }
    memset(r, 0, sizeof(*r));
}

static bool parse_backend_json(const char *json_text, backend_result_t *out)
{
    if (!json_text || !json_text[0] || !out) {
        return false;
    }

    backend_result_init(out);

    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        ESP_LOGW(TAG, "Backend invalid JSON: %.300s", json_text);
        return false;
    }

    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    cJSON *heard = cJSON_GetObjectItemCaseSensitive(root, "heard");
    cJSON *kiraactive = cJSON_GetObjectItemCaseSensitive(root, "kiraactive");
    cJSON *reply = cJSON_GetObjectItemCaseSensitive(root, "reply");
    cJSON *ha_ack = cJSON_GetObjectItemCaseSensitive(root, "ha_ack");
    cJSON *category = cJSON_GetObjectItemCaseSensitive(root, "category");

    if (cJSON_IsString(status) && status->valuestring) {
        strncpy(out->status, status->valuestring, sizeof(out->status) - 1);
        out->status_success = (strcmp(out->status, "success") == 0);
    }

    if (cJSON_IsString(heard) && heard->valuestring) {
        strncpy(out->heard, heard->valuestring, sizeof(out->heard) - 1);
    }

    if (cJSON_IsBool(kiraactive)) {
        out->kiraactive = cJSON_IsTrue(kiraactive);
    }

    if (cJSON_IsString(reply) && reply->valuestring) {
        strncpy(out->reply, reply->valuestring, sizeof(out->reply) - 1);
    }

    if (cJSON_IsString(ha_ack) && ha_ack->valuestring) {
        strncpy(out->ha_ack, ha_ack->valuestring, sizeof(out->ha_ack) - 1);
        out->ha_ok = (strcmp(out->ha_ack, "ok") == 0);
    }

    if (cJSON_IsString(category) && category->valuestring) {
        strncpy(out->category, category->valuestring, sizeof(out->category) - 1);
    }

    out->valid = true;
    cJSON_Delete(root);

    ESP_LOGI(TAG,
             "Backend parsed: status=%s category=%s kiraactive=%d ha_ack=%s heard=\"%s\" reply=\"%s\"",
             out->status[0] ? out->status : "(none)",
             out->category[0] ? out->category : "(none)",
             out->kiraactive,
             out->ha_ack[0] ? out->ha_ack : "(none)",
             out->heard,
             out->reply);

    return true;
}

/* -------------------------------------------------------------------------- */
/* Backend call                                                               */
/* -------------------------------------------------------------------------- */

static bool send_to_backend(const int16_t *pcm_data, size_t num_samples,
                            char *out_json, size_t json_len)
{
    if (!pcm_data || num_samples == 0 || !out_json || json_len == 0) {
        return false;
    }

    out_json[0] = '\0';

    float dur = (float)num_samples / SAMPLE_RATE;
    ESP_LOGI(TAG, "Sending to backend: %.1f s, %zu samples", dur, num_samples);

    size_t wav_len = 44 + num_samples * sizeof(int16_t);
    uint8_t *wav = heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM);
    if (!wav) {
        ESP_LOGE(TAG, "WAV alloc failed");
        return false;
    }

    write_wav_header(wav, (uint32_t)num_samples, SAMPLE_RATE);
    memcpy(wav + 44, pcm_data, num_samples * sizeof(int16_t));

    FILE *f = fopen("/spiffs/last.wav", "wb");
    if (f) {
        fwrite(wav, 1, wav_len, f);
        fclose(f);
        ESP_LOGI(TAG, "Audio dumped to /spiffs/last.wav (%u bytes)", (unsigned)wav_len);
    }

    ha_stt_result_t stt;
    esp_err_t ret = ha_stt_recognize(wav, wav_len, &stt);
    heap_caps_free(wav);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backend network/API error: %s", esp_err_to_name(ret));
        return false;
    }

    if (!stt.success) {
        ESP_LOGW(TAG, "Backend returned failure");
        return false;
    }

    strncpy(out_json, stt.text, json_len - 1);
    out_json[json_len - 1] = '\0';

    ESP_LOGI(TAG, "Backend raw: %.400s", out_json);
    return (out_json[0] != '\0');
}

static bool backend_call_async(const int16_t *src_pcm, size_t samples,
                               char *out_json, size_t out_json_len)
{
    if (!src_pcm || samples == 0 || !out_json || out_json_len == 0) {
        return false;
    }

    out_json[0] = '\0';

    backend_request_t *req = heap_caps_malloc(sizeof(backend_request_t), MALLOC_CAP_SPIRAM);
    if (!req) {
        ESP_LOGE(TAG, "Backend request alloc failed");
        return false;
    }

    memset(req, 0, sizeof(*req));

    req->pcm = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!req->pcm) {
        ESP_LOGE(TAG, "Backend PCM alloc failed");
        heap_caps_free(req);
        return false;
    }

    memcpy(req->pcm, src_pcm, samples * sizeof(int16_t));
    req->samples = samples;
    req->result = out_json;
    req->result_len = out_json_len;
    req->ok = false;
    req->done = xSemaphoreCreateBinary();

    if (!req->done) {
        ESP_LOGE(TAG, "Backend semaphore alloc failed");
        heap_caps_free(req->pcm);
        heap_caps_free(req);
        return false;
    }

    ESP_LOGI(TAG, "-> Queueing backend request (%zu samples)", samples);

    if (xQueueSend(s_backend_queue, &req, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Backend queue full");
        vSemaphoreDelete(req->done);
        heap_caps_free(req->pcm);
        heap_caps_free(req);
        return false;
    }

    xSemaphoreTake(req->done, portMAX_DELAY);
    vSemaphoreDelete(req->done);

    bool ok = req->ok;

    ESP_LOGI(TAG, "Backend done -> freeing buffers");
    heap_caps_free(req->pcm);
    heap_caps_free(req);

    return ok;
}

/* -------------------------------------------------------------------------- */
/* Audio recording                                                            */
/* -------------------------------------------------------------------------- */

static size_t record_utterance(int16_t *rec_buf, int16_t *frame, int timeout_frames)
{
    int speech_count = 0;
    int silence_count = 0;
    size_t rec_pos = 0;
    bool recording = false;
    int total_frames = 0;

    int16_t *pre_buf = heap_caps_malloc(PREROLL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pre_buf) {
        ESP_LOGE(TAG, "Alloc pre_buf failed");
        return 0;
    }
    memset(pre_buf, 0, PREROLL_SAMPLES * sizeof(int16_t));

    size_t pre_pos = 0;
    size_t pre_count = 0;

    while (1) {
        if (!audio_player_mic_is_active()) {
            heap_caps_free(pre_buf);
            return 0;
        }

        if (timeout_frames > 0 && !recording && total_frames >= timeout_frames) {
            ESP_LOGI(TAG, "Listen timeout (%d frames without speech)", total_frames);
            heap_caps_free(pre_buf);
            return 0;
        }

        size_t got = 0;
        esp_err_t ret = audio_player_mic_read(frame, FRAME_SAMPLES, &got, pdMS_TO_TICKS(200));
        if (ret != ESP_OK || got == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        total_frames++;

        int64_t sum = 0;
        for (size_t i = 0; i < got; i++) {
            int32_t s = frame[i];
            sum += (int64_t)s * (int64_t)s;
        }

        int rms = (int)sqrtf((float)(sum / (int64_t)got));

        for (size_t i = 0; i < got; i++) {
            pre_buf[pre_pos] = frame[i];
            pre_pos = (pre_pos + 1) % PREROLL_SAMPLES;
            if (pre_count < PREROLL_SAMPLES) {
                pre_count++;
            }
        }

        if (!recording) {
            if (rms > VOICE_VAD_THRESHOLD) {
                speech_count++;
                if (speech_count >= SPEECH_FRAMES) {
                    recording = true;
                    rec_pos = 0;
                    silence_count = 0;

                    ESP_LOGI(TAG, ">> Speech detected (RMS=%d)", rms);

                    if (pre_count > 0) {
                        size_t start = (pre_pos + PREROLL_SAMPLES - pre_count) % PREROLL_SAMPLES;
                        for (size_t i = 0; i < pre_count && rec_pos < MAX_SAMPLES; i++) {
                            size_t idx = (start + i) % PREROLL_SAMPLES;
                            rec_buf[rec_pos++] = pre_buf[idx];
                        }
                    }
                }
            } else {
                speech_count = 0;
            }
        } else {
            if (rec_pos + got <= MAX_SAMPLES) {
                memcpy(&rec_buf[rec_pos], frame, got * sizeof(int16_t));
                rec_pos += got;
            }

            if (rms < VOICE_VAD_THRESHOLD) {
                silence_count++;
                if (silence_count >= (SILENCE_FRAMES + POSTROLL_FRAMES)) {
                    ESP_LOGI(TAG, "<< End of speech (%zu samples, %.1f s)",
                             rec_pos, (float)rec_pos / SAMPLE_RATE);
                    heap_caps_free(pre_buf);
                    return rec_pos;
                }
            } else {
                silence_count = 0;
            }

            if (rec_pos >= MAX_SAMPLES) {
                ESP_LOGI(TAG, "<< Max duration (%d s)", MAX_RECORD_S);
                heap_caps_free(pre_buf);
                return rec_pos;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Backend worker task                                                        */
/* -------------------------------------------------------------------------- */

static void backend_task(void *arg)
{
    (void)arg;

    backend_request_t *req = NULL;

    while (1) {
        if (xQueueReceive(s_backend_queue, &req, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI("backend_task", "Processing backend request (%zu samples)", req->samples);

            req->ok = send_to_backend(req->pcm, req->samples, req->result, req->result_len);
            if (!req->ok && req->result && req->result_len > 0) {
                req->result[0] = '\0';
            }

            if (req->done) {
                xSemaphoreGive(req->done);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Main voice task                                                            */
/* -------------------------------------------------------------------------- */

static void voice_cmd_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "--- Starting voice_cmd ---");

    int16_t *rec_buf = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!rec_buf) {
        ESP_LOGE(TAG, "Alloc rec_buf failed (%d KB)", (MAX_SAMPLES * 2) / 1024);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Buffer PSRAM: %d KB", (MAX_SAMPLES * 2) / 1024);

    ensure_mic_running();

    int16_t frame[FRAME_SAMPLES];
    char backend_json[1024];
    backend_result_t backend;

    while (1) {
        if (!audio_player_mic_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            ensure_mic_running();
            continue;
        }

        backend_json[0] = '\0';
        backend_result_init(&backend);

        ESP_LOGI(TAG, "State: %s",
                 (s_state == VOICE_STATE_WAIT_WAKE) ? "WAIT_WAKE" : "WAIT_COMMAND");

        int timeout_frames = (s_state == VOICE_STATE_WAIT_COMMAND) ? FOLLOWUP_TIMEOUT : 0;

        size_t samples = record_utterance(rec_buf, frame, timeout_frames);
        if (samples == 0) {
            if (s_state == VOICE_STATE_WAIT_COMMAND) {
                ESP_LOGI(TAG, "No command after wake, back to WAIT_WAKE");
                s_state = VOICE_STATE_WAIT_WAKE;
            }
            ensure_mic_running();
            continue;
        }

        ensure_mic_stopped();

        led_anim_start();
        bool ok = backend_call_async(rec_buf, samples, backend_json, sizeof(backend_json));
        led_anim_stop();

        if (!ok) {
            ESP_LOGW(TAG, "Backend call failed");
            if (s_state == VOICE_STATE_WAIT_COMMAND) {
                play_and_wait(AUDIO_MSG_ERROR);
                s_state = VOICE_STATE_WAIT_WAKE;
            }
            ensure_mic_running();
            continue;
        }

        if (!parse_backend_json(backend_json, &backend)) {
            ESP_LOGW(TAG, "Backend JSON parse failed");
            if (s_state == VOICE_STATE_WAIT_COMMAND) {
                play_and_wait(AUDIO_MSG_ERROR);
                s_state = VOICE_STATE_WAIT_WAKE;
            }
            ensure_mic_running();
            continue;
        }

        if (s_state == VOICE_STATE_WAIT_WAKE) {
            if (backend.status_success &&
                strcmp(backend.category, "WAKE") == 0 &&
                backend.kiraactive) {

                ESP_LOGI(TAG, "Wake accepted by backend");
                play_and_wait(7);
                s_state = VOICE_STATE_WAIT_COMMAND;
            } else {
                ESP_LOGI(TAG, "Wake not detected");
            }

            ensure_mic_running();
            continue;
        }

        /* WAIT_COMMAND */
        if (backend.status_success &&
            strcmp(backend.category, "HA") == 0 &&
            backend.ha_ok) {

            ESP_LOGI(TAG, "HA action confirmed");
            play_and_wait(AUDIO_MSG_FAIT);
        } else {
            ESP_LOGW(TAG, "HA action failed or not acknowledged");
            play_and_wait(AUDIO_MSG_ERROR);
        }

        s_state = VOICE_STATE_WAIT_WAKE;
        ensure_mic_running();
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t voice_cmd_init(void)
{
    ha_client_init();

    s_backend_queue = xQueueCreate(BACKEND_QUEUE_LEN, sizeof(backend_request_t *));
    if (!s_backend_queue) {
        ESP_LOGE(TAG, "Failed to create backend queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok_backend = xTaskCreatePinnedToCore(
        backend_task,
        "backend_task",
        12288,
        NULL,
        4,
        NULL,
        1
    );
    if (ok_backend != pdPASS) {
        ESP_LOGE(TAG, "Failed to create backend task");
        vQueueDelete(s_backend_queue);
        s_backend_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok_vc = xTaskCreatePinnedToCore(
        voice_cmd_task,
        "voice_cmd",
        16384,
        NULL,
        4,
        &s_task,
        0
    );
    if (ok_vc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create voice_cmd task");
        vQueueDelete(s_backend_queue);
        s_backend_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_state = VOICE_STATE_WAIT_WAKE;
    ESP_LOGI(TAG, "Voice command module initialized");
    return ESP_OK;
}