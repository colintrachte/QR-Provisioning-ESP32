#pragma once
/**
 * portal.h — SoftAP captive portal.
 *
 * Owns everything that happens while the device is in AP mode:
 *   - DNS hijack task (redirects all queries to AP_GW_IP)
 *   - OS captive-portal probe handlers (Windows, iOS, Android)
 *   - Network scan endpoint (/api/scan)
 *   - Credential POST handler (/api/connect)
 *   - Static file serving for setup.html, setup.css, setup.js
 *
 * portal_start() is called by wifi_manager when it determines no stored
 * credentials exist or STA connection has failed. It blocks the calling
 * context via a FreeRTOS event group until credentials are submitted, then
 * returns them to wifi_manager for the STA connection attempt.
 *
 * wifi_manager.c calls portal_stop() once STA is connected so the httpd
 * instance and DNS task are torn down before app_server_start() opens
 * its own httpd on port 80.
 *
 * Thread safety: portal_get_credentials() is called from the wifi_manager
 * task after portal_start() unblocks. All httpd handlers run on the httpd
 * task. The credential buffer is written once (on POST) and read once
 * (by wifi_manager) — no mutex needed.
 */

#include <stdbool.h>

/**
 * Start the captive portal: DNS hijack task + httpd with all portal routes.
 * Blocks until credentials are submitted via POST /api/connect, then returns.
 * @param ap_ssid  The device's own SoftAP SSID, shown in the portal header.
 */
void portal_start(const char *ap_ssid);

/**
 * Stop the captive portal: tear down httpd and DNS task.
 * Call after portal_start() returns and before app_server_start().
 */
void portal_stop(void);

/**
 * Retrieve the credentials submitted by the user.
 * Valid only after portal_start() has returned.
 * @param out_ssid  Buffer of at least 33 bytes for the network SSID.
 * @param out_pass  Buffer of at least 65 bytes for the password.
 */
void portal_get_credentials(char *out_ssid, char *out_pass);
