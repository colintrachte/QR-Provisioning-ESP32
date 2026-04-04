#pragma once
/**
 * wifi_prov.h — WiFi provisioning via SoftAP + captive portal.
 *
 * Flow:
 *   1. On boot, attempt connection with NVS-stored credentials.
 *   2. If no credentials or connection fails → start SoftAP + DNS + HTTP server.
 *   3. Phone joins AP, captive portal redirect fires → web page lists nearby
 *      networks.  User picks one, enters password, submits.
 *   4. Credentials saved to NVS, STA connection attempted.
 *   5. On success: AP torn down, callbacks fired, provisioning done.
 *   6. On failure: portal stays up, user can retry.
 *
 * Callbacks run on the caller's task context via an internal event queue —
 * they are NOT called from WiFi event handlers.  Safe to call display_* from
 * inside them.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ── State enum (passed to on_state_change) ─────────────────────────────*/
typedef enum {
    WIFI_PROV_STATE_CONNECTING_SAVED,  /* Trying stored credentials        */
    WIFI_PROV_STATE_AP_STARTED,        /* SoftAP up, portal live           */
    WIFI_PROV_STATE_CLIENT_CONNECTED,  /* Phone joined our AP              */
    WIFI_PROV_STATE_CLIENT_GONE,       /* Phone left our AP                */
    WIFI_PROV_STATE_CREDS_RECEIVED,    /* User submitted SSID+pass         */
    WIFI_PROV_STATE_STA_CONNECTING,    /* Connecting to target network     */
    WIFI_PROV_STATE_STA_CONNECTED,     /* Connected! Provisioning done     */
    WIFI_PROV_STATE_STA_FAILED,        /* Connection failed, portal retry  */
    WIFI_PROV_STATE_ERROR,             /* Unrecoverable error              */
} wifi_prov_state_t;

/* ── Callback types ─────────────────────────────────────────────────────*/

/**
 * Called whenever provisioning state changes.
 * @param state   New state
 * @param ctx     User context pointer from wifi_prov_config_t
 */
typedef void (*wifi_prov_state_cb_t)(wifi_prov_state_t state, void *ctx);

/**
 * Called when STA connection succeeds.
 * @param ssid    Connected network SSID
 * @param ip      Assigned IP address string (e.g. "192.168.1.42")
 * @param ctx     User context pointer
 */
typedef void (*wifi_prov_connected_cb_t)(const char *ssid, const char *ip, void *ctx);

/* ── Configuration ──────────────────────────────────────────────────────*/
typedef struct {
    const char *ap_ssid;          /* SoftAP SSID (required)               */
    const char *ap_password;      /* SoftAP password; NULL or "" = open   */
    uint8_t     ap_channel;       /* 1–13, default 1                      */
    int         sta_max_retries;  /* STA connect attempts before giving up */
    int         portal_timeout_s; /* 0 = no timeout; else tear down after  */

    wifi_prov_state_cb_t     on_state_change;  /* optional                */
    wifi_prov_connected_cb_t on_connected;     /* optional                */
    void                    *cb_ctx;           /* passed to both callbacks */
} wifi_prov_config_t;

#define WIFI_PROV_CONFIG_DEFAULT() {    \
    .ap_ssid          = "RobotSetup",  \
    .ap_password      = "robot1234",   \
    .ap_channel       = 1,             \
    .sta_max_retries  = 5,             \
    .portal_timeout_s = 0,             \
    .on_state_change  = NULL,          \
    .on_connected     = NULL,          \
    .cb_ctx           = NULL,          \
}

/* ── API ────────────────────────────────────────────────────────────────*/

/**
 * Initialise NVS, netif, event loop, and attempt connection with stored
 * credentials.  Falls back to SoftAP portal if needed.
 *
 * This function returns immediately after starting the provisioning process.
 * All state changes are reported via callbacks.  Must be called from app_main
 * after display_init().
 */
esp_err_t wifi_prov_start(const wifi_prov_config_t *config);

/**
 * Force the portal to start even if credentials exist.
 * Useful for a "re-provision" button press.
 */
esp_err_t wifi_prov_reset_and_restart(void);

/**
 * Erase stored credentials from NVS.
 */
esp_err_t wifi_prov_erase_credentials(void);

/**
 * Returns true if the device is currently connected to a WiFi network as STA.
 */
bool wifi_prov_is_connected(void);

/**
 * Get the current IP address string.  Returns NULL if not connected.
 * Pointer is valid until next call or disconnect.
 */
const char *wifi_prov_get_ip(void);
