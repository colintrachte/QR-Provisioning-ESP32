#pragma once
/**
 * app_server.h — Robot control HTTP file server and WebSocket dispatcher.
 *
 * Started after STA connects. Serves the robot control web UI from LittleFS
 * and handles the WebSocket protocol for real-time drive commands.
 * ...
 */

#include "esp_err.h"
#include <stdbool.h>

/** * Callback type used when new WiFi credentials are received via the provisioning page.
 */
typedef void (*app_server_creds_cb_t)(const char *ssid, const char *password);

/**
 * Start the HTTP file server and WebSocket handler on port 80.
 * Call after wifi_manager_start() succeeds and portal_stop() has been called.
 * @return ESP_OK or an esp_err_t on httpd_start failure.
 */
esp_err_t app_server_start(void);

/**
 * Stop the server and close all WebSocket connections.
 * Calls ctrl_drive_emergency_stop() before tearing down.
 */
void app_server_stop(void);

/**
 * Registers provisioning-only routes (WiFi API, captive portal probes)
 * and starts the DNS hijack task.
 */
void app_server_enter_provisioning_mode(app_server_creds_cb_t creds_cb);

/**
 * Unregisters provisioning routes and stops the DNS hijack task.
 * Returns the server to its NORMAL operating mode.
 */
void app_server_exit_provisioning_mode(void);

/**
 * Push the current telemetry JSON to all open WebSocket clients.
 * Call from app_task at the desired telemetry rate (5–10 Hz recommended).
 * No-op if no clients are connected.
 */
void app_server_push_telemetry(void);

/** * Pushes the current arming status to all clients.
 * If armed is false, an optional reason can be provided.
 */
void app_server_push_arm_state(bool armed, const char *reason);

/** Return the number of currently open WebSocket connections. */
int app_server_get_client_count(void);
