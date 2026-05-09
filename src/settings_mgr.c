/**
 * settings_mgr.c — NVS-backed JSON configuration store.
 *
 * Design notes
 * ────────────
 * One NVS blob, one schema version, one mutex.
 *
 * Blob size budget
 * ────────────────
 * All string fields at max length + numeric fields serialised as JSON:
 *   device_name(32) + ap_ssid(32) + ap_password(64) + mdns_hostname(32)
 *   + ota_token(64) + ~15 numeric fields at ~10 chars each = ~400 chars.
 * NVS string max is 1984 bytes; we're well inside that.
 *
 * Defaults
 * ────────
 * apply_defaults() is the canonical source of truth for every tuneable's
 * factory value. It runs on first boot (NVS empty) and as the migration
 * fallback for fields missing from older schema versions.
 *
 * Migration
 * ─────────
 * migrate() is called when schema_version < SETTINGS_SCHEMA_VERSION.
 * It fills in any fields that were not present in the older JSON blob
 * (cJSON_GetObjectItem returns NULL → we set the default). The migrated
 * struct is immediately saved so the next boot skips migration.
 *
 * Save throttle
 * ─────────────
 * Not implemented here — the HTTP handler layer should debounce rapid
 * settings_save() calls (e.g. slider drag) if NVS wear is a concern.
 * settings_save() itself is not called in any tight loop by the firmware.
 *
 * Change callbacks
 * ────────────────
 * A single callback slot is provided. The callback fires on the calling
 * task after the NVS commit succeeds. Do not call settings_save() from
 * inside the callback — that would deadlock on the mutex.
 */

#include "settings_mgr.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "settings_mgr";

/* ── Singleton ──────────────────────────────────────────────────────────────*/
static robot_settings_t  s_settings;
static SemaphoreHandle_t s_mutex         = NULL;
static bool              s_initialized   = false;

/* ── Change callback ────────────────────────────────────────────────────────*/
static settings_change_cb_t s_change_cb  = NULL;
static void                *s_change_ctx = NULL;

/* ── Compile-time defaults ──────────────────────────────────────────────────
 *
 * These mirror the #defines in config.h that are candidates for migration
 * to runtime. The config.h values are used as the default here so that
 * existing deployments see identical behaviour on first boot.
 *
 * IMPORTANT: When you remove a #define from config.h (step 2 of the migration
 * plan), keep its value here as the hard-coded default literal. This file is
 * then the only authoritative source for that tunable's factory value.
 */
static void apply_defaults(robot_settings_t *s)
{
    memset(s, 0, sizeof(*s));

    s->schema_version = SETTINGS_SCHEMA_VERSION;

    /* Identity */
    strncpy(s->device_name, "Robot", sizeof(s->device_name) - 1);

    /* AP network — pull from config.h during migration period.
     * Once AP_SSID / AP_PASSWORD are removed from config.h, replace with
     * literal strings. */
    strncpy(s->ap_ssid,     AP_SSID,     sizeof(s->ap_ssid)     - 1);
    strncpy(s->ap_password, AP_PASSWORD, sizeof(s->ap_password) - 1);
    s->ap_channel = 1;

    /* mDNS */
#ifdef MDNS_ENABLE
    s->mdns_enable = MDNS_ENABLE;
#else
    s->mdns_enable = false;
#endif

#ifdef MDNS_HOSTNAME
    strncpy(s->mdns_hostname, MDNS_HOSTNAME, sizeof(s->mdns_hostname) - 1);
#else
    strncpy(s->mdns_hostname, "robot", sizeof(s->mdns_hostname) - 1);
#endif

    /* Drive control */
#ifdef DRIVE_DEADBAND
    s->drive_deadband = DRIVE_DEADBAND;
#else
    s->drive_deadband = 0.05f;
#endif

#ifdef DRIVE_MAX_DELTA_PER_TICK
    s->drive_ramp_rate = DRIVE_MAX_DELTA_PER_TICK;
#else
    s->drive_ramp_rate = 0.05f;
#endif

#ifdef DRIVE_WATCHDOG_MS
    s->drive_watchdog_ms = DRIVE_WATCHDOG_MS;
#else
    s->drive_watchdog_ms = 500;
#endif

    /* Telemetry */
#ifdef HEALTH_SCAN_INTERVAL_MS
    s->telemetry_interval_ms = HEALTH_SCAN_INTERVAL_MS;
#else
    s->telemetry_interval_ms = 200;
#endif

#ifdef HEALTH_RSSI_WARN_DBM
    s->rssi_warn_dbm = HEALTH_RSSI_WARN_DBM;
#else
    s->rssi_warn_dbm = -75;
#endif

    /* OTA auth */
#ifdef OTA_AUTH_TOKEN
    strncpy(s->ota_token, OTA_AUTH_TOKEN, sizeof(s->ota_token) - 1);
#else
    s->ota_token[0] = '\0';   /* open by default */
#endif

    /* Display */
#ifdef DISP_SLEEP_TIMEOUT_S
    s->display_sleep_timeout_s = DISP_SLEEP_TIMEOUT_S;
#else
    s->display_sleep_timeout_s = 0;   /* never sleep */
#endif
}

/* ── Validation ─────────────────────────────────────────────────────────────*/

esp_err_t settings_validate(const robot_settings_t *s,
                             char *err_buf, size_t err_buf_len)
{
#define FAIL(fmt, ...)                                              \
    do {                                                            \
        if (err_buf && err_buf_len)                                 \
            snprintf(err_buf, err_buf_len, fmt, ##__VA_ARGS__);    \
        return ESP_ERR_INVALID_ARG;                                 \
    } while (0)

    if (!s) FAIL("null settings pointer");

    /* Identity */
    if (s->device_name[0] == '\0')
        FAIL("device_name must not be empty");
    if (strnlen(s->device_name, sizeof(s->device_name)) == sizeof(s->device_name))
        FAIL("device_name exceeds %d chars", (int)sizeof(s->device_name) - 1);

    /* AP credentials */
    if (s->ap_ssid[0] == '\0')
        FAIL("ap_ssid must not be empty");
    if (strnlen(s->ap_ssid, sizeof(s->ap_ssid)) == sizeof(s->ap_ssid))
        FAIL("ap_ssid exceeds %d chars", (int)sizeof(s->ap_ssid) - 1);
    /* password either empty (open) or at least 8 chars (WPA2 minimum) */
    size_t pass_len = strnlen(s->ap_password, sizeof(s->ap_password));
    if (pass_len > 0 && pass_len < 8)
        FAIL("ap_password must be empty (open) or >= 8 chars");
    if (pass_len == sizeof(s->ap_password))
        FAIL("ap_password exceeds %d chars", (int)sizeof(s->ap_password) - 1);

    /* Channel: 0 = auto, 1–13 valid */
    if (s->ap_channel > 13)
        FAIL("ap_channel must be 0 (auto) or 1–13");

    /* mDNS hostname */
    if (s->mdns_enable) {
        if (s->mdns_hostname[0] == '\0')
            FAIL("mdns_hostname must not be empty when mdns_enable is true");
        if (strnlen(s->mdns_hostname, sizeof(s->mdns_hostname))
                == sizeof(s->mdns_hostname))
            FAIL("mdns_hostname exceeds %d chars",
                 (int)sizeof(s->mdns_hostname) - 1);
    }

    /* Drive */
    if (s->drive_deadband < 0.0f || s->drive_deadband > 0.5f)
        FAIL("drive_deadband out of range [0.0, 0.5]");
    if (isnan(s->drive_deadband) || isinf(s->drive_deadband))
        FAIL("drive_deadband is NaN or Inf");

    if (s->drive_ramp_rate <= 0.0f || s->drive_ramp_rate > 1.0f)
        FAIL("drive_ramp_rate out of range (0.0, 1.0]");
    if (isnan(s->drive_ramp_rate) || isinf(s->drive_ramp_rate))
        FAIL("drive_ramp_rate is NaN or Inf");

    if (s->drive_watchdog_ms < 100 || s->drive_watchdog_ms > 10000)
        FAIL("drive_watchdog_ms out of range [100, 10000]");

    /* Telemetry */
    if (s->telemetry_interval_ms < 50 || s->telemetry_interval_ms > 10000)
        FAIL("telemetry_interval_ms out of range [50, 10000]");

    if (s->rssi_warn_dbm < -120 || s->rssi_warn_dbm > 0)
        FAIL("rssi_warn_dbm out of range [-120, 0]");

    /* OTA token length */
    if (strnlen(s->ota_token, sizeof(s->ota_token)) == sizeof(s->ota_token))
        FAIL("ota_token exceeds %d chars", (int)sizeof(s->ota_token) - 1);

    /* Display sleep timeout: 0 = never sleep, up to 3600 s (1 hour) */
    if (s->display_sleep_timeout_s > 3600)
        FAIL("display_sleep_timeout_s out of range [0, 3600]");

#undef FAIL
    return ESP_OK;
}

/* ── Serialise ──────────────────────────────────────────────────────────────*/

static char *serialise(const robot_settings_t *s)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "schema_version",        s->schema_version);
    cJSON_AddStringToObject(obj, "device_name",           s->device_name);
    cJSON_AddStringToObject(obj, "ap_ssid",               s->ap_ssid);
    cJSON_AddStringToObject(obj, "ap_password",           s->ap_password);
    cJSON_AddNumberToObject(obj, "ap_channel",            s->ap_channel);
    cJSON_AddBoolToObject  (obj, "mdns_enable",           s->mdns_enable);
    cJSON_AddStringToObject(obj, "mdns_hostname",         s->mdns_hostname);
    cJSON_AddNumberToObject(obj, "drive_deadband",        (double)s->drive_deadband);
    cJSON_AddNumberToObject(obj, "drive_ramp_rate",       (double)s->drive_ramp_rate);
    cJSON_AddNumberToObject(obj, "drive_watchdog_ms",     (double)s->drive_watchdog_ms);
    cJSON_AddNumberToObject(obj, "telemetry_interval_ms", (double)s->telemetry_interval_ms);
    cJSON_AddNumberToObject(obj, "rssi_warn_dbm",         s->rssi_warn_dbm);
    cJSON_AddStringToObject(obj, "ota_token",             s->ota_token);
    cJSON_AddNumberToObject(obj, "display_sleep_timeout_s", (double)s->display_sleep_timeout_s);
    /* Palette: stored as a raw JSON string (already-serialised object).
     * An empty string means "no palette saved" — omit from output. */
    if (s->palette[0] != '\0') {
        cJSON *pal = cJSON_Parse(s->palette);
        if (pal) cJSON_AddItemToObject(obj, "palette", pal);
    }

    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return out;
}

/* ── Deserialise + migrate ─────────────────────────────────────────────────
 *
 * Parse the stored JSON into dst. For every key, if the JSON field is absent
 * or the wrong type, the corresponding field from fallback_defaults is used.
 * This is the migration path: new fields added to the schema have their default
 * applied automatically without wiping user-configured fields.
 */
static void deserialise(const char *json_str, robot_settings_t *dst)
{
    /* Start from defaults so any missing keys are already correct */
    apply_defaults(dst);

    cJSON *obj = cJSON_Parse(json_str);
    if (!obj) {
        ESP_LOGW(TAG, "JSON parse failed — using defaults");
        return;
    }

#define STR_FIELD(key, field, max_len)                                       \
    do {                                                                      \
        cJSON *_j = cJSON_GetObjectItem(obj, key);                            \
        if (cJSON_IsString(_j) && _j->valuestring) {                         \
            strncpy(dst->field, _j->valuestring, (max_len) - 1);             \
            dst->field[(max_len) - 1] = '\0';                                \
        }                                                                     \
    } while (0)

#define NUM_FIELD(key, field, cast)                                          \
    do {                                                                      \
        cJSON *_j = cJSON_GetObjectItem(obj, key);                            \
        if (cJSON_IsNumber(_j)) dst->field = (cast)(_j->valuedouble);        \
    } while (0)

#define BOOL_FIELD(key, field)                                               \
    do {                                                                      \
        cJSON *_j = cJSON_GetObjectItem(obj, key);                            \
        if (cJSON_IsBool(_j)) dst->field = cJSON_IsTrue(_j);                 \
    } while (0)

    NUM_FIELD ("schema_version",         schema_version,         int);
    STR_FIELD ("device_name",            device_name,            SETTINGS_DEVICE_NAME_MAX);
    STR_FIELD ("ap_ssid",                ap_ssid,                SETTINGS_AP_SSID_MAX);
    STR_FIELD ("ap_password",            ap_password,            SETTINGS_AP_PASS_MAX);
    NUM_FIELD ("ap_channel",             ap_channel,             uint8_t);
    BOOL_FIELD("mdns_enable",            mdns_enable);
    STR_FIELD ("mdns_hostname",          mdns_hostname,          SETTINGS_MDNS_HOST_MAX);
    NUM_FIELD ("drive_deadband",         drive_deadband,         float);
    NUM_FIELD ("drive_ramp_rate",        drive_ramp_rate,        float);
    NUM_FIELD ("drive_watchdog_ms",      drive_watchdog_ms,      uint32_t);
    NUM_FIELD ("telemetry_interval_ms",  telemetry_interval_ms,  uint32_t);
    NUM_FIELD ("rssi_warn_dbm",          rssi_warn_dbm,          int);
    STR_FIELD ("ota_token",              ota_token,              SETTINGS_OTA_TOKEN_MAX);
    NUM_FIELD ("display_sleep_timeout_s",display_sleep_timeout_s,uint32_t);

    /* Palette: stored as a sub-object; re-serialise to the flat string field */
    {
        cJSON *pal = cJSON_GetObjectItem(obj, "palette");
        if (cJSON_IsObject(pal)) {
            char *pal_str = cJSON_PrintUnformatted(pal);
            if (pal_str) {
                strncpy(dst->palette, pal_str, sizeof(dst->palette) - 1);
                dst->palette[sizeof(dst->palette) - 1] = '\0';
                free(pal_str);
            }
        }
    }

#undef STR_FIELD
#undef NUM_FIELD
#undef BOOL_FIELD

    cJSON_Delete(obj);
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

esp_err_t settings_load(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "Failed to create settings mutex");
            apply_defaults(&s_settings);
            return ESP_ERR_NO_MEM;
        }
    }

    /* Open NVS */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%s) — using defaults",
                 esp_err_to_name(err));
        apply_defaults(&s_settings);
        s_initialized = true;
        return ESP_ERR_NVS_NOT_FOUND;
    }

    /* Query blob length */
    size_t len = 0;
    err = nvs_get_str(nvs, SETTINGS_NVS_KEY, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(nvs);
        ESP_LOGW(TAG, "Settings key absent — using defaults");
        apply_defaults(&s_settings);
        s_initialized = true;
        return ESP_ERR_NVS_NOT_FOUND;
    }

    /* Read blob */
    char *buf = malloc(len);
    if (!buf) {
        nvs_close(nvs);
        ESP_LOGE(TAG, "OOM reading settings blob (%u bytes)", (unsigned)len);
        apply_defaults(&s_settings);
        s_initialized = true;
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(nvs, SETTINGS_NVS_KEY, buf, &len);
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str failed: %s — using defaults",
                 esp_err_to_name(err));
        free(buf);
        apply_defaults(&s_settings);
        s_initialized = true;
        return err;
    }

    /* Parse + migrate */
    deserialise(buf, &s_settings);
    free(buf);

    if (s_settings.schema_version < SETTINGS_SCHEMA_VERSION) {
        ESP_LOGI(TAG, "Schema migration: v%d → v%d",
                 s_settings.schema_version, SETTINGS_SCHEMA_VERSION);
        s_settings.schema_version = SETTINGS_SCHEMA_VERSION;
        /* Save migrated blob so next boot is clean */
        settings_save();
    }

    /* Validate loaded settings; if corrupt fall back to defaults */
    char reason[64];
    if (settings_validate(&s_settings, reason, sizeof(reason)) != ESP_OK) {
        ESP_LOGW(TAG, "Stored settings invalid (%s) — resetting to defaults",
                 reason);
        apply_defaults(&s_settings);
        settings_save();
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Settings loaded (schema v%d)", s_settings.schema_version);
    ESP_LOGI(TAG, "  device_name:         %s",  s_settings.device_name);
    ESP_LOGI(TAG, "  ap_ssid:             %s",  s_settings.ap_ssid);
    ESP_LOGI(TAG, "  mdns:                %s → %s.local",
             s_settings.mdns_enable ? "on" : "off", s_settings.mdns_hostname);
    ESP_LOGI(TAG, "  drive_deadband:      %.3f", s_settings.drive_deadband);
    ESP_LOGI(TAG, "  drive_ramp_rate:     %.3f", s_settings.drive_ramp_rate);
    ESP_LOGI(TAG, "  drive_watchdog_ms:   %lu",  (unsigned long)s_settings.drive_watchdog_ms);
    ESP_LOGI(TAG, "  telemetry_ms:        %lu",  (unsigned long)s_settings.telemetry_interval_ms);
    ESP_LOGI(TAG, "  rssi_warn_dbm:       %d",   s_settings.rssi_warn_dbm);

    return ESP_OK;
}

esp_err_t settings_save(void)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "settings_save called before settings_load");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_settings.schema_version = SETTINGS_SCHEMA_VERSION;

    char *json = serialise(&s_settings);
    if (!json) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Serialisation failed (OOM?)");
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        free(json);
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs, SETTINGS_NVS_KEY, json);
    free(json);

    if (err != ESP_OK) {
        nvs_close(nvs);
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "nvs_set_str failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    xSemaphoreGive(s_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s — settings may not persist",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Settings saved to NVS");

    /* Fire change callback outside the mutex */
    if (s_change_cb)
        s_change_cb(&s_settings, s_change_ctx);

    return ESP_OK;
}

esp_err_t settings_update(const robot_settings_t *src)
{
    if (!src) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_settings, src, sizeof(s_settings));
    xSemaphoreGive(s_mutex);

    return settings_save();
}

esp_err_t settings_reset_to_defaults(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    apply_defaults(&s_settings);
    xSemaphoreGive(s_mutex);

    ESP_LOGW(TAG, "Settings reset to factory defaults");
    return settings_save();
}

const robot_settings_t *settings_get(void)
{
    /* If called before settings_load() (programmer error), return defaults
     * rather than crashing. Log loudly so the bug is obvious. */
    if (!s_initialized) {
        ESP_LOGE(TAG, "settings_get called before settings_load — returning defaults");
        static robot_settings_t emergency;
        apply_defaults(&emergency);
        return &emergency;
    }
    /* NOTE: The returned pointer is valid only while the caller holds no
     * concurrent writes. For a safe snapshot across arbitrary fields, use
     * settings_get_copy() instead. settings_get() is kept for lightweight
     * single-field reads and for the telemetry fast-path. */
    return &s_settings;
}

esp_err_t settings_get_copy(robot_settings_t *dst)
{
    if (!dst) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) {
        apply_defaults(dst);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(dst, &s_settings, sizeof(*dst));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void settings_register_change_cb(settings_change_cb_t cb, void *ctx)
{
    s_change_cb  = cb;
    s_change_ctx = ctx;
}
