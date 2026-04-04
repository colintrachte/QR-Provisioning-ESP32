#pragma once
/**
 * prov_ui.h — Display state machine for the provisioning flow.
 *
 * Owns all display output during provisioning.  Responds to wifi_prov
 * state changes and manages the dual-QR cycling sequence.
 *
 * All display_* calls happen in prov_ui's own context (called from
 * main task or via the registered callback).  Never touches the display
 * from a WiFi event handler.
 *
 * QR cycling:
 *   When in AP_STARTED state, the display cycles every QR_CYCLE_MS between:
 *     1. WIFI URI QR  — phone camera offers to join the AP automatically
 *     2. URL QR       — http://192.168.4.1, for phones that miss captive redirect
 *   Cycling stops as soon as a phone connects (CLIENT_CONNECTED state).
 */

#include "wifi_prov.h"

/* Milliseconds between QR code switches when cycling */
#define QR_CYCLE_MS 5000

/**
 * Initialise the provisioning UI.
 * Generates both QR codes (requires qr_gen to be available).
 * Must be called AFTER display_init().
 *
 * @param ap_ssid     The SoftAP SSID to encode in the WIFI QR
 * @param ap_password The SoftAP password (NULL or "" for open)
 */
void prov_ui_init(const char *ap_ssid, const char *ap_password);

/**
 * Show the boot splash screen.  Call once immediately after display_init().
 * Flushes to hardware.
 */
void prov_ui_show_boot(void);

/**
 * WiFi provisioning state-change callback — pass this to wifi_prov_config_t.
 * Signature matches wifi_prov_state_cb_t.
 */
void prov_ui_on_state_change(wifi_prov_state_t state, void *ctx);

/**
 * WiFi connected callback — pass this to wifi_prov_config_t.
 * Signature matches wifi_prov_connected_cb_t.
 */
void prov_ui_on_connected(const char *ssid, const char *ip, void *ctx);

/**
 * Drive the QR cycling timer.  Call from app_main's idle loop or a tick task.
 * This function is non-blocking; it checks elapsed time internally and only
 * redraws + flushes when a switch is due.
 */
void prov_ui_tick(void);
