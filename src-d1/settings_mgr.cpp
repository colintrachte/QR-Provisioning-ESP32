/**
 * settings_mgr.cpp — NVS-backed JSON configuration store (ESP8266 port).
 *
 * Replaces ESP-IDF NVS + cJSON with LittleFS + ArduinoJson v6.
 * Uses StaticJsonDocument<1024> to avoid heap fragmentation on the ESP8266.
 *
 * Schema compatibility: field names and semantics are identical to the
 * ESP32-S3 version so the web UI (settings.js) needs no changes.
 */

#include "settings_mgr.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

static robot_settings_t  s_settings;
static bool              s_initialized = false;

/* ── Compile-time defaults ──────────────────────────────────────────────────
 * These mirror the #defines that config.h used to provide. Once all
 * hardware targets read from settings_get(), config.h becomes build-only
 * (partition names, pinouts) and these literals are the factory values.
 */
static void apply_defaults(robot_settings_t *s)
{
    memset(s, 0, sizeof(*s));

    s->schema_version = SETTINGS_SCHEMA_VERSION;

    strncpy(s->device_name, "RC-Tank", sizeof(s->device_name) - 1);
    strncpy(s->ap_ssid,     "RC-Tank-Setup", sizeof(s->ap_ssid) - 1);
    /* ap_password empty = open AP */
    s->ap_channel = 1;

    s->mdns_enable = true;
    strncpy(s->mdns_hostname, "rc-tank", sizeof(s->mdns_hostname) - 1);

    s->drive_deadband      = 0.05f;
    s->drive_ramp_rate     = 0.05f;
    s->drive_watchdog_ms   = 500;
    s->telemetry_interval_ms = 200;
    s->rssi_warn_dbm       = -75;
    /* ota_token empty = open access */
    s->display_sleep_timeout_s = 0;   /* 0 = never sleep */
}

/* ── Validation ────────────────────────────────────────────────────────────*/

bool settings_validate(const robot_settings_t *s, char *err_buf, size_t err_buf_len)
{
    auto fail = [&](const char *msg) {
        if (err_buf && err_buf_len)
            strlcpy(err_buf, msg, err_buf_len);
        return false;
    };

    if (!s) return fail("null settings pointer");

    if (s->device_name[0] == '\0')
        return fail("device_name must not be empty");
    if (strnlen(s->device_name, sizeof(s->device_name)) == sizeof(s->device_name))
        return fail("device_name exceeds max length");

    if (s->ap_ssid[0] == '\0')
        return fail("ap_ssid must not be empty");
    if (strnlen(s->ap_ssid, sizeof(s->ap_ssid)) == sizeof(s->ap_ssid))
        return fail("ap_ssid exceeds max length");

    size_t pass_len = strnlen(s->ap_password, sizeof(s->ap_password));
    if (pass_len > 0 && pass_len < 8)
        return fail("ap_password must be empty (open) or >= 8 chars");
    if (pass_len == sizeof(s->ap_password))
        return fail("ap_password exceeds max length");

    if (s->ap_channel > 13)
        return fail("ap_channel must be 0 (auto) or 1-13");

    if (s->mdns_enable) {
        if (s->mdns_hostname[0] == '\0')
            return fail("mdns_hostname must not be empty when mdns_enable is true");
        if (strnlen(s->mdns_hostname, sizeof(s->mdns_hostname)) == sizeof(s->mdns_hostname))
            return fail("mdns_hostname exceeds max length");
    }

    if (s->drive_deadband < 0.0f || s->drive_deadband > 0.5f)
        return fail("drive_deadband out of range [0.0, 0.5]");
    if (isnan(s->drive_deadband) || isinf(s->drive_deadband))
        return fail("drive_deadband is NaN or Inf");

    if (s->drive_ramp_rate <= 0.0f || s->drive_ramp_rate > 1.0f)
        return fail("drive_ramp_rate out of range (0.0, 1.0]");
    if (isnan(s->drive_ramp_rate) || isinf(s->drive_ramp_rate))
        return fail("drive_ramp_rate is NaN or Inf");

    if (s->drive_watchdog_ms < 100 || s->drive_watchdog_ms > 10000)
        return fail("drive_watchdog_ms out of range [100, 10000]");

    if (s->telemetry_interval_ms < 50 || s->telemetry_interval_ms > 10000)
        return fail("telemetry_interval_ms out of range [50, 10000]");

    if (s->rssi_warn_dbm < -120 || s->rssi_warn_dbm > 0)
        return fail("rssi_warn_dbm out of range [-120, 0]");

    if (strnlen(s->ota_token, sizeof(s->ota_token)) == sizeof(s->ota_token))
        return fail("ota_token exceeds max length");

    if (s->display_sleep_timeout_s > 3600)
        return fail("display_sleep_timeout_s out of range [0, 3600]");

    return true;
}

/* ── Serialise / deserialise ──────────────────────────────────────────────*/

static bool serialise(const robot_settings_t *s, char *out_buf, size_t out_len)
{
    StaticJsonDocument<1024> doc;
    doc["schema_version"]         = s->schema_version;
    doc["device_name"]            = s->device_name;
    doc["ap_ssid"]                = s->ap_ssid;
    doc["ap_password"]            = s->ap_password;
    doc["ap_channel"]             = s->ap_channel;
    doc["mdns_enable"]            = s->mdns_enable;
    doc["mdns_hostname"]          = s->mdns_hostname;
    doc["drive_deadband"]         = s->drive_deadband;
    doc["drive_ramp_rate"]        = s->drive_ramp_rate;
    doc["drive_watchdog_ms"]      = s->drive_watchdog_ms;
    doc["telemetry_interval_ms"]  = s->telemetry_interval_ms;
    doc["rssi_warn_dbm"]          = s->rssi_warn_dbm;
    doc["ota_token"]              = s->ota_token;
    doc["display_sleep_timeout_s"] = s->display_sleep_timeout_s;

    if (s->palette[0] != '\0')
        doc["palette"] = s->palette;   /* stored as string; UI re-parses */

    size_t n = serializeJson(doc, out_buf, out_len);
    return (n > 0 && n < out_len - 1);
}

static void deserialise(const char *json_str, robot_settings_t *dst)
{
    /* Start from defaults so any missing keys are already correct */
    apply_defaults(dst);

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, json_str);
    if (err) {
        Serial.printf("[settings_mgr] JSON parse failed: %s -- using defaults\n",
                      err.c_str());
        return;
    }

    dst->schema_version = doc["schema_version"] | SETTINGS_SCHEMA_VERSION;
    strlcpy(dst->device_name,     doc["device_name"]     | "RC-Tank",       sizeof(dst->device_name));
    strlcpy(dst->ap_ssid,         doc["ap_ssid"]         | "RC-Tank-Setup", sizeof(dst->ap_ssid));
    strlcpy(dst->ap_password,     doc["ap_password"]     | "",              sizeof(dst->ap_password));
    dst->ap_channel = doc["ap_channel"] | 1;
    dst->mdns_enable = doc["mdns_enable"] | true;
    strlcpy(dst->mdns_hostname,   doc["mdns_hostname"]   | "rc-tank",       sizeof(dst->mdns_hostname));
    dst->drive_deadband      = doc["drive_deadband"]      | 0.05f;
    dst->drive_ramp_rate     = doc["drive_ramp_rate"]     | 0.05f;
    dst->drive_watchdog_ms   = doc["drive_watchdog_ms"]   | 500;
    dst->telemetry_interval_ms = doc["telemetry_interval_ms"] | 200;
    dst->rssi_warn_dbm       = doc["rssi_warn_dbm"]       | -75;
    strlcpy(dst->ota_token,       doc["ota_token"]       | "",              sizeof(dst->ota_token));
    dst->display_sleep_timeout_s = doc["display_sleep_timeout_s"] | 0;

    const char *pal = doc["palette"];
    if (pal) {
        strlcpy(dst->palette, pal, sizeof(dst->palette));
    }
}

/* ── Public API ────────────────────────────────────────────────────────────*/

bool settings_load(void)
{
    if (!LittleFS.exists("/settings.json")) {
        Serial.println("[settings_mgr] /settings.json not found -- using defaults");
        apply_defaults(&s_settings);
        s_initialized = true;
        return settings_save();
    }

    File f = LittleFS.open("/settings.json", "r");
    if (!f) {
        Serial.println("[settings_mgr] Failed to open /settings.json -- using defaults");
        apply_defaults(&s_settings);
        s_initialized = true;
        return false;
    }

    size_t len = f.size();
    if (len > 1024) len = 1024;   /* sanity cap */
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        f.close();
        apply_defaults(&s_settings);
        s_initialized = true;
        return false;
    }
    f.readBytes(buf, len);
    buf[len] = '\0';
    f.close();

    deserialise(buf, &s_settings);
    free(buf);

    if (s_settings.schema_version < SETTINGS_SCHEMA_VERSION) {
        Serial.printf("[settings_mgr] Schema migration: v%d -> v%d\n",
                      s_settings.schema_version, SETTINGS_SCHEMA_VERSION);
        s_settings.schema_version = SETTINGS_SCHEMA_VERSION;
        settings_save();
    }

    char reason[64];
    if (!settings_validate(&s_settings, reason, sizeof(reason))) {
        Serial.printf("[settings_mgr] Stored settings invalid (%s) -- resetting to defaults\n", reason);
        apply_defaults(&s_settings);
        settings_save();
    }

    s_initialized = true;
    Serial.println("[settings_mgr] Settings loaded");
    Serial.printf("  device_name:      %s\n", s_settings.device_name);
    Serial.printf("  ap_ssid:          %s\n", s_settings.ap_ssid);
    Serial.printf("  mdns:             %s -> %s.local\n",
                  s_settings.mdns_enable ? "on" : "off", s_settings.mdns_hostname);
    Serial.printf("  drive_deadband:   %.3f\n", s_settings.drive_deadband);
    Serial.printf("  drive_ramp_rate:  %.3f\n", s_settings.drive_ramp_rate);
    Serial.printf("  drive_watchdog:   %lu ms\n", s_settings.drive_watchdog_ms);
    Serial.printf("  telemetry_ms:     %lu\n", s_settings.telemetry_interval_ms);
    Serial.printf("  rssi_warn_dbm:    %d\n", s_settings.rssi_warn_dbm);
    return true;
}

bool settings_save(void)
{
    s_settings.schema_version = SETTINGS_SCHEMA_VERSION;

    char buf[1024];
    if (!serialise(&s_settings, buf, sizeof(buf))) {
        Serial.println("[settings_mgr] Serialisation failed");
        return false;
    }

    File f = LittleFS.open("/settings.json", "w");
    if (!f) {
        Serial.println("[settings_mgr] Failed to open /settings.json for writing");
        return false;
    }
    f.print(buf);
    f.close();

    Serial.println("[settings_mgr] Settings saved to LittleFS");
    return true;
}

bool settings_update(const robot_settings_t *src, char *err_reason, size_t err_len)
{
    if (!src) {
        if (err_reason && err_len) strlcpy(err_reason, "null pointer", err_len);
        return false;
    }
    if (!settings_validate(src, err_reason, err_len))
        return false;

    memcpy(&s_settings, src, sizeof(s_settings));
    return settings_save();
}

bool settings_reset_to_defaults(void)
{
    apply_defaults(&s_settings);
    Serial.println("[settings_mgr] Reset to factory defaults");
    return settings_save();
}

robot_settings_t* settings_get(void)
{
    if (!s_initialized) {
        Serial.println("[settings_mgr] WARNING: settings_get called before settings_load -- using defaults");
        static robot_settings_t emergency;
        apply_defaults(&emergency);
        return &emergency;
    }
    return &s_settings;
}

void settings_get_copy(robot_settings_t *dst)
{
    if (!dst) return;
    if (!s_initialized) {
        apply_defaults(dst);
        return;
    }
    noInterrupts();
    memcpy(dst, &s_settings, sizeof(*dst));
    interrupts();
}
