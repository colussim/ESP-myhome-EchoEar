#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise le moteur de wake word local.
 *
 * Cette fonction prépare ESP-SR / AFE / WakeNet et les buffers nécessaires.
 * À appeler une seule fois au démarrage.
 */
esp_err_t wake_word_local_init(void);

/**
 * @brief Démarre l’écoute locale du wake word.
 *
 * Lance la tâche de traitement si elle n’est pas déjà active.
 */
esp_err_t wake_word_local_start(void);

/**
 * @brief Arrête l’écoute locale du wake word.
 *
 * La tâche reste créée mais cesse d’analyser le flux micro.
 */
void wake_word_local_stop(void);

/**
 * @brief Attend la détection du wake word.
 *
 * @param timeout_ticks délai max d’attente
 * @return true si le wake word a été détecté, false sinon
 */
bool wake_word_local_wait_for_trigger(TickType_t timeout_ticks);

/**
 * @brief Indique si le moteur wake word est actif.
 */
bool wake_word_local_is_running(void);

/**
 * @brief Libère les ressources du moteur wake word.
 */
void wake_word_local_deinit(void);

#ifdef __cplusplus
}
#endif
