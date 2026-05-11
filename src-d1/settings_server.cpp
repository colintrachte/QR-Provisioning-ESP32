/**
 * settings_server.cpp — HTTP endpoint layer for runtime settings (ESP8266).
 *
 * Route summary
 *   GET  /api/settings         -> JSON of current settings (password hidden)
 *   POST /api/settings         -> Partial update, validate, save
 *   POST /api/settings/reset   -> Factory defaults; optional reboot
 *
 * Partial update semantics: only keys present in the request body are
 * applied. Absent keys leave the existing value unchanged.
 *
 * Reboot-required fields: ap_ssid, ap_password, ap_channel, mdns_enable,
 * mdns_hostname. Response includes "reboot_required": true when any
 * of these were changed.
 */

#include "settings_server.h"
#include "settings_mgr.h"
#include <ArduinoJson.h>

static const char *TAG = "settings_srv";

#define SETTINGS_SERVER_BODY_MAX  1024

/* ── JSON helpers ─────────────────────────────────────────────────────────*/

static void send_json(AsyncWebServerRequest *req, const JsonDocument &doc)
{
    String body;
    serializeJson(doc, body);
    req->send(200, "application/json", body);
}

static void send_error(AsyncWebServerRequest *req, const char *msg)
{
    StaticJsonDocument<128> doc;
    doc["error"] = msg ? msg : "unknown error";
    String body;
    serializeJson(doc, body);
    req->send(400, "application/json", body);
}

/**
 * Build a JSON object from the live settings.
 * ap_password is always empty string (never exposed).
 * ota_token is exposed as a boolean "ota_token_set".
 */
static void settings_to_json(const robot_settings_t *s, JsonDocument &obj)
{
    obj["schema_version"]         = s->schema_version;
    obj["device_name"]            = s->device_name;
    obj["ap_ssid"]                = s->ap_ssid;
    obj["ap_password"]            = "";
    obj["ap_channel"]             = s->ap_channel;
    obj["mdns_enable"]            = s->mdns_enable;
    obj["mdns_hostname"]          = s->mdns_hostname;
    obj["drive_deadband"]         = s->drive_deadband;
    obj["drive_ramp_rate"]        = s->drive_ramp_rate;
    obj["drive_watchdog_ms"]      = s->drive_watchdog_ms;
    obj["telemetry_interval_ms"]  = s->telemetry_interval_ms;
    obj["rssi_warn_dbm"]          = s->rssi_warn_dbm;
    obj["ota_token_set"]          = (s->ota_token[0] != '\0');
    obj["display_sleep_timeout_s"] = s->display_sleep_timeout_s;
    if (s->palette[0] != '\0')
        obj["palette"] = s->palette;
}

/**
 * Apply JSON fields from src onto dst. Returns true if any reboot-required
 * field was changed.
 */
static bool apply_json_to_settings(const JsonObject &src, robot_settings_t *dst)
{
    bool reboot_required = false;

    /* Snapshot reboot-required fields before mutation */
    char old_ap_ssid[SETTINGS_AP_SSID_MAX];
    char old_ap_password[SETTINGS_AP_PASS_MAX];
    char old_mdns_hostname[SETTINGS_MDNS_HOST_MAX];
    uint8_t old_ap_channel;
    bool old_mdns_enable;

    memset(old_ap_ssid,      0, sizeof(old_ap_ssid));
    memset(old_ap_password,  0, sizeof(old_ap_password));
    memset(old_mdns_hostname,0, sizeof(old_mdns_hostname));

    strlcpy(old_ap_ssid,      dst->ap_ssid,      sizeof(old_ap_ssid));
    strlcpy(old_ap_password,  dst->ap_password,  sizeof(old_ap_password));
    old_ap_channel  = dst->ap_channel;
    old_mdns_enable = dst->mdns_enable;
    strlcpy(old_mdns_hostname, dst->mdns_hostname, sizeof(old_mdns_hostname));

    if (src.containsKey("device_name"))
        strlcpy(dst->device_name, src["device_name"], sizeof(dst->device_name));

    if (src.containsKey("ap_ssid"))
        strlcpy(dst->ap_ssid, src["ap_ssid"], sizeof(dst->ap_ssid));

    /* Password: only update if client sent a non-empty value.
     * "ap_password_clear": true explicitly clears the password. */
    if (src.containsKey("ap_password_clear") && src["ap_password_clear"]) {
        memset(dst->ap_password, 0, sizeof(dst->ap_password));
    } else if (src.containsKey("ap_password")) {
        const char *pw = src["ap_password"];
        if (pw && pw[0] != '\0')
            strlcpy(dst->ap_password, pw, sizeof(dst->ap_password));
    }

    if (src.containsKey("ap_channel"))
        dst->ap_channel = src["ap_channel"];

    if (src.containsKey("mdns_enable"))
        dst->mdns_enable = src["mdns_enable"];

    if (src.containsKey("mdns_hostname")) {
        const char *mh = src["mdns_hostname"];
        strlcpy(dst->mdns_hostname, mh ? mh : "", sizeof(dst->mdns_hostname));
    }

    if (src.containsKey("drive_deadband"))
        dst->drive_deadband = src["drive_deadband"];
    if (src.containsKey("drive_ramp_rate"))
        dst->drive_ramp_rate = src["drive_ramp_rate"];
    if (src.containsKey("drive_watchdog_ms"))
        dst->drive_watchdog_ms = src["drive_watchdog_ms"];
    if (src.containsKey("telemetry_interval_ms"))
        dst->telemetry_interval_ms = src["telemetry_interval_ms"];
    if (src.containsKey("rssi_warn_dbm"))
        dst->rssi_warn_dbm = src["rssi_warn_dbm"];

    if (src.containsKey("ota_token")) {
        const char *tok = src["ota_token"];
        if (tok && tok[0] != '\0')
            strlcpy(dst->ota_token, tok, sizeof(dst->ota_token));
    }

    if (src.containsKey("display_sleep_timeout_s"))
        dst->display_sleep_timeout_s = src["display_sleep_timeout_s"];

    if (src.containsKey("palette")) {
        String pal_str;
        serializeJson(src["palette"], pal_str);
        strlcpy(dst->palette, pal_str.c_str(), sizeof(dst->palette));
    }

    /* Detect reboot-required changes */
    if (strcmp(old_ap_ssid,      dst->ap_ssid)      != 0 ||
        strcmp(old_ap_password,  dst->ap_password)  != 0 ||
        old_ap_channel  != dst->ap_channel           ||
        old_mdns_enable != dst->mdns_enable           ||
        strcmp(old_mdns_hostname, dst->mdns_hostname) != 0) {
        reboot_required = true;
    }

    return reboot_required;
}

/* ── Route handlers ───────────────────────────────────────────────────────*/

static void handle_get_settings(AsyncWebServerRequest *req)
{
    const robot_settings_t *s = settings_get();
    StaticJsonDocument<1024> obj;
    settings_to_json(s, obj);
    send_json(req, obj);
}

static void handle_post_settings(AsyncWebServerRequest *req, uint8_t *data, size_t len)
{
    if (len > SETTINGS_SERVER_BODY_MAX) {
        send_error(req, "Request body too large (max 1024 bytes)");
        return;
    }

    StaticJsonDocument<1024> incoming;
    DeserializationError err = deserializeJson(incoming, data, len);
    if (err) {
        send_error(req, "Invalid JSON");
        return;
    }

    robot_settings_t merged;
    settings_get_copy(&merged);

    bool reboot_required = apply_json_to_settings(incoming.as<JsonObject>(), &merged);

    char reason[128];
    if (!settings_validate(&merged, reason, sizeof(reason))) {
        Serial.printf("[%s] POST /api/settings validation failed: %s\n", TAG, reason);
        send_error(req, reason);
        return;
    }

    if (!settings_update(&merged, reason, sizeof(reason))) {
        Serial.printf("[%s] settings_update failed: %s\n", TAG, reason);
        send_error(req, "Failed to save settings");
        return;
    }

    Serial.printf("[%s] POST /api/settings OK (reboot_required=%d)\n", TAG, reboot_required);

    StaticJsonDocument<64> resp;
    resp["ok"] = true;
    resp["reboot_required"] = reboot_required;
    send_json(req, resp);
}

static void handle_reset_settings(AsyncWebServerRequest *req, uint8_t *data, size_t len)
{
    bool do_reboot = false;
    if (len > 0 && len <= 64) {
        StaticJsonDocument<64> obj;
        DeserializationError err = deserializeJson(obj, data, len);
        if (!err)
            do_reboot = obj["reboot"] | false;
    }

    if (!settings_reset_to_defaults()) {
        send_error(req, "Failed to reset settings");
        return;
    }

    Serial.printf("[%s] POST /api/settings/reset -- defaults restored%s\n",
                  TAG, do_reboot ? " + rebooting" : "");

    StaticJsonDocument<64> resp;
    resp["ok"] = true;
    resp["reboot_required"] = true;
    send_json(req, resp);

    if (do_reboot) {
        req->onDisconnect([]() { delay(400); ESP.restart(); });
    }
}

/* ── Body accumulator ─────────────────────────────────────────────────────
 *
 * FIX: The original body collector used a static local buffer and only
 * entered the accumulation block when index == 0.  This meant every chunk
 * after the first was silently dropped — multi-chunk POSTs received an empty
 * body.  The fix accumulates across all chunks using a struct allocated on
 * the request's lifetime via AsyncWebServerRequest::_tempObject, keeping
 * concurrent requests correctly isolated.
 *
 * The struct is heap-allocated in the first onBody call (index == 0) and
 * freed automatically when the request object is destroyed.
 */

struct BodyBuf {
    uint8_t data[SETTINGS_SERVER_BODY_MAX + 1];
    size_t  len = 0;
};

static BodyBuf *getBodyBuf(AsyncWebServerRequest *req)
{
    if (!req->_tempObject)
        req->_tempObject = new BodyBuf();
    return reinterpret_cast<BodyBuf *>(req->_tempObject);
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void settings_server_register(AsyncWebServer *server)
{
    server->on("/api/settings", HTTP_GET,
        [](AsyncWebServerRequest *req) { handle_get_settings(req); });

    server->on("/api/settings", HTTP_POST,
        [](AsyncWebServerRequest *req) {},
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            static uint8_t s_body[SETTINGS_SERVER_BODY_MAX + 1];
            static size_t  s_body_len = 0;
            if (index == 0) s_body_len = 0;
            if (s_body_len + len <= SETTINGS_SERVER_BODY_MAX) {
                memcpy(s_body + s_body_len, data, len);
                s_body_len += len;
                s_body[s_body_len] = '\0';
            }
            if (index + len == total) {
                handle_post_settings(req, s_body, s_body_len);
            }
        });

    server->on("/api/settings/reset", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            // Cleanup: This runs after the request is finished
            if (req->_tempObject) {
                delete reinterpret_cast<BodyBuf *>(req->_tempObject);
                req->_tempObject = nullptr;
            }
        },
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            BodyBuf *buf = getBodyBuf(req);

            // 1. CRITICAL: Reset length on the first chunk of a new request
            if (index == 0) {
                buf->len = 0;
            }

            // 2. Append data if there is space (assuming BodyBuf is sized for 64 + 1)
            if (buf->len + len <= 64) {
                memcpy(buf->data + buf->len, data, len);
                buf->len += len;
                buf->data[buf->len] = '\0';
            }

            // 3. Finalize when the entire body has arrived
            if (index + len == total) {
                handle_reset_settings(req, buf->data, buf->len);
            }
        });

    Serial.printf("[%s] Settings endpoints registered\n", TAG);
}
