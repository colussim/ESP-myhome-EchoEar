idf_component_register(
    SRCS "main.c" "audio_player.c" "led_anim.c" "http_api.c" "display.c" "face_anim.c" "voice_cmd.c" "ha_client.c" "touch_wakeup.h" "touch_wakeup.c" "secrets.h" "wake_word_local.c" "wake_word_local.h"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_http_server esp_http_client nvs_flash spiffs esp_psram esp_lcd esp_jpeg esp_system
    PRIV_REQUIRES minimp3 mbedtls json esp_timer 
)
