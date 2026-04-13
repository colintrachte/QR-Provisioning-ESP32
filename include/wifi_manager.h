#pragma once
/**
 * wifi_manager.h — SoftAP captive-portal WiFi provisioning for ESP-IDF.
 *
 * Flow:
 *  1. wifi_manager_start() tries NVS credentials first.
 *  2. On failure (or no credentials): SoftAP + DNS hijack + HTTP portal.
 *  3. Portal lists nearby networks; user picks one and submits password.
 *  4. Credentials saved to NVS, device reboots and joins as STA.
 *  5. app_server_start() brings up the robot control HTTP + WebSocket server.
 *
 * Callbacks run on a dedicated FreeRTOS task — safe to call display_* inside.
 *
 * Typical usage:
 *   wifi_manager_config_t cfg = WIFI_MANAGER_CONFIG_DEFAULT();
 *   cfg.ap_ssid         = "MyDevice";
 *   cfg.ap_password     = "setup1234";
 *   cfg.on_state_change = my_state_cb;
 *   cfg.on_connected    = my_connected_cb;
 *   wifi_manager_start(&cfg);
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Portal network ─────────────────────────────────────────────────────────*/
#define WIFI_MANAGER_AP_IP      "192.168.4.1"
#define WIFI_MANAGER_AP_MAX_STA 4

/* ── State machine ──────────────────────────────────────────────────────────
 * Normal provisioning flow:
 *   [NVS creds found]  CONNECTING_SAVED → STA_CONNECTED
 *   [no creds / fail]  AP_STARTED → CLIENT_CONNECTED → CREDS_RECEIVED
 *                        → STA_CONNECTING → STA_CONNECTED
 *   [wrong password]   STA_CONNECTING → STA_FAILED → (portal stays open)
 */
typedef enum
{
    WIFI_MANAGER_STATE_CONNECTING_SAVED,  /* Trying NVS-stored credentials  */
    WIFI_MANAGER_STATE_AP_STARTED,        /* SoftAP up, captive portal live  */
    WIFI_MANAGER_STATE_CLIENT_CONNECTED,  /* A device joined the AP          */
    WIFI_MANAGER_STATE_CLIENT_GONE,       /* That device left the AP         */
    WIFI_MANAGER_STATE_CREDS_RECEIVED,    /* User submitted SSID + password  */
    WIFI_MANAGER_STATE_STA_CONNECTING,    /* Connecting to target network     */
    WIFI_MANAGER_STATE_STA_CONNECTED,     /* Connected — provisioning done   */
    WIFI_MANAGER_STATE_STA_FAILED,        /* Connection failed, will retry   */
    WIFI_MANAGER_STATE_ERROR,             /* Unrecoverable error             */
} wifi_manager_state_t;

/* ── Callbacks ──────────────────────────────────────────────────────────────*/

/** Fired on every state transition.
 * @param state  New state.
 * @param ctx    User pointer from wifi_manager_config_t::cb_ctx. */
typedef void (*wifi_manager_state_cb_t)(wifi_manager_state_t state, void *ctx);

/** Fired once when the STA interface gets an IP address.
 * @param ssid  Joined network name.
 * @param ip    Assigned IPv4 as dotted-decimal string.
 * @param ctx   User pointer from wifi_manager_config_t::cb_ctx. */
typedef void (*wifi_manager_connected_cb_t)(const char *ssid, const char *ip,
                                            void *ctx);

/* ── Configuration ──────────────────────────────────────────────────────────*/
typedef struct
{
    const char *ap_ssid;         /* SoftAP SSID (required, ≤32 chars)       */
    const char *ap_password;     /* SoftAP password; NULL/"" = open network  */
    uint8_t     ap_channel;      /* WiFi channel 1–13 (0 → default 1)       */
    int         sta_max_retries; /* STA reconnect attempts before giving up  */
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

/* ── Core API ───────────────────────────────────────────────────────────────*/

/** Start the WiFi manager. Tries NVS credentials first; falls back to portal.
 *  Non-blocking: progress reported through callbacks.
 *  @param config  Populated wifi_manager_config_t.
 *  @return ESP_OK or an esp_err_t on hard failure (e.g. OOM). */
esp_err_t wifi_manager_start(const wifi_manager_config_t *config);

/** Erase stored credentials and restart the captive portal. */
esp_err_t wifi_manager_reset(void);

/** Erase stored credentials without restarting (e.g. factory reset flows). */
esp_err_t wifi_manager_erase_credentials(void);

/** Returns true if the STA interface currently has an IP address. */
bool wifi_manager_is_connected(void);

/** Returns the current STA IP string, or NULL if not connected.
 *  Valid until the next disconnect event. */
const char *wifi_manager_get_ip(void);

/* ── App server (robot control HTTP + WebSocket) ────────────────────────────
 * Started automatically by the manager when STA connects (port 80).
 * Call stop only if you need to tear down (e.g. switching to AP-only mode). */
void app_server_start(void);
void app_server_stop(void);

#ifdef __cplusplus
}
#endif
