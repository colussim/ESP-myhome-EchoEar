#pragma once

#include "esp_err.h"

/**
 * @brief Démarre le serveur HTTP avec toutes les routes API
 */
esp_err_t http_api_start(void);
