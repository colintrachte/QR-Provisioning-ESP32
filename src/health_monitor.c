/**
 * health_monitor.c — System health aggregator.
 *
 * I2C scanning has moved to i_sensors.c. This module reads from
 * i_sensors_get_data(), i_battery_percent(), and the WiFi driver.
 * It then formats a JSON telemetry blob for app_server to push.
 *
 * JSON
 * ────
 * The telemetry blob is built with cJSON rather than hand-rolled snprintf.
 * cJSON handles NaN/null for the temperature field cleanly, and escapes any
 * future string fields automatically.
 *
 * Double-buffer
 * ─────────────
 * health_monitor_tick() (main-loop task) writes into the inactive buffer,
 * then flips s_json_active in a single volatile int store (atomic on
 * ESP32-S3). app_server_push_telemetry() (httpd task) always reads from
 * s_json[s_json_active]. This eliminates the race where the reader could
 * observe a half-written result.
 *
 * The worst case is the reader seeing the previous complete JSON frame
 * (one telemetry period stale) — acceptable for 5 Hz telemetry.
 */

#include "health_monitor.h"
#include "i_sensors.h"
#include "i_battery.h"
#include "wifi_manager.h"
#include "utils_json.h"
#include "settings_mgr.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "health";

static int8_t   s_rssi          = 0;
static uint32_t s_last_rssi_ms  = 0;

/* Double-buffered serialised JSON — see module comment above. */
static char         s_json[2][HEALTH_JSON_BUF_LEN];
static volatile int s_json_active = 0;

/* ── RSSI ────────────────────────────────────────────────────────────────── */

static void check_rssi(void)
{
    if (!wifi_manager_is_connected()) return;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;

    s_rssi = ap.rssi;

    int warn_dbm = settings_get()->rssi_warn_dbm;
    if (s_rssi < warn_dbm)
        ESP_LOGW(TAG, "Weak WiFi: %d dBm (threshold %d)", s_rssi, warn_dbm);
    else if (DEBUG_HEALTH)
        ESP_LOGD(TAG, "RSSI: %d dBm", s_rssi);
}

/* ── Telemetry JSON builder ─────────────────────────────────────────────────
 *
 * Shape (matches what index.js already parses):
 *   { "rssi": -65, "battery": 80, "temp": 23.4, "uptime": 123, "heap": 200000, "errors": 0 }
 *   "temp" is JSON null when no sensor is available (isnan).
 *
 * We write into the inactive buffer then atomically publish by flipping
 * s_json_active. The httpd task always reads s_json[s_json_active] and will
 * see either the previous complete frame or the new one — never a partial write.
 */
static void build_json(void)
{
    i_sensor_data_t sd      = i_sensors_get_data();
    uint8_t         bat_pct = i_battery_percent();
    uint32_t uptime_s = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    uint32_t heap     = (uint32_t)esp_get_free_heap_size();

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return;   /* OOM — keep serving the previous frame */

    cJSON_AddNumberToObject(obj, "rssi",    s_rssi);
    cJSON_AddNumberToObject(obj, "battery", bat_pct);

    /* Temperature: emit JSON null when no sensor present */
    if (isnan(sd.temperature_c))
        cJSON_AddNullToObject(obj, "temp");
    else
        cJSON_AddNumberToObject(obj, "temp", (double)sd.temperature_c);

    cJSON_AddNumberToObject(obj, "uptime", (double)uptime_s);
    cJSON_AddNumberToObject(obj, "heap",   (double)heap);
    cJSON_AddNumberToObject(obj, "errors", 0);

    int write_idx = 1 - s_json_active;

    char *serialised = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (!serialised) return;   /* OOM — keep previous frame */

    /* Guard against overflow — HEALTH_JSON_BUF_LEN must be large enough.
     * 128 B is sufficient for the current field set; log a warning if not. */
    size_t len = strlen(serialised);
    if (len >= HEALTH_JSON_BUF_LEN) {
        ESP_LOGW(TAG, "Telemetry JSON truncated (%u >= %d)",
                 (unsigned)len, HEALTH_JSON_BUF_LEN);
        len = HEALTH_JSON_BUF_LEN - 1;
    }
    memcpy(s_json[write_idx], serialised, len);
    s_json[write_idx][len] = '\0';
    free(serialised);

    s_json_active = write_idx;   /* atomic int store — publishes the new frame */
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void health_monitor_init(void)
{
    const i_peripheral_map_t *pm = i_sensors_get_peripheral_map();
    ESP_LOGI(TAG, "health_monitor_init — OLED: %s",
             pm->oled ? "PRESENT" : "ABSENT");
    build_json();
}

void health_monitor_tick(void)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now - s_last_rssi_ms >= settings_get()->telemetry_interval_ms) {
        s_last_rssi_ms = now;
        check_rssi();
    }
    build_json();
}

int8_t health_monitor_get_rssi(void)
{
    return s_rssi;
}

const char *health_monitor_get_telemetry_json(void)
{
    return s_json[s_json_active];
}
