/**
 * health_monitor.cpp — System health aggregator for ESP8266.
 *
 * Double-buffered serialised JSON:
 *   health_monitor_tick() writes into the inactive buffer,
 *   then flips s_json_active with interrupts disabled (atomic on single core).
 *   The WebSocket reader always reads s_json[s_json_active].
 *
 * Shape (wire-compatible with S3):
 *   { "rssi": -65, "battery": 80, "temp": null, "uptime": 123,
 *     "heap": 20000, "errors": 0 }
 */

#include "health_monitor.h"
#include "settings_mgr.h"
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>

/* External battery function -- provided by i_battery.cpp */
extern uint8_t i_battery_percent(void);

static const char *TAG = "health";

static int8_t   s_rssi          = 0;
static uint32_t s_last_rssi_ms  = 0;
static uint32_t s_last_build_ms = 0;

static char         s_json[2][HEALTH_JSON_BUF_LEN];
static volatile int s_json_active = 0;

static void check_rssi(void)
{
    if (WiFi.status() != WL_CONNECTED) return;
    s_rssi = WiFi.RSSI();
    int warn_dbm = settings_get()->rssi_warn_dbm;
    if (s_rssi < warn_dbm)
        Serial.printf("[%s] Weak WiFi: %d dBm (threshold %d)\n", TAG, s_rssi, warn_dbm);
}

static void build_json(void)
{
    uint8_t  bat_pct  = i_battery_percent();
    uint32_t uptime_s = millis() / 1000;
    uint32_t heap     = ESP.getFreeHeap();

    StaticJsonDocument<256> doc;
    doc["rssi"]    = s_rssi;
    doc["battery"] = bat_pct;
    doc["temp"]    = nullptr;   /* ESP8266 has no internal temp sensor */
    doc["uptime"]  = uptime_s;
    doc["heap"]    = heap;
    doc["errors"]  = 0;

    int write_idx = 1 - s_json_active;

    size_t n = serializeJson(doc, s_json[write_idx], HEALTH_JSON_BUF_LEN);
    if (n >= HEALTH_JSON_BUF_LEN - 1) {
        Serial.printf("[%s] Telemetry JSON truncated\n", TAG);
        s_json[write_idx][HEALTH_JSON_BUF_LEN - 1] = '\0';
    }

    noInterrupts();
    s_json_active = write_idx;
    interrupts();
}

void health_monitor_init(void)
{
    Serial.printf("[%s] health_monitor_init\n", TAG);
    build_json();
}

void health_monitor_tick(void)
{
    uint32_t now = millis();
    if (now - s_last_rssi_ms >= settings_get()->telemetry_interval_ms) {
        s_last_rssi_ms = now;
        check_rssi();
    }
    if (now - s_last_build_ms >= settings_get()->telemetry_interval_ms) {
        s_last_build_ms = now;
        build_json();
    }
}

int8_t health_monitor_get_rssi(void)
{
    return s_rssi;
}

const char* health_monitor_get_telemetry_json(void)
{
    return s_json[s_json_active];
}
