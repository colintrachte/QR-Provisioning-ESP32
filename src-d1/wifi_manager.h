/**
 * wifi_manager.h — STA/AP WiFi state machine for ESP8266.
 *
 * Non-blocking state machine polled from loop().
 */

#pragma once
#include <Arduino.h>

typedef enum {
    WIFI_MGR_STATE_IDLE,
    WIFI_MGR_STATE_CONNECTING_SAVED,
    WIFI_MGR_STATE_AP_STARTED,
    WIFI_MGR_STATE_CLIENT_CONNECTED,
    WIFI_MGR_STATE_CLIENT_DISCONNECTED,
    WIFI_MGR_STATE_CREDS_RECEIVED,
    WIFI_MGR_STATE_STA_CONNECTING,
    WIFI_MGR_STATE_STA_CONNECTED,
    WIFI_MGR_STATE_STA_FAILED,
    WIFI_MGR_STATE_ERROR,
} wifi_mgr_state_t;

typedef void (*wifi_mgr_state_cb_t)(wifi_mgr_state_t state, void *ctx);
typedef void (*wifi_mgr_connected_cb_t)(const char *ssid, const char *ip, void *ctx);

struct wifi_mgr_config_t {
    const char            *ap_ssid;
    const char            *ap_password;
    uint8_t                ap_channel;
    int                    sta_max_retries;
    wifi_mgr_state_cb_t    on_state_change;
    wifi_mgr_connected_cb_t on_connected;
    void                  *cb_ctx;
};

void wifi_mgr_start(const wifi_mgr_config_t *cfg);
void wifi_mgr_poll(void);
void wifi_mgr_set_credentials(const char *ssid, const char *pass);
void wifi_mgr_erase_credentials(void);
bool wifi_mgr_is_connected(void);
const char* wifi_mgr_get_ip(void);
wifi_mgr_state_t wifi_mgr_get_state(void);

/* For the portal: trigger a WiFi.scan and return result count.
 * The caller should use WiFi.SSID(i), WiFi.RSSI(i), etc. directly. */
int wifi_mgr_trigger_scan(void);
bool wifi_mgr_scan_done(void);
