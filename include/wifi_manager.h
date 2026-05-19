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
    WIFI_MANAGER_STATE_STA_RETRYING,
    WIFI_MANAGER_STATE_ERROR,             /* Unrecoverable error            */
} wifi_manager_state_t;

typedef void (*wifi_manager_state_cb_t)(wifi_manager_state_t state, void *ctx);
typedef void (*wifi_manager_connected_cb_t)(const char *ssid, const char *ip, void *ctx);

/**
* @brief Configuration for wifi_manager_start().
 * on_radio_reset: Called from mgr_task (WiFi manager task) after
 * esp_wifi_set_mode() during AP reset. Executes on the same task
 * as the WiFi state machine. Keep this handler fast — it blocks
 * mgr_task while running. I2C reinit (~1ms) is acceptable; avoid
 * anything that could block for meaningful time (flash writes,
 * long delays, waiting on other tasks).
 */
typedef struct {
    const char *ap_ssid;          /**< SSID of the provisioning SoftAP (NULL = use default) */
    const char *ap_password;      /**< Password of the provisioning SoftAP (NULL = open) */
    uint8_t     ap_channel;       /**< Channel for SoftAP (1-13) */
    int         sta_max_retries;  /**< Maximum STA connection retries before portal fallback */
    void       *cb_ctx;           /**< Opaque context passed to all callbacks */
    wifi_manager_state_cb_t on_state_change;   /**< Called on every state change (may be called from WiFi task) */
    wifi_manager_connected_cb_t on_connected;  /**< Called once STA has IP (may be called from WiFi task) */
    void (*on_radio_reset)(void *ctx);         /**< Called after esp_wifi_set_mode() during AP/STA reset */
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
 * @brief Start the WiFi manager.
 *
 * Tries stored NVS credentials first. On failure or missing credentials
 * falls back to SoftAP + captive portal (portal.c).
 *
 * Non-blocking after return. All further progress is reported via callbacks.
 *
 * @param config Must remain valid for the lifetime of the manager.
 * @return ESP_OK on success, or ESP_ERR_NO_MEM / ESP_ERR_NVS_* on hard failure.
 */
esp_err_t wifi_manager_start(const wifi_manager_config_t *config);

/**
 * @brief Erase stored credentials and immediately restart provisioning (AP mode).
 *
 * @return ESP_OK on success, or an esp_err_t on NVS failure.
 */
esp_err_t wifi_manager_reset(void);

/**
 * @brief Erase stored credentials without triggering an immediate portal restart.
 *
 * Useful when the caller wants to control the transition.
 *
 * @return ESP_OK on success, or an esp_err_t on NVS failure.
 */
esp_err_t wifi_manager_erase_credentials(void);

/**
 * @brief Returns true if the STA interface currently holds a valid IP address.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Returns the current STA IP address as a dotted-decimal string.
 *
 * @return Pointer to static buffer (valid until next state change) or NULL if not connected.
 */
const char *wifi_manager_get_ip(void);

#ifdef __cplusplus
}
#endif
