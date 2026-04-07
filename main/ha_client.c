#include "ha_client.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

static const char *TAG = "ha_client";

/* -------------------------------------------------------------------------- */
/* HTTP response accumulator                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool truncated;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (!resp) {
        return ESP_OK;
    }

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data && evt->data_len > 0 && resp->buf && resp->cap > 0) {
                size_t room = 0;
                if (resp->cap > resp->len + 1) {
                    room = resp->cap - resp->len - 1;
                }

                if (room == 0) {
                    resp->truncated = true;
                    return ESP_OK;
                }

                size_t copy_len = ((size_t)evt->data_len < room) ? (size_t)evt->data_len : room;
                memcpy(resp->buf + resp->len, evt->data, copy_len);
                resp->len += copy_len;
                resp->buf[resp->len] = '\0';

                if ((size_t)evt->data_len > copy_len) {
                    resp->truncated = true;
                }
            }
            break;

        default:
            break;
    }

    return ESP_OK;
}

static void http_resp_init(http_resp_t *resp, char *buf, size_t cap)
{
    if (!resp || !buf || cap == 0) {
        return;
    }

    resp->buf = buf;
    resp->len = 0;
    resp->cap = cap;
    resp->truncated = false;
    resp->buf[0] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Binary response accumulator (PSRAM) — pour TTS                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    bool     oom;
} http_bin_resp_t;

static esp_err_t http_binary_event_handler(esp_http_client_event_t *evt)
{
    http_bin_resp_t *resp = (http_bin_resp_t *)evt->user_data;
    if (!resp) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        size_t new_len = resp->len + (size_t)evt->data_len;
        if (new_len > resp->cap) {
            size_t new_cap = new_len + 32768; /* granularité 32 KB */
            uint8_t *tmp = heap_caps_realloc(resp->buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!tmp) {
                resp->oom = true;
                return ESP_OK;
            }
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, (size_t)evt->data_len);
        resp->len += (size_t)evt->data_len;
    }

    return ESP_OK;
}

static void log_http_problem(const char *prefix, esp_err_t err, int status, const http_resp_t *resp)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s network error: %s", prefix, esp_err_to_name(err));
        return;
    }

    if (status < 200 || status >= 300) {
        if (resp && resp->buf && resp->buf[0]) {
            ESP_LOGE(TAG, "%s HTTP %d: %.400s", prefix, status, resp->buf);
        } else {
            ESP_LOGE(TAG, "%s HTTP %d (empty body)", prefix, status);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public init                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t ha_client_init(void)
{
    ESP_LOGI(TAG, "Backend client initialized (%s)", WHISPER_URL);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* WAV -> backend JSON                                                        */
/* -------------------------------------------------------------------------- */

esp_err_t ha_stt_recognize(const uint8_t *wav_data, size_t wav_len,
                           ha_stt_result_t *result)
{
    if (!wav_data || wav_len == 0 || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));

    char resp_buf[1024];
    http_resp_t resp;
    http_resp_init(&resp, resp_buf, sizeof(resp_buf));

    esp_http_client_config_t cfg = {
        .url = WHISPER_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 45000,
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .buffer_size = 8192,
        .buffer_size_tx = 16384,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);

    /* Backend custom */
    esp_http_client_set_header(client, "X-Token", WHISPER_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

    /* WAV brut */
    esp_http_client_set_post_field(client, (const char *)wav_data, (int)wav_len);

    ESP_LOGI(TAG, "Backend POST %zu bytes -> %s", wav_len, WHISPER_URL);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        log_http_problem("Backend", err, status, &resp);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    if (resp.truncated) {
        ESP_LOGW(TAG, "Backend response truncated to %u bytes",
                 (unsigned)(sizeof(resp_buf) - 1));
    }

    if (resp.buf[0] == '\0') {
        ESP_LOGE(TAG, "Backend empty response body");
        return ESP_FAIL;
    }

    /* IMPORTANT:
        Here, we don't parse the JSON.
        We send the raw JSON to voice_cmd.c
    */
    strncpy(result->text, resp.buf, sizeof(result->text) - 1);
    result->text[sizeof(result->text) - 1] = '\0';
    result->success = true;

    ESP_LOGI(TAG, "Backend JSON: %.400s", result->text);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Text -> WAV (TTS backend)                                                  */
/* -------------------------------------------------------------------------- */

esp_err_t ha_tts_speak(const char *text, uint8_t **out_buf, size_t *out_len)
{
    if (!text || !text[0] || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_buf = NULL;
    *out_len = 0;

    http_bin_resp_t resp = {0};
    resp.buf = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM); /* 64 KB initial */
    if (!resp.buf) {
        ESP_LOGE(TAG, "TTS: alloc PSRAM failed");
        return ESP_ERR_NO_MEM;
    }
    resp.cap = 65536;

    esp_http_client_config_t cfg = {
        .url             = TTS_URL,
        .event_handler   = http_binary_event_handler,
        .user_data       = &resp,
        .timeout_ms      = 30000,
        .transport_type  = HTTP_TRANSPORT_OVER_TCP,
        .buffer_size     = 8192,
        .buffer_size_tx  = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        heap_caps_free(resp.buf);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "X-Token", WHISPER_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    esp_http_client_set_post_field(client, text, (int)strlen(text));

    ESP_LOGI(TAG, "TTS POST \"%s\" -> %s", text, TTS_URL);

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "TTS error: %s HTTP %d", esp_err_to_name(err), status);
        heap_caps_free(resp.buf);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    if (resp.oom || resp.len == 0) {
        ESP_LOGE(TAG, "TTS: %s", resp.oom ? "OOM durant réception" : "réponse vide");
        heap_caps_free(resp.buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TTS: %zu bytes WAV reçus", resp.len);
    *out_buf = resp.buf;
    *out_len = resp.len;
    return ESP_OK;
}