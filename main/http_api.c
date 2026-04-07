#include "http_api.h"
#include "config.h"
#include "audio_player.h"
#include "face_anim.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "cJSON.h"

static const char *TAG = "http_api";
extern void main_touch_wake_request(void);

// ===== Helpers =====


static esp_err_t face_get_handler(httpd_req_t *req)
{
    int x = 0, y = 0;
    char resp[64];

    face_anim_get_position(&x, &y);
    snprintf(resp, sizeof(resp), "{\"x\":%d,\"y\":%d}", x, y);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t handle_wake(httpd_req_t *req)
{
    ESP_LOGI("http_api", "Wake request received");

    main_touch_wake_request();

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t face_set_handler(httpd_req_t *req)
{
    char query[64];
    char val[16];
    int x, y;

    face_anim_get_position(&x, &y);

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "x", val, sizeof(val)) == ESP_OK) {
            x = atoi(val);
        }
        if (httpd_query_key_value(query, "y", val, sizeof(val)) == ESP_OK) {
            y = atoi(val);
        }
    }

    face_anim_set_position(x, y);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"x\":%d,\"y\":%d}", x, y);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}


static void send_json(httpd_req_t *req, int status, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status == 200 ? "200 OK" :
                               status == 400 ? "400 Bad Request" :
                               status == 401 ? "401 Unauthorized" :
                               status == 404 ? "404 Not Found" :
                               "500 Internal Server Error");
    httpd_resp_sendstr(req, json);
}

static bool check_auth(httpd_req_t *req)
{
#if !ENABLE_AUTH
    return true;
#else
    // Verify the X-API-Token header
    char token_buf[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-API-Token", token_buf, sizeof(token_buf)) == ESP_OK) {
        if (strcmp(token_buf, API_TOKEN) == 0) return true;
    }

    // Fallback: query parameter ?token=...
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[128] = {0};
        if (httpd_query_key_value(query, "token", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, API_TOKEN) == 0) return true;
        }
    }

    send_json(req, 401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return false;
#endif
}

// Decode URL (%xx and +)
static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di < dst_size - 1; i++) {
        if (src[i] == '+') {
            dst[di++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

// Escape JSON basic
static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di < dst_size - 2; i++) {
        switch (src[i]) {
        case '\\': if (di + 2 < dst_size) { dst[di++] = '\\'; dst[di++] = '\\'; } break;
        case '"':  if (di + 2 < dst_size) { dst[di++] = '\\'; dst[di++] = '"'; }  break;
        case '\n': if (di + 2 < dst_size) { dst[di++] = '\\'; dst[di++] = 'n'; }  break;
        case '\r': if (di + 2 < dst_size) { dst[di++] = '\\'; dst[di++] = 'r'; }  break;
        default:
            if ((unsigned char)src[i] >= 0x20) dst[di++] = src[i];
            break;
        }
    }
    dst[di] = '\0';
}

// Extract ID from the path /play/N
static int extract_id_from_uri(const char *uri)
{
    const char *last_slash = strrchr(uri, '/');
    if (!last_slash || *(last_slash + 1) == '\0') return -1;
    return atoi(last_slash + 1);
}

// ===== Handlers =====

// GET /play?id=N&vol=V  ou  GET /play/N?vol=V
static esp_err_t handle_play(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char query[128] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    int id = -1;
    float vol = -1;

    // Try to extract the ID from the path
    const char *uri = req->uri;
    // Separate URI from the query string
    char uri_path[64] = {0};
    const char *qmark = strchr(uri, '?');
    if (qmark) {
        size_t plen = qmark - uri;
        if (plen >= sizeof(uri_path)) plen = sizeof(uri_path) - 1;
        memcpy(uri_path, uri, plen);
    } else {
        strncpy(uri_path, uri, sizeof(uri_path) - 1);
    }

    // /play/N
    if (strlen(uri_path) > 6) {  // "/play/" = 6 chars
        id = extract_id_from_uri(uri_path);
    }

    // /play?id=N
    if (id < 1) {
        char val[8] = {0};
        if (httpd_query_key_value(query, "id", val, sizeof(val)) == ESP_OK) {
            id = atoi(val);
        }
    }

    if (id < 1 || id > 13) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"id must be between 1 and 13\"}");
        return ESP_OK;
    }

    // Optional volume
    char vol_str[16] = {0};
    if (httpd_query_key_value(query, "vol", vol_str, sizeof(vol_str)) == ESP_OK) {
        vol = strtof(vol_str, NULL);
        audio_player_set_volume(vol);
    }

    esp_err_t ret = audio_player_play_file(id);
    if (ret != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"playback failed\"}");
        return ESP_OK;
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"queued\":%d,\"volume\":%.2f}",
             id, audio_player_get_volume());
    send_json(req, 200, resp);
    return ESP_OK;
}

// GET /playtxt?txt=Bonjour&vol=0.6
static esp_err_t handle_playtxt(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

#if !ENABLE_ELEVENLABS
    send_json(req, 400, "{\"ok\":false,\"error\":\"ElevenLabs disabled\"}");
    return ESP_OK;
#else
    char query[512] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"missing query\"}");
        return ESP_OK;
    }

    char txt_encoded[256] = {0};
    if (httpd_query_key_value(query, "txt", txt_encoded, sizeof(txt_encoded)) != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"missing txt parameter\"}");
        return ESP_OK;
    }

    char txt[256] = {0};
    url_decode(txt_encoded, txt, sizeof(txt));

    // Optional volume
    char vol_str[16] = {0};
    if (httpd_query_key_value(query, "vol", vol_str, sizeof(vol_str)) == ESP_OK) {
        audio_player_set_volume(strtof(vol_str, NULL));
    }

    // Escape for JSON
    char txt_escaped[512] = {0};
    json_escape(txt, txt_escaped, sizeof(txt_escaped));

    // Body JSON for ElevenLabs API
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"text\":\"%s\",\"model_id\":\"eleven_multilingual_v2\","
             "\"voice_settings\":{\"stability\":0.5,\"similarity_boost\":0.7}}",
             txt_escaped);

    // API URL
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.elevenlabs.io/v1/text-to-speech/%s", VOICE_ID);

    // HTTPS POST request
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "xi-api-key", ELEVENLABS_KEY);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "audio/mpeg");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_open(client, strlen(body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        send_json(req, 500, "{\"ok\":false,\"error\":\"HTTP open failed\"}");
        return ESP_OK;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        char err_resp[128];
        snprintf(err_resp, sizeof(err_resp),
                 "{\"ok\":false,\"error\":\"ElevenLabs HTTP %d\"}", status);
        esp_http_client_cleanup(client);
        send_json(req, 500, err_resp);
        return ESP_OK;
    }

    // Allocate buffer for MP3 data (with some extra space just in case)
    size_t buf_cap = content_length > 0 ? (size_t)content_length + 1024 : 128 * 1024;
    if (buf_cap > 512 * 1024) buf_cap = 512 * 1024;

    uint8_t *mp3_buf = heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mp3_buf) {
        esp_http_client_cleanup(client);
        send_json(req, 500, "{\"ok\":false,\"error\":\"PSRAM alloc failed\"}");
        return ESP_OK;
    }

    size_t total = 0;
    int read_len;
    while ((read_len = esp_http_client_read(client, (char *)(mp3_buf + total), buf_cap - total)) > 0) {
        total += (size_t)read_len;
        if (total >= buf_cap) break;
    }
    esp_http_client_cleanup(client);

    if (total < 1000) {
        heap_caps_free(mp3_buf);
        send_json(req, 500, "{\"ok\":false,\"error\":\"TTS download too small\"}");
        return ESP_OK;
    }

    // Restart playback (the buffer will be freed by audio_player)
    err = audio_player_play_buffer(mp3_buf, total);
    if (err != ESP_OK) {
        heap_caps_free(mp3_buf);
        send_json(req, 500, "{\"ok\":false,\"error\":\"playback failed\"}");
        return ESP_OK;
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"playing\":true,\"bytes\":%d,\"volume\":%.2f}",
             (int)total, audio_player_get_volume());
    send_json(req, 200, resp);
    return ESP_OK;
#endif
}

// GET /volume?level=0.7
static esp_err_t handle_volume(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;

    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"missing query\"}");
        return ESP_OK;
    }

    char val[16] = {0};
    if (httpd_query_key_value(query, "level", val, sizeof(val)) != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"paramètre level manquant\"}");
        return ESP_OK;
    }

    float v = strtof(val, NULL);
    audio_player_set_volume(v);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"volume\":%.2f}", audio_player_get_volume());
    send_json(req, 200, resp);
    return ESP_OK;
}

// GET /stop
static esp_err_t handle_stop(httpd_req_t *req)
{
    if (!check_auth(req)) return ESP_OK;
    audio_player_stop();
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

// GET /status
static esp_err_t handle_status(httpd_req_t *req)
{
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"playing\":%s,\"volume\":%.2f,\"file\":\"%s\",\"error\":\"%s\"}",
             audio_player_is_playing() ? "true" : "false",
             audio_player_get_volume(),
             audio_player_current_file(),
             audio_player_last_error());
    send_json(req, 200, resp);
    return ESP_OK;
}

// GET /list
static esp_err_t handle_list(httpd_req_t *req)
{
    char resp[512] = "{\"ok\":true,\"files\":[";
    size_t pos = strlen(resp);
    bool first = true;

    // List files in /spiffs/msg/
    DIR *dir = opendir("/spiffs/msg");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                int written = snprintf(resp + pos, sizeof(resp) - pos,
                                       "%s\"/msg/%s\"", first ? "" : ",", entry->d_name);
                if (written > 0) pos += written;
                first = false;
            }
        }
        closedir(dir);
    }

    snprintf(resp + pos, sizeof(resp) - pos, "]}");
    send_json(req, 200, resp);
    return ESP_OK;
}

// Catch-all pour /play/N
static esp_err_t handle_catch_all(httpd_req_t *req)
{
    const char *uri = req->uri;
    // Extract the path without query string
    char path[64] = {0};
    const char *q = strchr(uri, '?');
    size_t plen = q ? (size_t)(q - uri) : strlen(uri);
    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
    memcpy(path, uri, plen);

    if (strncmp(path, "/play/", 6) == 0) {
        return handle_play(req);
    }

    send_json(req, 404, "{\"ok\":false,\"error\":\"Not found\"}");
    return ESP_OK;
}

// ===== Start server =====
esp_err_t http_api_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 14;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");

    // Exact routes (high priority)
    const httpd_uri_t uris[] = {
        { .uri = "/play",     .method = HTTP_GET, .handler = handle_play },
        { .uri = "/playtxt",  .method = HTTP_GET, .handler = handle_playtxt },
        { .uri = "/volume",   .method = HTTP_GET, .handler = handle_volume },
        { .uri = "/stop",     .method = HTTP_GET, .handler = handle_stop },
        { .uri = "/status",   .method = HTTP_GET, .handler = handle_status },
        { .uri = "/list",     .method = HTTP_GET, .handler = handle_list },

        // new face routes
        { .uri = "/face/get", .method = HTTP_GET, .handler = face_get_handler },
        { .uri = "/face/set", .method = HTTP_GET, .handler = face_set_handler },

         // Wake endpoint for touch wakeup
        { .uri = "/wake", .method = HTTP_GET, .handler = handle_wake },

        // catch-all in end 
        { .uri = "/*",        .method = HTTP_GET, .handler = handle_catch_all },
        
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    ESP_LOGI(TAG, "  GET /play/1 .. /play/13");
    ESP_LOGI(TAG, "  GET /play?id=N&vol=V");
    ESP_LOGI(TAG, "  GET /playtxt?txt=...&vol=V");
    ESP_LOGI(TAG, "  GET /volume?level=0.7");
    ESP_LOGI(TAG, "  GET /stop");
    ESP_LOGI(TAG, "  GET /status");
    ESP_LOGI(TAG, "  GET /list");
    ESP_LOGI(TAG, "  GET /face/get");
    ESP_LOGI(TAG, "  GET /face/set?x=112&y=145");
    ESP_LOGI(TAG, "  GET /wake");


    return ESP_OK;
}


