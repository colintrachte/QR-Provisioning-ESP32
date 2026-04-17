#pragma once
/**
 * app_server.h — Robot control HTTP file server and WebSocket dispatcher.
 *
 * Started after STA connects. Serves the robot control web UI from LittleFS
 * and handles the WebSocket protocol for real-time drive commands.
 *
 * WebSocket message protocol (text frames, client → server):
 *   "x:F,y:F"    Drive command. F = float -1.0…+1.0. Sent at 20 Hz.
 *   "arm"        Arm the drive system.
 *   "disarm"     Disarm; stops motors immediately.
 *   "stop"       Halt motors (sent when both axes reach zero).
 *   "led:F"      Set onboard LED brightness 0.0…1.0.
 *   "ping"       Latency probe; server replies "pong".
 *
 * WebSocket message protocol (text frames, server → client):
 *   "pong"            Reply to ping.
 *   <telemetry JSON>  Health monitor JSON blob pushed at ~2 Hz from app_task.
 *
 * Telemetry push:
 *   app_server_push_telemetry() is called from app_task. It sends the
 *   cached JSON from health_monitor_get_telemetry_json() to all open
 *   WebSocket connections.
 *
 * Client count:
 *   app_server_get_client_count() is read by prov_ui to switch between
 *   QR and plain-IP display modes on the connected screen.
 */

#include "esp_err.h"

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
 * Push the current telemetry JSON to all open WebSocket clients.
 * Call from app_task at the desired telemetry rate (5–10 Hz recommended).
 * No-op if no clients are connected.
 */
void app_server_push_telemetry(void);

/** Return the number of currently open WebSocket connections. */
int app_server_get_client_count(void);
