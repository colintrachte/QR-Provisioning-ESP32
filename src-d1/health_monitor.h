/**
 * health_monitor.h — System health aggregator for ESP8266.
 *
 * Builds a JSON telemetry blob for the WebSocket push.
 * Double-buffered so the reader never sees a torn write.
 */

#pragma once
#include <Arduino.h>

#define HEALTH_JSON_BUF_LEN  256

void health_monitor_init(void);
void health_monitor_tick(void);
int8_t health_monitor_get_rssi(void);
const char* health_monitor_get_telemetry_json(void);
