/**
 * ota_server.h — OTA firmware and filesystem update endpoints.
 *
 * Registers two HTTP POST handlers on the existing app_server httpd:
 *
 *   POST /ota/firmware   — receive a .bin and flash it to the inactive OTA
 *                          partition. Reboots on success.
 *   POST /ota/filesystem — receive a .bin and flash it to the LittleFS
 *                          "storage" partition. Remounts on success; no reboot.
 *
 * Both endpoints stream the upload in chunks so RAM usage is bounded
 * regardless of firmware size. Authentication is a single shared token
 * set in config.h as OTA_AUTH_TOKEN. Pass it as the HTTP header:
 *   X-OTA-Token: <token>
 * Leave OTA_AUTH_TOKEN as an empty string to disable auth (LAN-only use).
 *
 * Rollback (firmware only)
 * ─────────────────────────
 * Requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in sdkconfig.defaults.
 * After flashing, the bootloader boots the new image in a "pending verify"
 * state. ota_server_mark_valid() must be called after the application has
 * confirmed it is healthy (WiFi connected, server up). If the new firmware
 * crashes before that call, the next boot automatically rolls back to the
 * previous image.
 *
 * ota_server_mark_valid() is safe to call unconditionally — it is a no-op
 * if rollback is not enabled or the image is already marked valid.
 */

#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Register OTA endpoints on an already-started httpd instance.
 * Call from app_server_start() after httpd_start().
 *
 * @param server  Handle returned by httpd_start().
 * @return ESP_OK, or an error if handler registration failed.
 */
esp_err_t ota_server_register(httpd_handle_t server);

/**
 * Mark the running firmware as valid, cancelling any pending rollback.
 * Call once after the application has confirmed healthy operation
 * (WiFi connected, WebSocket server up).
 */
void ota_server_mark_valid(void);
