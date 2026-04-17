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
    WIFI_MANAGER_STATE_CLIENT_GONE,       /* That device left the AP        */
    WIFI_MANAGER_STATE_CREDS_RECEIVED,    /* portal.c submitted credentials */
    WIFI_MANAGER_STATE_STA_CONNECTING,    /* Connecting to target network    */
    WIFI_MANAGER_STATE_STA_CONNECTED,     /* Connected — provisioning done  */
    WIFI_MANAGER_STATE_STA_FAILED,        /* Connection failed, retrying    */
    WIFI_MANAGER_STATE_ERROR,             /* Unrecoverable error            */
} wifi_manager_state_t;

typedef void (*wifi_manager_state_cb_t)(wifi_manager_state_t state, void *ctx);
typedef void (*wifi_manager_connected_cb_t)(const char *ssid, const char *ip, void *ctx);

typedef struct {
    const char *ap_ssid;
    const char *ap_password;
    uint8_t     ap_channel;      /* 0 → default 1                          */
    int         sta_max_retries;
    wifi_manager_state_cb_t     on_state_change;
    wifi_manager_connected_cb_t on_connected;
    void                       *cb_ctx;
} wifi_manager_config_t;

#define WIFI_MANAGER_CONFIG_DEFAULT() { \
    .ap_ssid         = "ESP32-Setup",  \
    .ap_password     = "setup1234",    \
    .ap_channel      = 1,              \
    .sta_max_retries = 5,              \
    .on_state_change = NULL,           \
    .on_connected    = NULL,           \
    .cb_ctx          = NULL,           \
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
