/**
 * prov_ui.c — OLED display state machine for the WiFi provisioning flow.
 *
 * Screen layout (128 × 64 px, SSD1306)
 * ─────────────────────────────────────
 *  Left  55 px  QR code, auto-scaled to fit within QR_PANEL_W and DISP_HEIGHT.
 *  Right 70 px  Human-readable equivalent (x = 58 … 127).
 *  y = 55-63    Status bar (no separator line).
 *
 * QR scale selection
 * ──────────────────
 *  best_qr_scale() picks the largest integer scale (3 → 2 → 1) where the
 *  rendered image (modules × scale + quiet × 2) fits both dimensions.
 *  This prevents the 66 px overflow that occurs when a longer WIFI: URI
 *  (e.g. 38 chars) produces a v3 QR (29 modules) at the old hardcoded scale=2.
 *
 * Three QR codes used during provisioning
 * ────────────────────────────────────────
 *  QR_WIFI   — WIFI: URI so the phone camera can auto-join the AP.
 *  QR_SETUP  — URL to the captive-portal setup page.
 *  QR_INDEX  — URL to the robot control page (built once the real IP is known).
 */

#include "prov_ui.h"
#include "display.h"
#include "qr_gen.h"
#include "wifi_manager.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "qrcodegen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "prov_ui";

/* ── QR buffers ─────────────────────────────────────────────────────────────*/
static uint8_t s_qr_wifi [QR_BUF_LEN];
static uint8_t s_qr_setup[QR_BUF_LEN];
static uint8_t s_qr_index[QR_BUF_LEN];
static bool    s_qr_wifi_ok  = false;
static bool    s_qr_setup_ok = false;
static bool    s_qr_index_ok = false;

/* ── Display state ──────────────────────────────────────────────────────────*/
typedef enum { QR_WIFI = 0, QR_SETUP = 1, QR_INDEX = 2 } qr_slot_t;
static qr_slot_t s_qr_shown = QR_WIFI;

/* ── Stored connection info ─────────────────────────────────────────────────*/
static char s_ssid[33] = {0};
static char s_ip[16]   = {0};

/* ── Layout constants ───────────────────────────────────────────────────────*/
#define QR_X          0    /* QR left edge (including quiet zone)            */
#define QR_Y          0    /* QR top edge                                    */
#define QR_PANEL_W   55    /* max px for QR column; TEXT_X must exceed this  */
#define TEXT_X       58    /* right-column text start                        */
#define STATUS_CLR_Y 54    /* top of status-bar clear region                 */
#define STATUS_TEXT_Y 56   /* status text baseline                           */

/* ── Status bar message — persists across full-screen redraws ───────────────*/
static char s_status_msg[32] = {0};

/* ── QR scale helper ─────────────────────────────────────────────────────────
 * Returns the largest scale where the rendered QR fits within both QR_PANEL_W
 * (horizontal) and DISP_HEIGHT (vertical).  Falls back to 1 if nothing fits.
 *
 * Quiet zone: display_draw_qr() uses (scale < 3) ? scale*2 : 0 px.
 *
 * @param qrcode  Buffer produced by qrcodegen_encodeText().
 * @return        Scale factor (1, 2, or 3).
 */
static int best_qr_scale(const uint8_t *qrcode)
{
    int modules = qrcodegen_getSize(qrcode);
    const uint8_t quiet = 2; // modules
    const uint8_t total_modules = modules + (2 * quiet);

    /* * We want the largest 's' such that:
     * total_modules * s <= available_pixels
     * available_pixels = min(QR_PANEL_W, DISP_HEIGHT)
     */
    uint8_t limit = (QR_PANEL_W < DISP_HEIGHT) ? QR_PANEL_W : DISP_HEIGHT;
    uint8_t s = limit / total_modules;

    if (s < 1) return 1;
    if (s > 3) return 3; // Keep it within reasonable bounds for SSD1306
    return s;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  INTERNAL SCREEN RENDERERS
 * ═══════════════════════════════════════════════════════════════════════════*/

/* Re-draws the stored status message at the bottom of the screen.
 * Called at the end of every render_qr_screen() so a status update that
 * races with a full-screen transition is never silently dropped. */
static void render_chrome(void)
{
    if (s_status_msg[0]) {
        display_draw_text(2, STATUS_TEXT_Y, s_status_msg, DISP_FONT_SMALL);
    }
}

/**
 * Clear screen, draw QR on left (auto-scaled) and text labels on right.
 *
 * @param slot    Which pre-generated QR buffer to render.
 * @param label1  First right-column line (DISP_FONT_SMALL).
 * @param label2  Second right-column line.
 * @param label3  Third right-column line.
 */
static void render_qr_screen(qr_slot_t slot,
                              const char *label1,
                              const char *label2,
                              const char *label3)
{
    uint8_t *qr    = NULL;
    bool     ready = false;

    switch (slot) {
    case QR_WIFI:  qr = s_qr_wifi;  ready = s_qr_wifi_ok;  break;
    case QR_SETUP: qr = s_qr_setup; ready = s_qr_setup_ok; break;
    case QR_INDEX: qr = s_qr_index; ready = s_qr_index_ok; break;
    }

    display_clear();

    if (ready) {
        int scale = best_qr_scale(qr);
        ESP_LOGD(TAG, "render_qr slot=%d scale=%d modules=%d",
                 (int)slot, scale, qrcodegen_getSize(qr));
        display_draw_qr(QR_X, QR_Y, scale, qr);
    } else {
        display_draw_text(2, 20, "QR N/A", DISP_FONT_SMALL);
    }

    if (label1 && label1[0]) display_draw_text(TEXT_X,  1, label1, DISP_FONT_SMALL);
    if (label2 && label2[0]) display_draw_text(TEXT_X, 12, label2, DISP_FONT_SMALL);
    if (label3 && label3[0]) display_draw_text(TEXT_X, 23, label3, DISP_FONT_SMALL);

    render_chrome();
    display_flush();
    s_qr_shown = slot;
}

/**
 * Update only the status bar (bottom ~10 px) without redrawing the QR.
 * Stores msg so render_qr_screen() re-applies it after a full clear.
 *
 * @param msg  Text to display, or NULL to clear.
 */
static void render_status_bar(const char *msg)
{
    if (msg) {
        strncpy(s_status_msg, msg, sizeof(s_status_msg) - 1);
        s_status_msg[sizeof(s_status_msg) - 1] = '\0';
    } else {
        s_status_msg[0] = '\0';
    }

    display_clear_region(0, STATUS_CLR_Y, DISP_WIDTH, DISP_HEIGHT - STATUS_CLR_Y);
    if (s_status_msg[0]) {
        display_draw_text(2, STATUS_TEXT_Y, s_status_msg, DISP_FONT_SMALL);
    }
    display_flush();
}

/**
 * Full-screen "connecting" splash shown while STA attempts to join.
 *
 * @param ssid  Network name to display (truncated to 15 chars).
 */
static void render_connecting(const char *ssid)
{
    char trunc[16];
    snprintf(trunc, sizeof(trunc), "%.15s", (ssid && ssid[0]) ? ssid : "...");

    s_status_msg[0] = '\0'; /* connecting screen owns the full display */
    display_clear();
    display_draw_text( 0,  0, "Connecting:",    DISP_FONT_NORMAL);
    display_draw_text( 0, 15, trunc,            DISP_FONT_MEDIUM);
    display_draw_text( 0, 36, "Please wait...", DISP_FONT_NORMAL);
    display_flush();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════*/

void prov_ui_init(const char *ap_ssid, const char *ap_password)
{
    ESP_LOGI(TAG, "Generating QR codes");

    s_qr_wifi_ok  = qr_gen_wifi(ap_ssid, ap_password, s_qr_wifi);
    s_qr_setup_ok = qr_gen_url("http://" WIFI_MANAGER_AP_IP "/", s_qr_setup);
    s_qr_index_ok = false; /* built in prov_ui_on_connected() once IP is known */

    if (!s_qr_wifi_ok)  ESP_LOGW(TAG, "WiFi QR generation failed");
    if (!s_qr_setup_ok) ESP_LOGW(TAG, "Setup URL QR generation failed");
}

void prov_ui_show_boot(void)
{
    s_status_msg[0] = '\0';
    display_clear();
    display_draw_text(0,  0, "WiFi Manager",        DISP_FONT_BOLD);
    display_draw_text(0, 18, "Starting...",   DISP_FONT_NORMAL);
    display_flush();
    ESP_LOGI(TAG, "Boot screen shown");
}

void prov_ui_on_state_change(wifi_manager_state_t state, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "state_change -> %d", (int)state);

    switch (state) {

    case WIFI_MANAGER_STATE_CONNECTING_SAVED:
        /* Overlay status on boot screen — no full redraw needed. */
        render_status_bar("Trying saved WiFi...");
        break;

    case WIFI_MANAGER_STATE_AP_STARTED:
        s_ssid[0] = '\0';
        s_ip[0]   = '\0';
        s_status_msg[0] = '\0'; /* clear any leftover boot/retry message */
        render_qr_screen(QR_WIFI,
                         "Scan to join:",
                         AP_SSID,
                         AP_PASSWORD[0] ? AP_PASSWORD : "(open)");
        break;

    case WIFI_MANAGER_STATE_CLIENT_CONNECTED:
        render_qr_screen(QR_SETUP,
                         "Scan to setup:",
                         "192.168.4.1",
                         "");
        break;

    case WIFI_MANAGER_STATE_CLIENT_GONE:
        render_qr_screen(QR_WIFI,
                         "Join AP:",
                         AP_SSID,
                         AP_PASSWORD[0] ? AP_PASSWORD : "(open)");
        break;

    case WIFI_MANAGER_STATE_CREDS_RECEIVED:
        render_status_bar("Credentials saved");
        break;

    case WIFI_MANAGER_STATE_STA_CONNECTING:
        render_connecting(s_ssid[0] ? s_ssid : "...");
        break;

    case WIFI_MANAGER_STATE_STA_CONNECTED:
        /* prov_ui_on_connected() fires immediately after with ssid + ip. */
        break;

    case WIFI_MANAGER_STATE_STA_FAILED:
        s_ssid[0] = '\0';
        /* Redraw the QR so the user can try again; status note at bottom. */
        s_status_msg[0] = '\0';
        render_qr_screen(QR_WIFI,
                         "Join AP:",
                         AP_SSID,
                         AP_PASSWORD[0] ? AP_PASSWORD : "(open)");
        render_status_bar("WiFi failed — retry?");
        break;

    case WIFI_MANAGER_STATE_ERROR:
        s_ssid[0] = '\0';
        s_ip[0]   = '\0';
        s_status_msg[0] = '\0';
        display_clear();
        display_draw_text(28, 16, "ERROR",            DISP_FONT_BOLD);
        display_draw_text( 4, 36, "Check serial log", DISP_FONT_NORMAL);
        display_flush();
        break;
    }
}

void prov_ui_on_connected(const char *ssid, const char *ip, void *ctx)
{
    (void)ctx;

    strncpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid) - 1);
    strncpy(s_ip,   ip   ? ip   : "", sizeof(s_ip)   - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    s_ip[sizeof(s_ip)     - 1] = '\0';

    char index_url[32];
    snprintf(index_url, sizeof(index_url), "http://%s/", s_ip);
    s_qr_index_ok = qr_gen_url(index_url, s_qr_index);
    if (!s_qr_index_ok) {
        ESP_LOGW(TAG, "Index page QR generation failed: %s", index_url);
    }

    s_status_msg[0] = '\0';
    render_qr_screen(QR_INDEX, "", s_ip, "");
    ESP_LOGI(TAG, "Connected: SSID=%s  IP=%s", s_ssid, s_ip);
}

void prov_ui_tick(void)
{
    /* No-op — transitions are driven by prov_ui_on_state_change(). */
}
