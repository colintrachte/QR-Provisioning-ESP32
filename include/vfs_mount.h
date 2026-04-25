#pragma once
#include "esp_err.h"

/**
 * Idempotent LittleFS mount at /littlefs (partition label "storage").
 * Safe to call from multiple modules (portal, app_server, etc.).
 * Returns ESP_OK if mounted or already mounted.
 */
esp_err_t vfs_mount_littlefs(void);
