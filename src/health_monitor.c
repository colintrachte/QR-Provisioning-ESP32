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

/* ESP32-S3 internal temperature sensor
 * ─────────────────────────────────────
 * Available on ESP32-S3, C3, C6, H2 via driver/temperature_sensor.h (IDF 5.x).
 * On targets without this driver the #ifdef guard compiles it out cleanly;
 * the "temp" JSON field then falls back to i_sensors data (NAN until an
 * external sensor is wired in).
 *
 * Range config: -10 °C to 80 °C covers normal indoor operating temperatures
 * and gives ±0.5 °C accuracy. Widen if you expect extreme ambients.
 */
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) \
 || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
#  define HEALTH_HAS_INTERNAL_TEMP 1
#  include "driver/temperature_sensor.h"
static temperature_sensor_handle_t s_tsens = NULL;
#else
#  define HEALTH_HAS_INTERNAL_TEMP 0
#endif

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

    /* Temperature priority:
     *   1. Internal ESP32-S3 sensor (always present, no wiring required).
     *   2. External I2C sensor via i_sensors (future BME280 / SHT31 etc.).
     *   3. NAN → JSON null (no sensor available).
     *
     * The internal sensor reflects die temperature which runs ~5–15 °C above
     * ambient at idle and more under motor load. This is still useful for
     * detecting thermal stress even if it does not represent ambient. */
    float temperature = NAN;

#if HEALTH_HAS_INTERNAL_TEMP
    if (s_tsens != NULL) {
        float tsens_out = NAN;
        esp_err_t err = temperature_sensor_get_celsius(s_tsens, &tsens_out);
        if (err == ESP_OK && !isnan(tsens_out)) {
            temperature = tsens_out;
        } else if (err != ESP_OK) {
            ESP_LOGD(TAG, "temperature_sensor_get_celsius: %s", esp_err_to_name(err));
        }
    }
#endif

    /* Fall back to external sensor if internal is unavailable */
    if (isnan(temperature) && !isnan(sd.temperature_c)) {
        temperature = sd.temperature_c;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return;   /* OOM — keep serving the previous frame */

    cJSON_AddNumberToObject(obj, "rssi",    s_rssi);
    cJSON_AddNumberToObject(obj, "battery", bat_pct);

    if (isnan(temperature))
        cJSON_AddNullToObject(obj, "temp");
    else
        cJSON_AddNumberToObject(obj, "temp", (double)temperature);

    cJSON_AddNumberToObject(obj, "uptime", (double)uptime_s);
    cJSON_AddNumberToObject(obj, "heap",   (double)heap);
    cJSON_AddNumberToObject(obj, "errors", 0);

    int write_idx = 1 - s_json_active;

    char *serialised = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (!serialised) return;

    size_t len = strlen(serialised);
    if (len >= HEALTH_JSON_BUF_LEN) {
        ESP_LOGW(TAG, "Telemetry JSON truncated (%u >= %d)",
                 (unsigned)len, HEALTH_JSON_BUF_LEN);
        len = HEALTH_JSON_BUF_LEN - 1;
    }
    memcpy(s_json[write_idx], serialised, len);
    s_json[write_idx][len] = '\0';
    free(serialised);

    s_json_active = write_idx;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void health_monitor_init(void)
{
    const i_peripheral_map_t *pm = i_sensors_get_peripheral_map();
    ESP_LOGI(TAG, "health_monitor_init — OLED: %s",
             pm->oled ? "PRESENT" : "ABSENT");

#if HEALTH_HAS_INTERNAL_TEMP
    temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&tsens_cfg, &s_tsens);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_tsens);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "temperature_sensor_enable failed: %s", esp_err_to_name(err));
            temperature_sensor_uninstall(s_tsens);
            s_tsens = NULL;
        } else {
            ESP_LOGI(TAG, "Internal temperature sensor enabled");
        }
    } else {
        ESP_LOGW(TAG, "temperature_sensor_install failed: %s", esp_err_to_name(err));
        s_tsens = NULL;
    }
#endif

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
