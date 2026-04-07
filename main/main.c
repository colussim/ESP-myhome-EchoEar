#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_littlefs.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "config.h"
#include "board.h"
#include "led_anim.h"
#include "audio_player.h"
#include "display.h"
#include "face_anim.h"
#include "http_api.h"
#include "voice_cmd.h"
#include "touch_wakeup.h"
#include "wake_local.h"

static const char *TAG = "main";
static volatile bool s_touch_wake_armed = false;
static volatile bool s_stt_in_progress  = false;


// -----------------------------------------------------------------------------
// Shared EventGroup wake_local <-> voice_cmd
// Defined here, declared extern in wake_local.h
// -----------------------------------------------------------------------------
EventGroupHandle_t g_wake_event_group = NULL;

// -----------------------------------------------------------------------------
// STT state (used by other modules)
// -----------------------------------------------------------------------------
bool main_is_stt_in_progress(void)
{
    return s_stt_in_progress;
}

void main_set_stt_in_progress(bool v)
{
    s_stt_in_progress = v;
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// -----------------------------------------------------------------------------
// Sleep / wake
// -----------------------------------------------------------------------------
#define INACTIVITY_TIMEOUT_MS   (5 * 60 * 1000)   // 5 minutes
#define WAKE_DEBOUNCE_MS        800
#define INACTIVITY_POLL_MS      250

static volatile bool      s_screen_sleeping    = false;
static volatile bool      s_touch_wake_pending = false;
static volatile TickType_t s_last_activity_tick = 0;
static volatile TickType_t s_last_wake_tick     = 0;

// -----------------------------------------------------------------------------
// Shared I2C bus for audio codecs
// -----------------------------------------------------------------------------
i2c_master_bus_handle_t s_i2c_bus = NULL;

// -----------------------------------------------------------------------------
// External audio helpers
// -----------------------------------------------------------------------------
extern bool      audio_player_is_playing(void);
extern esp_err_t audio_player_play_file(int id);

// -----------------------------------------------------------------------------
// SPIFFS/LittleFS debug listing
// -----------------------------------------------------------------------------
static void list_spiffs_files(void)
{
    DIR *dir = opendir("/spiffs");
    if (!dir) {
        ESP_LOGE("spiffs", "Unable to open /spiffs");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        ESP_LOGI("spiffs", "FILE: %s", ent->d_name);
    }
    closedir(dir);
}

// -----------------------------------------------------------------------------
// Activity tracking
// -----------------------------------------------------------------------------
static inline void mark_activity_from_task(void)
{
    s_last_activity_tick = xTaskGetTickCount();
}

static inline uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}

// -----------------------------------------------------------------------------
// Safe screen power control — backlight only, never cut LCD power
// -----------------------------------------------------------------------------
static void screen_backlight_set(bool on)
{
    gpio_set_level(BOARD_LCD_BL, on ? 1 : 0);
    s_screen_sleeping = !on;
    ESP_LOGI(TAG, "Screen %s", on ? "ON" : "OFF");
}

// -----------------------------------------------------------------------------
// Time / NTP
// -----------------------------------------------------------------------------
static void time_init_timezone_zurich(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
}

static void time_init_ntp(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    ESP_LOGI(TAG, "NTP started");
}

static bool time_is_reasonable(void)
{
    time_t now = 0;
    time(&now);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    return (timeinfo.tm_year + 1900) >= 2024;
}

static void wait_for_time_sync(int max_wait_seconds)
{
    int waited = 0;
    while (!time_is_reasonable() && waited < max_wait_seconds) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited++;
    }

    if (time_is_reasonable()) {
        time_t now;
        struct tm timeinfo = {0};
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "Time synced: %02d:%02d:%02d",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        ESP_LOGW(TAG, "Time sync not available yet, greeting will use fallback");
    }
}

// -----------------------------------------------------------------------------
// Shared I2C init
// -----------------------------------------------------------------------------
static i2c_master_bus_handle_t shared_i2c_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port             = I2C_NUM_0,
        .sda_io_num           = BOARD_I2C_SDA,
        .scl_io_num           = BOARD_I2C_SCL,
        .clk_source           = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt    = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));
    ESP_LOGI(TAG, "I2C initialized (SDA=%d, SCL=%d)", BOARD_I2C_SDA, BOARD_I2C_SCL);
    return bus_handle;
}

// -----------------------------------------------------------------------------
// Wake sequence (touch / écran)
// -----------------------------------------------------------------------------
static void show_kira_and_greet(void)
{
    s_touch_wake_armed = false;

    screen_backlight_set(true);
    vTaskDelay(pdMS_TO_TICKS(80));

    face_anim_stop_talking();
    face_anim_set_amplitude(0);

    ESP_LOGI(TAG, "Touch wake: play id 7");
    audio_player_play_file(7);

    mark_activity_from_task();
}

// -----------------------------------------------------------------------------
// Public hooks appelés par d'autres modules
// -----------------------------------------------------------------------------
void main_touch_wake_request(void)
{
    ESP_LOGI(TAG, "main_touch_wake_request()");
    s_touch_wake_pending = true;
}

void main_notify_user_activity(void)
{
    mark_activity_from_task();
}

bool main_is_screen_sleeping(void)
{
    return s_screen_sleeping;
}

bool main_is_touch_wake_armed(void)
{
    return s_touch_wake_armed;
}

// -----------------------------------------------------------------------------
// Inactivity / wake management task
// -----------------------------------------------------------------------------
static void inactivity_task(void *arg)
{
    (void)arg;

    s_last_activity_tick = xTaskGetTickCount();
    s_last_wake_tick     = 0;

    ESP_LOGI(TAG, "inactivity_task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(INACTIVITY_POLL_MS));

        TickType_t now    = xTaskGetTickCount();
        uint32_t idle_ms  = ticks_to_ms(now - s_last_activity_tick);

        /* Pendant la lecture audio, le système est considéré actif */
        if (audio_player_is_playing()) {
            mark_activity_from_task();
            continue;
        }

        /* Mise en veille écran après inactivité */
        if (!s_screen_sleeping && idle_ms >= INACTIVITY_TIMEOUT_MS) {
            ESP_LOGI(TAG, "Inactivity timeout -> screen sleep");
            screen_backlight_set(false);
        }

        /* Réveil par touch */
        if (s_touch_wake_pending) {
            ESP_LOGI(TAG, "Touch wake pending detected");
            s_touch_wake_pending = false;

            uint32_t since_last_wake_ms = ticks_to_ms(now - s_last_wake_tick);
            if (since_last_wake_ms < WAKE_DEBOUNCE_MS) {
                ESP_LOGI(TAG, "Touch wake ignored by debounce (%lu ms)",
                         (unsigned long)since_last_wake_ms);
                continue;
            }

            s_last_wake_tick = now;
            mark_activity_from_task();

            if (s_screen_sleeping) {
                ESP_LOGI(TAG, "Touch wake -> screen on + greeting");
                show_kira_and_greet();
            } else {
                ESP_LOGI(TAG, "Touch wake received but screen already on");
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Wi-Fi events
// -----------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
#if WIFI_USE_STATIC_IP
        ESP_LOGI(TAG, "WiFi connected, static IP: %s", WIFI_IP);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
#endif
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting in 1s...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

#if WIFI_USE_STATIC_IP
    esp_netif_dhcpc_stop(netif);

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_str_to_ip4(WIFI_IP,      &ip_info.ip);
    esp_netif_str_to_ip4(WIFI_GATEWAY, &ip_info.gw);
    esp_netif_str_to_ip4(WIFI_SUBNET,  &ip_info.netmask);
    esp_netif_set_ip_info(netif, &ip_info);

    esp_netif_dns_info_t dns_info = {0};
    esp_netif_str_to_ip4(WIFI_DNS1, &dns_info.ip.u_addr.ip4);
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
#else
    (void)netif;
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid,     WIFI_SSID,     sizeof(wifi_config.sta.ssid)     - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(82));

    ESP_LOGI(TAG, "Connecting WiFi to %s (PS=OFF, TX=max)...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
}

// -----------------------------------------------------------------------------
// LittleFS init
// -----------------------------------------------------------------------------
static void spiffs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path            = "/spiffs",
        .partition_label      = "spiffs",
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed (%s)", esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "LittleFS: %zu bytes total, %zu bytes used", total, used);
}

// -----------------------------------------------------------------------------
// app_main
// -----------------------------------------------------------------------------
void app_main(void)
{
    time_init_timezone_zurich();
    ESP_LOGI(TAG, "=== EchoEar Player (ESP-VoCat v1.2) ===");

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Filesystem
    spiffs_init();
    list_spiffs_files();

    // LED
    ESP_ERROR_CHECK(led_anim_init());

    // Audio (I2C + codec)
    s_i2c_bus = shared_i2c_init();
    ESP_ERROR_CHECK(audio_player_init(s_i2c_bus));

    // Network + time
    wifi_init();
    time_init_ntp();
    wait_for_time_sync(10);

    // Display
    ESP_LOGI(TAG, "RAM libre avant display_init:   %u", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "PSRAM libre avant display_init: %u", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(face_anim_init());
    face_anim_stop_talking();
    face_anim_set_amplitude(0);
    screen_backlight_set(true);

    // Salut de démarrage : Bonjour (ID 14) si journée, Bonsoir (ID 15) si soir/nuit
    {
        time_t now = time(NULL);
        struct tm tinfo;
        localtime_r(&now, &tinfo);
        int hour = tinfo.tm_hour;
        int greeting_id = (hour >= 6 && hour < 20) ? 11 : 12;
        ESP_LOGI(TAG, "Startup greeting: hour=%d, playing id %d", hour, greeting_id);
        audio_player_play_file(greeting_id);
        // Attendre la fin du message avant de démarrer le wake word
        while (audio_player_is_playing()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // HTTP API
    ESP_ERROR_CHECK(http_api_start());

    // -------------------------------------------------------------------------
    // EventGroup wake — MUST be created before voice_cmd_init and wake_local_init
    // -------------------------------------------------------------------------
    g_wake_event_group = xEventGroupCreate();
    if (!g_wake_event_group) {
        ESP_LOGE(TAG, "Failed to create g_wake_event_group — system halted");
        return;
    }
    ESP_LOGI(TAG, "g_wake_event_group created");

    // voice_cmd waits on g_wake_event_group => must be init after
    ESP_ERROR_CHECK(voice_cmd_init());

    // Touch wakeup
    ESP_ERROR_CHECK(touch_wakeup_init());
    ESP_LOGI(TAG, "touch_wakeup_init() OK");

    // Debug memory before wake_local_init
    ESP_LOGI(TAG, "Free RAM before wake_local_init:  %u", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Free PSRAM before wake_local_init: %u", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Free internal RAM:                 %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // -------------------------------------------------------------------------
    // Wake word local (AFE / esp-sr) — feed + fetch sur core 1
    // -------------------------------------------------------------------------
    bool wake_ok = wake_local_init();

    if (wake_ok) {
        // Feed task — reads the I2S and feeds the AFE, high priority
        StaticTask_t *feed_tcb   = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        StackType_t  *feed_stack = heap_caps_malloc(8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        if (feed_tcb && feed_stack) {
            xTaskCreateStaticPinnedToCore(wake_feed_task, "wake_feed",
                8192, NULL, 6, feed_stack, feed_tcb, 1);
            ESP_LOGI(TAG, "wake_feed_task created on core 1");
        } else {
            ESP_LOGE(TAG, "Allocation wake_feed_task failed");
        }

        // Fetch task — retrieves AFE results, slightly lower priority
        StaticTask_t *fetch_tcb   = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        StackType_t  *fetch_stack = heap_caps_malloc(4096 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        if (fetch_tcb && fetch_stack) {
            xTaskCreateStaticPinnedToCore(wake_fetch_task, "wake_fetch",
                4096, NULL, 5, fetch_stack, fetch_tcb, 1);
            ESP_LOGI(TAG, "wake_fetch_task created on core 1");
        } else {
            ESP_LOGE(TAG, "Allocation wake_fetch_task failed");
        }
    } else {
        ESP_LOGE(TAG, "wake_local_init() failed — wake word disabled");
    }

    // -------------------------------------------------------------------------
    // Inactivity / screen management — core 0
    // -------------------------------------------------------------------------
    StaticTask_t *inact_tcb   = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    StackType_t  *inact_stack = heap_caps_malloc(8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    if (inact_tcb && inact_stack) {
        xTaskCreateStaticPinnedToCore(inactivity_task, "inactivity",
            8192, NULL, 4, inact_stack, inact_tcb, 0);
        ESP_LOGI(TAG, "inactivity_task created on core 0");
    } else {
        ESP_LOGE(TAG, "Allocation inactivity_task failed");
    }

    ESP_LOGI(TAG, "System ready!");
}