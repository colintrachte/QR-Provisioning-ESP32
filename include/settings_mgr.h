/**
 * settings_mgr.h — Runtime configuration store.
 *
 * Single source of truth for all tunables that do not require a firmware
 * reflash. Hardware-locked constants (GPIO numbers, ADC channels, LEDC
 * timer/channel assignments) stay in config.h; everything the user might
 * want to change at runtime lives here.
 *
 * Storage model
 * ─────────────
 * All settings are serialised as a single cJSON blob and stored under one
 * NVS key ("cfg") in the "settings" namespace. An atomic write (serialize →
 * nvs_set_str → nvs_commit) prevents partial-update corruption.
 *
 * Schema version
 * ─────────────
 * s_settings.schema_version is written on every save. On load, if the stored
 * version < SETTINGS_SCHEMA_VERSION, settings_load() runs migrate() to fill
 * in any new keys with their defaults before returning. This lets firmware
 * upgrades add new tunables without wiping existing user config.
 *
 * Thread safety
 * ─────────────
 * settings_get() returns a const pointer to the singleton struct. Reads are
 * safe from any task — the struct is updated only inside settings_load() (boot,
 * single task) and settings_save() (protected by an internal mutex).
 * Consumers that want to react to changes should register via
 * settings_register_change_cb().
 *
 * Usage
 * ─────
 * 1. Call settings_load() once, right after nvs_flash_init() in app_main().
 * 2. Read tunables via settings_get()->field.
 * 3. Modify via settings_update() + settings_save() from the HTTP handler.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ── Schema version ─────────────────────────────────────────────────────────
 * Bump this integer whenever new fields are added to robot_settings_t.
 * The migration path in settings_mgr.c will apply defaults for any new key. */
#define SETTINGS_SCHEMA_VERSION  3

/* ── NVS location ───────────────────────────────────────────────────────────*/
#define SETTINGS_NVS_NS   "settings"
#define SETTINGS_NVS_KEY  "cfg"

/* ── Limits ─────────────────────────────────────────────────────────────────*/
#define SETTINGS_DEVICE_NAME_MAX   32
#define SETTINGS_AP_SSID_MAX       32
#define SETTINGS_AP_PASS_MAX       64
#define SETTINGS_MDNS_HOST_MAX     32
#define SETTINGS_OTA_TOKEN_MAX     64

/* ── Settings struct ────────────────────────────────────────────────────────
 *
 * All fields must be trivially copyable (no pointers). String fields are
 * fixed-size char arrays so the struct can be stack-copied safely.
 *
 * Field naming convention: snake_case, units appended (_ms, _dbm, _pct).
 */
typedef struct {
    /* Meta */
    int     schema_version;

    /* Identity */
    char    device_name[SETTINGS_DEVICE_NAME_MAX];   /* "Robot" */

    /* Network — AP mode */
    char    ap_ssid    [SETTINGS_AP_SSID_MAX];        /* SoftAP SSID          */
    char    ap_password[SETTINGS_AP_PASS_MAX];        /* "" = open network    */
    uint8_t ap_channel;                               /* 1–13, 0 = auto       */

    /* Network — mDNS */
    bool    mdns_enable;
    char    mdns_hostname[SETTINGS_MDNS_HOST_MAX];   /* "robot" → robot.local */

    /* Drive control */
    float   drive_deadband;           /* 0.0–0.5, default 0.05               */
    float   drive_ramp_rate;          /* max Δ per 10 ms tick, default 0.05  */
    uint32_t drive_watchdog_ms;       /* command timeout, default 500        */

    /* Telemetry / health */
    uint32_t telemetry_interval_ms;  /* push period, default 200 (5 Hz)     */
    int      rssi_warn_dbm;          /* log warning below this, default -75  */
    int      battery_warn_pct;       /* OLED + telemetry flag below this, default 20 */

    /* OTA security */
    char    ota_token[SETTINGS_OTA_TOKEN_MAX];       /* "" = no auth         */

    /* Display */
    uint32_t display_sleep_timeout_s; /* 0 = never sleep, default 0         */
    /* UI theme palette — stored as a serialised JSON object string.
     * Empty string means "use firmware defaults" (no saved palette).
     * 512 bytes is enough for ~10 CSS variables at "#rrggbb" each. */
    char palette[512];

} robot_settings_t;

/* ── Change callback ────────────────────────────────────────────────────────
 * Called on the task that invoked settings_save(). Keep it short; do not
 * call settings_save() recursively from inside the callback. */
typedef void (*settings_change_cb_t)(const robot_settings_t *new_settings,
                                     void *ctx);

/* ── Public API ─────────────────────────────────────────────────────────────*/

/**
 * @brief Load settings from NVS into the singleton.
 *
 * Must be called once from app_main() after nvs_flash_init().
 *
 * @pre nvs_flash_init() has succeeded.
 * @post settings_get() returns valid data.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if defaults were used,
 *         or another esp_err_t on failure.
 */
esp_err_t settings_load(void);

/**
 * @brief Write the current singleton to NVS (serialize → nvs_set_str → nvs_commit).
 * Fires registered change callbacks after a successful save.
 *
 * Thread-safe: serialisation is mutex-protected.
 *
 * @return ESP_OK on success, or an NVS/cJSON error code.
 */
esp_err_t settings_save(void);

/**
 * @brief Overwrite the singleton with src, then call settings_save().
 * Use from HTTP handlers: copy the validated new struct in, then save.
 *
 * @param src   Pointer to the new settings; must not be NULL.
 * @return      Result of settings_save().
 */
esp_err_t settings_update(const robot_settings_t *src);

/**
 * @brief Reset the singleton to compile-time defaults and save to NVS.
 * Does NOT restart the device — callers may choose to reboot afterward.
 */
esp_err_t settings_reset_to_defaults(void);

/**
 * @brief Read-only accessor. Returns a stable pointer valid for the lifetime of
 * the firmware run. Do not cache the returned struct by value if you need
 * to observe future changes; instead re-call settings_get() or use a
 * change callback.
 *@return Stable const pointer valid for the lifetime of the firmware.
 */
const robot_settings_t *settings_get(void);

/**
 * @brief Register a callback invoked after every successful settings_save().
 * Only one callback slot is provided per call — call again to replace.
 * @param cb Callback (NULL to clear).
 * @param ctx Opaque context passed to the callback.
 */
void settings_register_change_cb(settings_change_cb_t cb, void *ctx);

/**
 * @brief Validate a candidate settings struct.
 *
 * @param s Settings to validate.
 * @param err_buf Optional buffer for error message (can be NULL).
 * @param err_buf_len Size of err_buf.
 * @return ESP_OK if valid, or ESP_ERR_INVALID_ARG with message in err_buf.
 */
esp_err_t settings_validate(const robot_settings_t *s,char *err_buf, size_t err_buf_len);
/**
 * @brief Copy current settings under mutex into *dst.
 *
 * Use this instead of settings_get() when you need a consistent snapshot
 * of multiple fields (e.g. in an HTTP handler that runs for >1 tick while
 * settings_update() could race on another task).
 *
 * Returns ESP_OK on success, ESP_ERR_INVALID_ARG if dst is NULL,
 * ESP_ERR_INVALID_STATE if settings_load() has not been called yet
 * (dst is filled with defaults in that case).
 * @param dst Destination struct (must not be NULL).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if dst is NULL.
 */
esp_err_t settings_get_copy(robot_settings_t *dst);
