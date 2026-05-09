/**
 * settings_server.c — HTTP endpoint layer for runtime settings.
 *
 * Route summary
 * ─────────────
 *   GET  /api/settings         Serialise current settings to JSON. 200 OK.
 *   POST /api/settings         Partial-update: merge body into current,
 *                              validate merged result, save. 200 OK or 400.
 *   POST /api/settings/reset   Reset to factory defaults. 200 OK.
 *                              Body: { "reboot": true } triggers esp_restart().
 *
 * Partial update semantics
 * ────────────────────────
 * The POST handler starts from the current settings struct (not defaults).
 * It overlays only the keys that appear in the request body. This means:
 *   - The client can send { "drive_deadband": 0.08 } without needing to
 *     echo back the full settings object.
 *   - Fields left out of the body are unchanged.
 * Validation runs on the merged result, not the delta alone, so invariants
 * that span fields (e.g. password length) are checked in full context.
 *
 * Reboot-required fields
 * ──────────────────────
 * Changes to ap_ssid, ap_password, ap_channel, mdns_enable, mdns_hostname
 * require a reboot to take effect (WiFi stack is already running). The POST
 * response includes "reboot_required": true when any of these were changed.
 * The /api/settings/reset handler always sets "reboot_required": true.
 *
 * Body size limit
 * ───────────────
 * POST body is capped at SETTINGS_SERVER_BODY_MAX (1024 bytes). This is
 * well above any realistic settings payload and prevents heap exhaustion
 * from malformed large requests.
 */

#include "settings_server.h"
#include "settings_mgr.h"
#include "utils_web.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "settings_srv";

#define SETTINGS_SERVER_BODY_MAX  1024

/* ── JSON helpers ────────────────────────────────────────────────────────────*/
/* send_json() and send_error() are now web_send_json() / web_send_error()
 * from utils_web.h — the canonical shared implementation that sets
 * Content-Type, Cache-Control, and CORS headers in one place.
 * Local aliases keep the call-sites below unchanged. */
#define send_json(req, obj) web_send_json((req), (obj))
#define send_error(req, msg) web_send_error((req), (msg))

/**
 * Serialise a robot_settings_t into a cJSON object.
 * The caller owns the returned object and must cJSON_Delete() it.
 */
static cJSON *settings_to_json(const robot_settings_t *s)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "schema_version",         s->schema_version);
    cJSON_AddStringToObject(obj, "device_name",            s->device_name);
    cJSON_AddStringToObject(obj, "ap_ssid",                s->ap_ssid);
    /* Never expose password over the wire — send empty string placeholder.
     * A non-empty POST body field replaces it; an empty/absent field keeps
     * the existing password. */
    cJSON_AddStringToObject(obj, "ap_password",            "");
    cJSON_AddNumberToObject(obj, "ap_channel",             s->ap_channel);
    cJSON_AddBoolToObject  (obj, "mdns_enable",            s->mdns_enable);
    cJSON_AddStringToObject(obj, "mdns_hostname",          s->mdns_hostname);
    cJSON_AddNumberToObject(obj, "drive_deadband",         (double)s->drive_deadband);
    cJSON_AddNumberToObject(obj, "drive_ramp_rate",        (double)s->drive_ramp_rate);
    cJSON_AddNumberToObject(obj, "drive_watchdog_ms",      (double)s->drive_watchdog_ms);
    cJSON_AddNumberToObject(obj, "telemetry_interval_ms",  (double)s->telemetry_interval_ms);
    cJSON_AddNumberToObject(obj, "rssi_warn_dbm",          s->rssi_warn_dbm);
    /* OTA token: indicate presence without exposing value */
    cJSON_AddBoolToObject  (obj, "ota_token_set",          s->ota_token[0] != '\0');
    cJSON_AddNumberToObject(obj, "display_sleep_timeout_s",(double)s->display_sleep_timeout_s);

    /* Palette: re-parse the stored JSON string back into an object so the
     * client receives it as a proper JSON sub-object, not a string. */
    if (s->palette[0] != '\0') {
        cJSON *pal = cJSON_Parse(s->palette);
        if (pal) cJSON_AddItemToObject(obj, "palette", pal);
    }

    return obj;
}

/**
 * Apply JSON fields from src onto dst (partial update).
 * Absent keys leave the dst field unchanged.
 * Returns true if any reboot-required field was changed.
 */
static bool apply_json_to_settings(const cJSON *src, robot_settings_t *dst)
{
    bool reboot_required = false;

#define APPLY_STR(key, field, max_len)                                     \
    do {                                                                    \
        cJSON *_j = cJSON_GetObjectItem(src, key);                          \
        if (cJSON_IsString(_j) && _j->valuestring) {                       \
            strncpy(dst->field, _j->valuestring, (max_len) - 1);           \
            dst->field[(max_len) - 1] = '\0';                              \
        }                                                                   \
    } while (0)

#define APPLY_NUM(key, field, cast)                                        \
    do {                                                                    \
        cJSON *_j = cJSON_GetObjectItem(src, key);                          \
        if (cJSON_IsNumber(_j)) dst->field = (cast)(_j->valuedouble);      \
    } while (0)

#define APPLY_BOOL(key, field)                                             \
    do {                                                                    \
        cJSON *_j = cJSON_GetObjectItem(src, key);                          \
        if (cJSON_IsBool(_j)) dst->field = cJSON_IsTrue(_j);              \
    } while (0)

    /* Track reboot-required fields by snapshotting before applying */
    char   old_ap_ssid    [SETTINGS_AP_SSID_MAX];
    char   old_ap_password[SETTINGS_AP_PASS_MAX];
    uint8_t old_ap_channel;
    bool   old_mdns_enable;
    char   old_mdns_hostname[SETTINGS_MDNS_HOST_MAX];

    /* Zero-initialise snapshot buffers so strncmp is safe even if the source
     * string is shorter than the buffer (strncpy does not pad beyond src len
     * on all implementations, so a dirty stack could produce false positives). */
    memset(old_ap_ssid,      0, sizeof(old_ap_ssid));
    memset(old_ap_password,  0, sizeof(old_ap_password));
    memset(old_mdns_hostname,0, sizeof(old_mdns_hostname));

    strncpy(old_ap_ssid,     dst->ap_ssid,      sizeof(old_ap_ssid)     - 1);
    strncpy(old_ap_password, dst->ap_password,  sizeof(old_ap_password) - 1);
    old_ap_channel    = dst->ap_channel;
    old_mdns_enable   = dst->mdns_enable;
    strncpy(old_mdns_hostname, dst->mdns_hostname, sizeof(old_mdns_hostname) - 1);

    /* Apply all fields */
    APPLY_STR ("device_name",            device_name,            SETTINGS_DEVICE_NAME_MAX);
    APPLY_STR ("ap_ssid",                ap_ssid,                SETTINGS_AP_SSID_MAX);

    /* Password: only update if the client sent a non-empty value.
     * An empty string means "keep existing password".
     * A boolean "ap_password_clear": true explicitly clears the password
     * (sets the AP to open), taking precedence over the ap_password field. */
    {
        cJSON *clear = cJSON_GetObjectItem(src, "ap_password_clear");
        if (cJSON_IsTrue(clear)) {
            memset(dst->ap_password, 0, sizeof(dst->ap_password));
        } else {
            cJSON *pw = cJSON_GetObjectItem(src, "ap_password");
            if (cJSON_IsString(pw) && pw->valuestring && pw->valuestring[0] != '\0') {
                strncpy(dst->ap_password, pw->valuestring, sizeof(dst->ap_password) - 1);
                dst->ap_password[sizeof(dst->ap_password) - 1] = '\0';
            }
        }
    }

    APPLY_NUM ("ap_channel",             ap_channel,             uint8_t);
    APPLY_BOOL("mdns_enable",            mdns_enable);
    APPLY_STR ("mdns_hostname",          mdns_hostname,          SETTINGS_MDNS_HOST_MAX);
    APPLY_NUM ("drive_deadband",         drive_deadband,         float);
    APPLY_NUM ("drive_ramp_rate",        drive_ramp_rate,        float);
    APPLY_NUM ("drive_watchdog_ms",      drive_watchdog_ms,      uint32_t);
    APPLY_NUM ("telemetry_interval_ms",  telemetry_interval_ms,  uint32_t);
    APPLY_NUM ("rssi_warn_dbm",          rssi_warn_dbm,          int);

    /* OTA token: only update if the client sent a non-empty value.
     * An empty string means "keep existing token" — same semantics as
     * ap_password. This prevents accidentally clearing the token when the
     * field is left blank because the server never echoes the real value. */
    {
        cJSON *tok = cJSON_GetObjectItem(src, "ota_token");
        if (cJSON_IsString(tok) && tok->valuestring && tok->valuestring[0] != '\0') {
            strncpy(dst->ota_token, tok->valuestring, sizeof(dst->ota_token) - 1);
            dst->ota_token[sizeof(dst->ota_token) - 1] = '\0';
        }
    }

    APPLY_NUM ("display_sleep_timeout_s",display_sleep_timeout_s,uint32_t);

    /* Palette: the client sends a JSON sub-object; serialise it to the flat
     * string field. An absent or non-object value leaves the stored palette
     * unchanged. */
    {
        cJSON *pal = cJSON_GetObjectItem(src, "palette");
        if (cJSON_IsObject(pal)) {
            char *pal_str = cJSON_PrintUnformatted(pal);
            if (pal_str) {
                strncpy(dst->palette, pal_str, sizeof(dst->palette) - 1);
                dst->palette[sizeof(dst->palette) - 1] = '\0';
                free(pal_str);
            }
        }
    }

#undef APPLY_STR
#undef APPLY_NUM
#undef APPLY_BOOL

    /* Check if any reboot-required field changed */
    if (strncmp(old_ap_ssid,      dst->ap_ssid,      sizeof(old_ap_ssid))      != 0 ||
        strncmp(old_ap_password,  dst->ap_password,  sizeof(old_ap_password))  != 0 ||
        old_ap_channel  != dst->ap_channel  ||
        old_mdns_enable != dst->mdns_enable ||
        strncmp(old_mdns_hostname, dst->mdns_hostname, sizeof(old_mdns_hostname)) != 0) {
        reboot_required = true;
    }

    return reboot_required;
}

/* ── Route handlers ──────────────────────────────────────────────────────────*/

/**
 * GET /api/settings
 * Returns the current settings as JSON. ap_password is always empty string.
 */
static esp_err_t handle_get_settings(httpd_req_t *req)
{
    const robot_settings_t *s = settings_get();
    cJSON *obj = settings_to_json(s);
    ESP_LOGD(TAG, "GET /api/settings");
    return send_json(req, obj);
}

/**
 * POST /api/settings
 * Partial update: merge body fields onto current settings, validate, save.
 *
 * Response 200: { "ok": true, "reboot_required": <bool> }
 * Response 400: { "error": "<reason>" }
 */
static esp_err_t handle_post_settings(httpd_req_t *req)
{
    /* Cap body size to prevent heap exhaustion */
    if (req->content_len > SETTINGS_SERVER_BODY_MAX) {
        return send_error(req, "Request body too large (max 1024 bytes)");
    }

    /* Read body */
    int len = req->content_len > 0 ? (int)req->content_len : SETTINGS_SERVER_BODY_MAX;
    char *body = malloc(len + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int received = httpd_req_recv(req, body, len);
    if (received <= 0) {
        free(body);
        return send_error(req, "Empty or unreadable body");
    }
    body[received] = '\0';

    /* Parse JSON */
    cJSON *incoming = cJSON_Parse(body);
    free(body);
    if (!incoming) {
        return send_error(req, "Invalid JSON");
    }

    /* Merge onto a mutex-guarded snapshot of current settings.
     * settings_get_copy() is preferred over settings_get() here because
     * the POST handler is long-running (parse + validate + NVS write) and
     * a concurrent settings_update() could produce a torn read from the
     * raw singleton pointer. */
    robot_settings_t merged;
    settings_get_copy(&merged);

    bool reboot_required = apply_json_to_settings(incoming, &merged);
    cJSON_Delete(incoming);

    /* Validate merged result */
    char reason[128];
    esp_err_t verr = settings_validate(&merged, reason, sizeof(reason));
    if (verr != ESP_OK) {
        ESP_LOGW(TAG, "POST /api/settings validation failed: %s", reason);
        return send_error(req, reason);
    }

    /* Persist */
    esp_err_t serr = settings_update(&merged);
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "settings_update failed: %s", esp_err_to_name(serr));
        return send_error(req, "Failed to save settings (NVS error)");
    }

    ESP_LOGI(TAG, "POST /api/settings OK (reboot_required=%d)", reboot_required);

    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddBoolToObject(resp, "ok",               true);
        cJSON_AddBoolToObject(resp, "reboot_required",  reboot_required);
    }
    return send_json(req, resp);
}

/**
 * POST /api/settings/reset
 * Resets to factory defaults. If body contains { "reboot": true },
 * the device reboots after saving.
 *
 * Response 200: { "ok": true, "reboot_required": true }
 */
static esp_err_t handle_reset_settings(httpd_req_t *req)
{
    /* Read optional body for reboot flag */
    bool do_reboot = false;
    if (req->content_len > 0 && req->content_len <= 64) {
        char body[65] = {0};
        int n = httpd_req_recv(req, body, sizeof(body) - 1);
        if (n > 0) {
            body[n] = '\0';
            cJSON *obj = cJSON_Parse(body);
            if (obj) {
                cJSON *r = cJSON_GetObjectItem(obj, "reboot");
                if (cJSON_IsTrue(r)) do_reboot = true;
                cJSON_Delete(obj);
            }
        }
    }

    esp_err_t err = settings_reset_to_defaults();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings_reset_to_defaults failed: %s",
                 esp_err_to_name(err));
        return send_error(req, "Failed to reset settings");
    }

    ESP_LOGW(TAG, "POST /api/settings/reset — defaults restored%s",
             do_reboot ? " + rebooting" : "");

    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddBoolToObject(resp, "ok",              true);
        cJSON_AddBoolToObject(resp, "reboot_required", true);
    }
    send_json(req, resp);

    if (do_reboot) {
        vTaskDelay(pdMS_TO_TICKS(400));   /* let response reach browser */
        esp_restart();
    }

    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────────*/

esp_err_t settings_server_register(httpd_handle_t server)
{
    static const httpd_uri_t get_route = {
        .uri     = "/api/settings",
        .method  = HTTP_GET,
        .handler = handle_get_settings,
    };
    static const httpd_uri_t post_route = {
        .uri     = "/api/settings",
        .method  = HTTP_POST,
        .handler = handle_post_settings,
    };
    static const httpd_uri_t reset_route = {
        .uri     = "/api/settings/reset",
        .method  = HTTP_POST,
        .handler = handle_reset_settings,
    };

    esp_err_t err;

    err = httpd_register_uri_handler(server, &get_route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /api/settings: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = httpd_register_uri_handler(server, &post_route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register POST /api/settings: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = httpd_register_uri_handler(server, &reset_route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register POST /api/settings/reset: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Settings endpoints registered: "
             "GET /api/settings, POST /api/settings, POST /api/settings/reset");
    return ESP_OK;
}
