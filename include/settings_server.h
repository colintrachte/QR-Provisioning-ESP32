/**
 * settings_server.h — HTTP API layer for runtime settings.
 *
 * Registers three routes on an existing httpd instance:
 *
 *   GET  /api/settings         → full settings JSON blob
 *   POST /api/settings         → validate + save new settings
 *   POST /api/settings/reset   → factory defaults + optional reboot
 *
 * Designed to be registered inside app_server_start() alongside the other
 * route tables. settings_server_register() does not start an httpd — it
 * attaches to the one app_server already owns.
 *
 * The GET response shape mirrors robot_settings_t exactly. The POST body
 * must be a JSON object; only fields present in the body are validated and
 * updated (partial update). Fields absent from the POST body keep their
 * current values.
 *
 * Partial-update model
 * ────────────────────
 * The handler reads the current settings via settings_get(), applies the
 * incoming JSON fields on top, validates the merged result, then calls
 * settings_update(). This lets the frontend send only the fields the user
 * changed rather than the entire blob, which avoids stomping on concurrent
 * changes made via another endpoint (e.g. /api/connect updating WiFi creds).
 *
 * Error responses
 * ───────────────
 * All errors return application/json with an "error" string field:
 *   { "error": "drive_deadband out of range [0.0, 0.5]" }
 *
 * Reboot-required fields
 * ──────────────────────
 * Some settings (ap_ssid, ap_password, mdns_hostname) do not take effect
 * until the next boot. The POST response includes a "reboot_required" bool
 * field so the frontend can prompt the user.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Register /api/settings (GET, POST) and /api/settings/reset (POST) on
 * the given httpd instance. Call from app_server_start() after httpd_start().
 *
 * @param server  Running httpd handle.
 * @return        ESP_OK on success.
 */
esp_err_t settings_server_register(httpd_handle_t server);
