#pragma once
/**
 * wifi_manager.h — STA/AP WiFi state machine and NVS credential storage.
 *
 * This module handles exactly three concerns:
 *   1. NVS: load, save, and erase {ssid, password} in the "wifi" namespace.
 *   2. SoftAP: start/stop the access point so phones can join.
 *   3. STA: connect to the target network, retry on failure, report state.
 *
 * Everything else — DNS hijacking, captive portal HTTP, OS probe handlers,
 * WebSocket dispatch, file serving — lives in portal.c and app_server.c.
 *
 * Typical call sequence:
 *   wifi_manager_config_t cfg = WIFI_MANAGER_CONFIG_DEFAULT();
 *   cfg.ap_ssid         = "MyDevice";
 *   cfg.ap_password     = "setup1234";
 *   cfg.on_state_change = my_state_cb;
 *   cfg.on_connected    = my_connected_cb;
 *   wifi_manager_start(&cfg);
 *
 * wifi_manager_start() is non-blocking after returning ESP_OK. Progress is
 * reported through callbacks, which run on a dedicated FreeRTOS task — safe
 * to call display_* or o_led_* from inside them.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_AP_IP      "192.168.4.1"
#define WIFI_MANAGER_AP_MAX_STA 4

/* ── State machine ──────────────────────────────────────────────────────────
 * Normal provisioning flow:
 *   [NVS creds exist]   CONNECTING_SAVED → STA_CONNECTED
 *   [no creds / fail]   AP_STARTED → CLIENT_CONNECTED → CREDS_RECEIVED
 *                         → STA_CONNECTING → STA_CONNECTED
 *   [wrong password]    STA_CONNECTING → STA_FAILED → AP_STARTED (portal)
 */
typedef enum {
    WIFI_MANAGER_STATE_CONNECTING_SAVED,  /* Trying NVS-stored credentials  */
    WIFI_MANAGER_STATE_AP_STARTED,        /* SoftAP up, waiting for client  */
    WIFI_MANAGER_STATE_CLIENT_CONNECTED,  /* A device joined the AP         */
    WIFI_MANAGER_STATE_CLIENT_DISCONNECTED, /* That device left the AP        */
    WIFI_MANAGER_STATE_CREDS_RECEIVED,    /* portal.c submitted credentials */
    WIFI_MANAGER_STATE_STA_CONNECTING,    /* Connecting to target network    */
    WIFI_MANAGER_STATE_STA_CONNECTED,     /* Connected — provisioning done  */
    WIFI_MANAGER_STATE_STA_FAILED,        /* Connection failed, retrying    */
    WIFI_MANAGER_STATE_ERROR,             /* Unrecoverable error            */
} wifi_manager_state_t;

typedef void (*wifi_manager_state_cb_t)(wifi_manager_state_t state, void *ctx);
typedef void (*wifi_manager_connected_cb_t)(const char *ssid, const char *ip, void *ctx);

/**
 * on_radio_reset: Called from mgr_task (WiFi manager task) after
 * esp_wifi_set_mode() during AP reset. Executes on the same task
 * as the WiFi state machine. Keep this handler fast — it blocks
 * mgr_task while running. I2C reinit (~1ms) is acceptable; avoid
 * anything that could block for meaningful time (flash writes,
 * long delays, waiting on other tasks).
 */
typedef struct {
    const char *ap_ssid;
    const char *ap_password;
    uint8_t     ap_channel;
    int         sta_max_retries;
    void       *cb_ctx;
    void      (*on_state_change)(wifi_manager_state_t state, void *ctx);
    void      (*on_connected)(const char *ssid, const char *ip, void *ctx);
    void      (*on_radio_reset)(void *ctx);   // ← add here
} wifi_manager_config_t;

#define WIFI_MANAGER_CONFIG_DEFAULT() { \
    .ap_ssid        = NULL,             \
    .ap_password    = NULL,             \
    .ap_channel     = 1,                \
    .sta_max_retries = 3,               \
    .cb_ctx         = NULL,             \
    .on_state_change = NULL,            \
    .on_connected    = NULL,            \
    .on_radio_reset  = NULL,            \
}

/**
 * Start the WiFi manager. Tries NVS credentials first; falls back to the
 * captive portal (portal.c) if none exist or connection fails.
 * Non-blocking: progress is reported through callbacks.
 * @return ESP_OK or an esp_err_t on hard failure (e.g. OOM, NVS fault).
 */
esp_err_t wifi_manager_start(const wifi_manager_config_t *config);

/** Erase stored credentials and return to the portal. */
esp_err_t wifi_manager_reset(void);

/** Erase stored credentials without triggering a portal restart. */
esp_err_t wifi_manager_erase_credentials(void);

/** True if STA interface currently holds an IP address. */
bool wifi_manager_is_connected(void);

/** Current STA IP as dotted-decimal string, or NULL if not connected. */
const char *wifi_manager_get_ip(void);

#ifdef __cplusplus
}
#endif
