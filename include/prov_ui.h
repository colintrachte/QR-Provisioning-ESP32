#pragma once
/**
 * prov_ui.h — OLED display state machine for the WiFi provisioning flow.
 *
 * Owns all display output while wifi_manager is running.  Translates
 * wifi_manager state changes into human-readable OLED screens.
 *
 * Screen sequence (normal flow)
 * ─────────────────────────────
 *   Boot splash
 *     → QR_WIFI  (join AP)         — AP starts
 *     → QR_SETUP (open portal)     — client device joins AP
 *     → "Connecting…"              — credentials submitted
 *     → QR_INDEX (robot control)   — STA connected, IP obtained
 *
 * Each QR screen shows the QR code on the left half and a human-readable
 * text equivalent on the right half, so users who cannot scan QR codes are
 * never blocked.
 *
 * Three QR codes
 * ──────────────
 *   QR_WIFI  — WIFI: URI.  Phone cameras recognise it and offer to
 *              join the SoftAP automatically.
 *              Format: WIFI:S:<ssid>;T:WPA;P:<password>;;
 *
 *   QR_SETUP — URL to the captive portal setup page.
 *              Format: http://192.168.4.1/
 *
 *   QR_INDEX — URL to the robot control page, with the real STA IP.
 *              Generated at connection time once the IP is known.
 *              Format: http://<ip>/
 *
 * Connected screen behaviour
 * ──────────────────────────
 *   When the STA is connected but no WebSocket client is open:
 *     shows QR_INDEX + IP so the operator can navigate there.
 *   When a WebSocket client connects (count > 0):
 *     shows plain IP + SSID — the QR is no longer needed.
 *   prov_ui_set_client_count() switches between these views.
 *
 * Thread safety
 * ─────────────
 * All display_* calls happen on the wifi_manager event task, which runs
 * callbacks on a dedicated FreeRTOS task — never from a WiFi ISR context.
 * display.c protects the u8g2 buffer with its own internal mutex.
 */

#include "wifi_manager.h"

typedef enum
{
    QR_WIFI,
    QR_SETUP,
    QR_INDEX
} qr_slot_t;

/**
 * Initialise the provisioning UI and pre-generate the WiFi and setup QR codes.
 * CPU-intensive (~50 ms); call before wifi_manager_start() to avoid blocking
 * the provisioning startup sequence.
 * Must be called after display_init().
 * The index-page QR is generated later in prov_ui_on_connected() once the
 * STA IP address is known.
 *
 * @param ap_ssid     SoftAP SSID to encode in the WiFi-join QR code.
 * @param ap_password SoftAP password (NULL or "" for open networks).
 */
void prov_ui_init(const char *ap_ssid, const char *ap_password);

/**
 * Render and flush the boot splash screen.
 * Call once immediately after display_init(), before prov_ui_init().
 */
void prov_ui_show_boot(void);

/**
 * WiFi manager state-change callback.
 * Pass as wifi_manager_config_t::on_state_change.
 *
 * State → screen mapping:
 *   CONNECTING_SAVED   → status bar "Trying saved WiFi…"
 *   AP_STARTED         → QR_WIFI  + AP SSID/password text
 *   CLIENT_CONNECTED   → QR_SETUP + setup URL text
 *   CLIENT_GONE        → QR_WIFI  (back to join screen)
 *   CREDS_RECEIVED     → status bar "Credentials saved"
 *   STA_CONNECTING     → "Connecting: <ssid>" full screen
 *   STA_CONNECTED      → (handled by on_connected below)
 *   STA_FAILED         → QR_WIFI + status bar "WiFi failed — retry?"
 *   ERROR              → full-screen error
 */
void prov_ui_on_state_change(wifi_manager_state_t state, void *ctx);

/**
 * WiFi manager connected callback.
 * Pass as wifi_manager_config_t::on_connected.
 * Generates the index-page QR with the real IP and renders the connected screen.
 *
 * @param ssid  Joined network name.
 * @param ip    Assigned IPv4 as dotted-decimal string.
 * @param ctx   User pointer (unused).
 */
void prov_ui_on_connected(const char *ssid, const char *ip, void *ctx);

/**
 * Update the connected-screen view based on current WebSocket client count.
 * Called by wifi_manager whenever a client connects or disconnects.
 * No-op if the STA IP is not yet known (not in connected state).
 *
 * @param count  Current number of open WebSocket connections.
 */
void prov_ui_set_client_count(int count);

/**
 * Tick — call from your main loop for API forward-compatibility.
 * Currently a no-op; all transitions are event-driven via on_state_change.
 * Reserved for future use (e.g. slow QR cycling, heartbeat-based detection).
 */
void prov_ui_tick(void);
