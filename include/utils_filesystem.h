#pragma once
#include "esp_err.h" // Add this to define esp_err_t
/**
 * utils_filesystem.h — Filesystem and JSON helper API.
 *
 * Owns:
 *   - LittleFS mount (idempotent, call from any module)
 *   - FS inventory logging
 *   - WiFi scan result serialisation (shared by portal + app_server)
 *   - WiFi status JSON (shared by portal + app_server)
 *   - AP record → quality conversion
 */

/* ── LittleFS ───────────────────────────────────────────────────────────── */
#define FS_BASE  "/littlefs"
/**
 * Mount the LittleFS "storage" partition at /littlefs.
 * Idempotent — safe to call from portal.c and app_server.c; only mounts once.
 */
esp_err_t vfs_mount_littlefs(void);

/**
 * Log every file in /littlefs to the serial monitor (INFO level).
 * Call once after mounting to confirm .gz assets are present.
 */
void vfs_log_inventory(void);
