#include "wake_local.h"
#include "esp_log.h"
#include "audio_player.h"
#include "main.h"

#define TAG "wake_local"

// TODO: inclure les headers ESP-SR nécessaires

void wake_local_init(void) {
    ESP_LOGI(TAG, "Initialisation du wake word local...");
    // TODO: Initialiser ESP-SR/Skainet ici
}

void wake_local_task(void *arg) {
    (void)arg;
    while (1) {
        // TODO: Boucle d'écoute du wake word
        // Si wake word détecté :
        // 1. audio_player_play_file(7);
        // 2. Attendre la fin du son
        // 3. Capturer la phrase suivante
        // 4. Envoyer à Ollama
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
