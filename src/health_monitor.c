/**
 * health_monitor.c — System health aggregator.
 *
 * I2C scanning has moved to i_sensors.c. This module reads from
 * i_sensors_get_data(), i_battery_percent(), and the WiFi driver.
 * It then formats a JSON telemetry blob for app_server to push.
 */

#include "health_monitor.h"
#include "i_sensors.h"
#include "i_battery.h"
#include "wifi_manager.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "health";

static int8_t  s_rssi             = 0;
static uint32_t s_last_rssi_ms    = 0;
static char    s_json[HEALTH_JSON_BUF_LEN];

/* ── RSSI ───────────────────────────────────────────────────────────────────*/

static void check_rssi(void)
{
    if (!wifi_manager_is_connected()) return;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;

    s_rssi = ap.rssi;

    if (s_rssi < HEALTH_RSSI_WARN_DBM)
        ESP_LOGW(TAG, "Weak WiFi: %d dBm (threshold %d)", s_rssi, HEALTH_RSSI_WARN_DBM);
    else if (DEBUG_HEALTH)
        ESP_LOGD(TAG, "RSSI: %d dBm", s_rssi);
}

/* ── Telemetry JSON builder ─────────────────────────────────────────────────
 * Matches the JSON shape script.js already parses. Extend fields here as
 * new sensors are wired up in i_sensors.
 */
static void build_json(void)
{
    i_sensor_data_t sd  = i_sensors_get_data();
    uint8_t battery_pct = i_battery_percent();
    uint32_t uptime_s   = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    uint32_t heap       = (uint32_t)esp_get_free_heap_size();

    /* Temperature: emit null if no sensor */
    char temp_str[16];
    if (isnan(sd.temperature_c))
        snprintf(temp_str, sizeof(temp_str), "null");
    else
        snprintf(temp_str, sizeof(temp_str), "%.1f", sd.temperature_c);

    snprintf(s_json, sizeof(s_json),
        "{"
        "\"rssi\":%d,"
        "\"battery\":%u,"
        "\"temp\":%s,"
        "\"uptime\":%lu,"
        "\"heap\":%lu,"
        "\"errors\":0"
        "}",
        s_rssi, battery_pct, temp_str,
        (unsigned long)uptime_s, (unsigned long)heap);
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

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
    if (now - s_last_rssi_ms >= (uint32_t)HEALTH_SCAN_INTERVAL_MS) {
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
    return s_json;
}
