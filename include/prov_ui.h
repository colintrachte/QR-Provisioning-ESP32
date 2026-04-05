#pragma once
/**
 * prov_ui.h — OLED display state machine for the WiFi provisioning flow.
 *
 * Owns all display output while wifi_manager is running.  Translates
 * wifi_manager state changes into human-readable OLED screens.
 *
 * Screen sequence (normal flow)
 * ─────────────────────────────
 *   Boot splash  →  WiFi QR (join AP)  →  URL QR (open portal)
 *     →  "Connecting…"  →  "Connected / IP"
 *
 * The WiFi QR lets phone cameras auto-join the SoftAP.
 * The URL QR is a fallback for phones where the captive-portal redirect
 * did not fire; it points directly at http://192.168.4.1/.
 * The display switches between QR codes only on state transitions —
 * never on a timer.
 *
 * Thread safety
 * ─────────────
 * All display_* calls happen on the wifi_manager event task, which runs
 * callbacks on a dedicated FreeRTOS task — never from a WiFi ISR context.
 */

#include "wifi_manager.h"

/**
 * Initialise the provisioning UI and pre-generate both QR codes.
 * CPU-intensive (~50 ms); call before wifi_manager_start() to avoid
 * blocking the provisioning startup sequence.
 * Must be called after display_init().
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
 */
void prov_ui_on_state_change(wifi_manager_state_t state, void *ctx);

/**
 * WiFi manager connected callback.
 * Pass as wifi_manager_config_t::on_connected.
 */
void prov_ui_on_connected(const char *ssid, const char *ip, void *ctx);

/**
 * No-op tick — call from your main loop for API forward-compatibility.
 *
 * QR transitions happen only on explicit wifi_manager state changes, never
 * on a timer.  The display is redrawn only when something meaningful occurs.
 * This function is a stable hook in case a specific deployment ever needs
 * periodic behaviour; for now it does nothing.
 */
void prov_ui_tick(void);
