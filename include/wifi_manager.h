#pragma once
/**
 * wifi_manager.h — SoftAP captive-portal WiFi provisioning for ESP-IDF.
 *
 * Drop-in WiFiManager equivalent for ESP32 + ESP-IDF projects.
 *
 * Behaviour
 * ─────────
 *  1. On start, attempt STA connection using credentials stored in NVS.
 *  2. If no credentials exist, or connection fails → start SoftAP + DNS
 *     hijack + HTTP captive portal.
 *  3. User's device joins the AP; OS captive-portal detection fires
 *     automatically (Windows, iOS, Android all handled).
 *  4. Portal page lists nearby networks; user picks one and enters password.
 *  5. Credentials saved to NVS, STA connection attempted.
 *  6. On success: callbacks fired, portal stays running until you call
 *     wifi_manager_stop_portal().
 *  7. On failure: portal stays up, user can retry.
 *
 * Callbacks run on a dedicated FreeRTOS task, NOT from WiFi event handlers,
 * so it is safe to call display_* functions directly inside them.
 *
 * Typical usage
 * ─────────────
 *  wifi_manager_config_t cfg = WIFI_MANAGER_CONFIG_DEFAULT();
 *  cfg.ap_ssid          = "MyDevice";
 *  cfg.ap_password      = "setup1234";
 *  cfg.on_state_change  = my_state_cb;
 *  cfg.on_connected     = my_connected_cb;
 *  wifi_manager_start(&cfg);
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Portal network configuration ──────────────────────────────────────*/

/** IP address assigned to the ESP32 AP interface (and portal URL base). */
#define WIFI_MANAGER_AP_IP   "192.168.4.1"

/** Maximum simultaneous stations on the SoftAP. */
#define WIFI_MANAGER_AP_MAX_STA  4

/* ── State machine ──────────────────────────────────────────────────────
 *
 * States are delivered in order during a normal provisioning flow:
 *
 *   [boot, NVS creds found]
 *     CONNECTING_SAVED → STA_CONNECTED
 *
 *   [boot, no creds or STA failed]
 *     AP_STARTED → CLIENT_CONNECTED → CREDS_RECEIVED
 *       → STA_CONNECTING → STA_CONNECTED
 *
 *   [wrong password]
 *     ... STA_CONNECTING → STA_FAILED → (portal stays open)
 */
typedef enum {
    WIFI_MANAGER_STATE_CONNECTING_SAVED,  /**< Trying NVS-stored credentials   */
    WIFI_MANAGER_STATE_AP_STARTED,        /**< SoftAP up, captive portal live  */
    WIFI_MANAGER_STATE_CLIENT_CONNECTED,  /**< A device joined the AP          */
    WIFI_MANAGER_STATE_CLIENT_GONE,       /**< That device left the AP         */
    WIFI_MANAGER_STATE_CREDS_RECEIVED,    /**< User submitted SSID + password  */
    WIFI_MANAGER_STATE_STA_CONNECTING,    /**< Connecting to target network    */
    WIFI_MANAGER_STATE_STA_CONNECTED,     /**< Connected — provisioning done   */
    WIFI_MANAGER_STATE_STA_FAILED,        /**< Connection failed, will retry   */
    WIFI_MANAGER_STATE_ERROR,             /**< Unrecoverable error             */
} wifi_manager_state_t;

/* ── Callback signatures ────────────────────────────────────────────────*/

/**
 * Fired on every state transition.
 * @param state  New state value.
 * @param ctx    User pointer from wifi_manager_config_t::cb_ctx.
 */
typedef void (*wifi_manager_state_cb_t)(wifi_manager_state_t state, void *ctx);

/**
 * Fired once when the STA interface obtains an IP address.
 * @param ssid  SSID of the network that was joined.
 * @param ip    Assigned IPv4 address as a dotted-decimal string.
 * @param ctx   User pointer from wifi_manager_config_t::cb_ctx.
 */
typedef void (*wifi_manager_connected_cb_t)(const char *ssid, const char *ip,
                                            void *ctx);

/* ── Configuration ──────────────────────────────────────────────────────*/
typedef struct {
    const char *ap_ssid;         /**< SoftAP SSID (required, ≤32 chars)       */
    const char *ap_password;     /**< SoftAP password; NULL/"" = open network  */
    uint8_t     ap_channel;      /**< WiFi channel 1–13 (0 → use default 1)   */
    int         sta_max_retries; /**< STA reconnect attempts before giving up  */

    wifi_manager_state_cb_t     on_state_change; /**< State-change callback (optional) */
    wifi_manager_connected_cb_t on_connected;    /**< Connected callback (optional)    */
    void                       *cb_ctx;          /**< Passed as-is to both callbacks   */
} wifi_manager_config_t;

/** Reasonable defaults — override the fields you care about. */
#define WIFI_MANAGER_CONFIG_DEFAULT() {  \
    .ap_ssid         = "ESP32-Setup",   \
    .ap_password     = "setup1234",     \
    .ap_channel      = 1,               \
    .sta_max_retries = 5,               \
    .on_state_change = NULL,            \
    .on_connected    = NULL,            \
    .cb_ctx          = NULL,            \
}

/* ── Public API ─────────────────────────────────────────────────────────*/

/**
 * Start WiFi manager.
 *
 * Initialises NVS, netif, and the WiFi driver.  Tries NVS credentials first;
 * falls back to SoftAP portal if they are absent or stale.
 *
 * Non-blocking: returns after kicking off the internal state machine.
 * Progress is reported through the registered callbacks.
 *
 * @param config  Pointer to a populated wifi_manager_config_t.
 * @return ESP_OK, or an esp_err_t code on hard failure (e.g. OOM).
 */
esp_err_t wifi_manager_start(const wifi_manager_config_t *config);

/**
 * Erase stored credentials from NVS and restart the captive portal.
 * Safe to call at any time (e.g. from a "reset WiFi" button handler).
 */
esp_err_t wifi_manager_reset(void);

/**
 * Erase stored credentials without restarting.
 * Useful during factory reset flows.
 */
esp_err_t wifi_manager_erase_credentials(void);

/** Returns true if the STA interface currently has an IP address. */
bool wifi_manager_is_connected(void);

/**
 * Returns the current STA IP address as a string, or NULL if not connected.
 * The returned pointer is valid until the next disconnect event.
 */
const char *wifi_manager_get_ip(void);

#ifdef __cplusplus
}
#endif
