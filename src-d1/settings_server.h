/**
 * settings_server.h — HTTP endpoint layer for runtime settings (ESP8266).
 *
 * Registers routes on an existing AsyncWebServer instance.
 * Wire-compatible with the ESP32-S3 /api/settings API.
 */

#pragma once
#include <ESPAsyncWebServer.h>

/**
 * Register GET /api/settings, POST /api/settings, POST /api/settings/reset
 * on the provided server instance.
 */
void settings_server_register(AsyncWebServer *server);
