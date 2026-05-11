/**
 * settings_mgr.h — Runtime settings singleton for ESP8266 (Arduino).
 *
 * Ported from ESP-IDF settings_mgr.c. Uses LittleFS + ArduinoJson instead
 * of NVS + cJSON. All string lengths and field names match the S3 schema
 * so the HTTP API is wire-compatible.
 */

#pragma once
#include <Arduino.h>

#define SETTINGS_SCHEMA_VERSION       2
#define SETTINGS_DEVICE_NAME_MAX      32
#define SETTINGS_AP_SSID_MAX          32
#define SETTINGS_AP_PASS_MAX          64
#define SETTINGS_MDNS_HOST_MAX        32
#define SETTINGS_OTA_TOKEN_MAX        64
#define SETTINGS_PALETTE_MAX          256

struct robot_settings_t {
    int         schema_version;
    char        device_name[SETTINGS_DEVICE_NAME_MAX];
    char        ap_ssid[SETTINGS_AP_SSID_MAX];
    char        ap_password[SETTINGS_AP_PASS_MAX];
    uint8_t     ap_channel;
    bool        mdns_enable;
    char        mdns_hostname[SETTINGS_MDNS_HOST_MAX];
    float       drive_deadband;
    float       drive_ramp_rate;
    uint32_t    drive_watchdog_ms;
    uint32_t    telemetry_interval_ms;
    int         rssi_warn_dbm;
    char        ota_token[SETTINGS_OTA_TOKEN_MAX];
    uint32_t    display_sleep_timeout_s;
    char        palette[SETTINGS_PALETTE_MAX];
};

bool settings_load(void);
bool settings_save(void);
bool settings_update(const robot_settings_t *src, char *err_reason, size_t err_len);
bool settings_reset_to_defaults(void);
robot_settings_t* settings_get(void);
void settings_get_copy(robot_settings_t *dst);
bool settings_validate(const robot_settings_t *s, char *err_buf, size_t err_buf_len);
