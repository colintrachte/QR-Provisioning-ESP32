/**
 * prov_ui.c — OLED state machine for the WiFi provisioning flow.
 *
 * Layout (128×64 SSD1306): QR on left (≤64 px wide), text labels on right
 * (x=68+), status bar at y=54-63.  best_qr_scale() auto-fits QR to panel.
 *
 * QR slots: QR_WIFI (join AP), QR_SETUP (portal URL), QR_INDEX (robot URL).
 * QR_INDEX is generated in prov_ui_on_connected() once the STA IP is known.
 * When a WebSocket client is connected, the index screen shows just the IP
 * (no QR) since the user is clearly already there.
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

static const char *TAG = "prov_ui";

/* ── QR buffers ─────────────────────────────────────────────────────────────*/
static uint8_t s_qr_wifi [QR_BUF_LEN];
static uint8_t s_qr_setup[QR_BUF_LEN];
static uint8_t s_qr_index[QR_BUF_LEN];
static bool    s_qr_wifi_ok  = false;
static bool    s_qr_setup_ok = false;
static bool    s_qr_index_ok = false;

/* ── Runtime state ──────────────────────────────────────────────────────────*/
static char    s_ssid[33]       = {0};
static char    s_ip[16]         = {0};
static char    s_status_msg[32] = {0};
static int     s_ws_clients     = 0;   /* set by prov_ui_set_client_count()  */

/* ── Layout ─────────────────────────────────────────────────────────────────*/
#define QR_X          0
#define QR_Y          0
#define QR_PANEL_W   64   /* max px wide for QR; TEXT_X must exceed this     */
#define TEXT_X       68
#define STATUS_CLR_Y 54
#define STATUS_TEXT_Y 56

/* ── Scale selection ────────────────────────────────────────────────────────
 * Returns the largest scale (3→2→1) where modules*scale + quiet*2 fits the
 * QR panel and display height.  Falls back to 1 with a warning if nothing
 * fits — the image will be clipped but display_draw_qr() already logs that. */
static uint8_t best_qr_scale(const uint8_t *qrcode)
{
    uint8_t modules = qrcodegen_getSize(qrcode);
    for (uint8_t s = 3; s >= 1; s--)
    {
        uint8_t quiet    = (s >= 3) ? 0 : (s * 2);
        uint8_t total_px = modules * s + quiet * 2;
        if (total_px <= QR_PANEL_W && total_px <= DISP_HEIGHT)
            return s;
    }
    ESP_LOGW(TAG, "best_qr_scale: QR (%d modules) too large for panel — using scale 1", modules);
    return 1;
}

/* ── Internal renderers ─────────────────────────────────────────────────────*/

/* Redraws the persisted status message in the bottom bar. Called at the end
 * of every full-screen render so a racing status update is never lost. */
static void render_chrome(void)
{
    if (s_status_msg[0])
        display_draw_text(2, STATUS_TEXT_Y, s_status_msg, DISP_FONT_SMALL);
}

/**
 * Full redraw: QR on left, up to three text labels on right.
 * @param slot    Which pre-generated QR buffer to use.
 * @param label1  Right-column line 1 (DISP_FONT_SMALL).
 * @param label2  Right-column line 2.
 * @param label3  Right-column line 3.
 */
static void render_qr_screen(qr_slot_t slot,
                              const char *label1,
                              const char *label2,
                              const char *label3)
{
    uint8_t *qr    = NULL;
    bool     ready = false;

    switch (slot)
    {
        case QR_WIFI:  qr = s_qr_wifi;  ready = s_qr_wifi_ok;  break;
        case QR_SETUP: qr = s_qr_setup; ready = s_qr_setup_ok; break;
        case QR_INDEX: qr = s_qr_index; ready = s_qr_index_ok; break;
    }

    display_clear();

    if (ready)
    {
        int scale = best_qr_scale(qr);
        ESP_LOGD(TAG, "render_qr slot=%d scale=%d modules=%d",
                 (int)slot, scale, qrcodegen_getSize(qr));
        display_draw_qr(QR_X, QR_Y, scale, qr);
    }
    else
    {
        display_draw_text(QR_X, QR_Y, "QR N/A", DISP_FONT_SMALL);
    }

    if (label1 && label1[0]) display_draw_text(TEXT_X,  1, label1, DISP_FONT_SMALL);
    if (label2 && label2[0]) display_draw_text(TEXT_X, 12, label2, DISP_FONT_SMALL);
    if (label3 && label3[0]) display_draw_text(TEXT_X, 23, label3, DISP_FONT_SMALL);

    render_chrome();
    display_flush();
}

/**
 * Partial update: rewrite only the bottom status bar row.
 * Persists msg so the next render_qr_screen() call repaints it.
 * @param msg  Text to show, or NULL to clear.
 */
static void render_status_bar(const char *msg)
{
    if (msg)
    {
        strncpy(s_status_msg, msg, sizeof(s_status_msg) - 1);
        s_status_msg[sizeof(s_status_msg) - 1] = '\0';
    }
    else
    {
        s_status_msg[0] = '\0';
    }

    display_clear_region(0, STATUS_CLR_Y, DISP_WIDTH, DISP_HEIGHT - STATUS_CLR_Y);
    if (s_status_msg[0])
        display_draw_text(2, STATUS_TEXT_Y, s_status_msg, DISP_FONT_SMALL);
    display_flush();
}

/**
 * Full-screen connecting splash while STA attempts to associate.
 * @param ssid  Network name to display (truncated to 15 chars).
 */
static void render_connecting(const char *ssid)
{
    char trunc[16];
    snprintf(trunc, sizeof(trunc), "%.15s", (ssid && ssid[0]) ? ssid : "...");

    s_status_msg[0] = '\0';
    display_clear();
    display_draw_text(0,  0, "Connecting:",    DISP_FONT_NORMAL);
    display_draw_text(0, 15, trunc,            DISP_FONT_MEDIUM);
    display_draw_text(0, 36, "Please wait...", DISP_FONT_NORMAL);
    display_flush();
}

/* Redraws the connected screen based on current client count.
 * With no browser connected: show QR + IP so the user can navigate there.
 * With a client present: show just the IP — they're already in the UI. */
static void render_connected(void)
{
    if (!s_ip[0]) return;

    if (s_ws_clients > 0)
    {
        display_clear();
        display_draw_text(0,  0, "Connected:",  DISP_FONT_NORMAL);
        display_draw_text(0, 16, s_ip,          DISP_FONT_MEDIUM);
        display_draw_text(0, 36, s_ssid,        DISP_FONT_SMALL);
        display_flush();
    }
    else
    {
        render_qr_screen(QR_INDEX, "Scan to open:", s_ip, "");
    }
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

void prov_ui_init(const char *ap_ssid, const char *ap_password)
{
    ESP_LOGI(TAG, "Generating QR codes");
    s_qr_wifi_ok  = qr_gen_wifi(ap_ssid, ap_password, s_qr_wifi);
    s_qr_setup_ok = qr_gen_url("http://" WIFI_MANAGER_AP_IP "/", s_qr_setup);
    s_qr_index_ok = false;  /* generated in prov_ui_on_connected() */
    if (!s_qr_wifi_ok)  ESP_LOGW(TAG, "WiFi QR generation failed");
    if (!s_qr_setup_ok) ESP_LOGW(TAG, "Setup URL QR generation failed");
}

void prov_ui_show_boot(void)
{
    s_status_msg[0] = '\0';
    display_clear();
    display_draw_text(0,  0, "WiFi Manager", DISP_FONT_BOLD);
    display_draw_text(0, 18, "Starting...",  DISP_FONT_NORMAL);
    display_flush();
    ESP_LOGI(TAG, "Boot screen shown");
}

void prov_ui_on_state_change(wifi_manager_state_t state, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "state -> %d", (int)state);

    switch (state)
    {
    case WIFI_MANAGER_STATE_CONNECTING_SAVED:
        render_status_bar("Trying saved WiFi...");
        break;

    case WIFI_MANAGER_STATE_AP_STARTED:
        s_ssid[0] = '\0';
        s_ip[0]   = '\0';
        s_status_msg[0] = '\0';
        render_qr_screen(QR_WIFI, "Scan to join:",
                         AP_SSID, AP_PASSWORD[0] ? AP_PASSWORD : "(open)");
        break;

    case WIFI_MANAGER_STATE_CLIENT_CONNECTED:
        render_qr_screen(QR_SETUP, "Scan to setup:", WIFI_MANAGER_AP_IP, "");
        break;

    case WIFI_MANAGER_STATE_CLIENT_GONE:
        render_qr_screen(QR_WIFI, "Join AP:",
                         AP_SSID, AP_PASSWORD[0] ? AP_PASSWORD : "(open)");
        break;

    case WIFI_MANAGER_STATE_CREDS_RECEIVED:
        render_status_bar("Credentials saved");
        break;

    case WIFI_MANAGER_STATE_STA_CONNECTING:
        render_connecting(s_ssid[0] ? s_ssid : "...");
        break;

    case WIFI_MANAGER_STATE_STA_CONNECTED:
        /* prov_ui_on_connected() fires next with ssid + ip */
        break;

    case WIFI_MANAGER_STATE_STA_FAILED:
        s_ssid[0]       = '\0';
        s_status_msg[0] = '\0';
        render_qr_screen(QR_WIFI, "Join AP:",
                         AP_SSID, AP_PASSWORD[0] ? AP_PASSWORD : "(open)");
        render_status_bar("WiFi failed \xe2\x80\x94 retry?");
        break;

    case WIFI_MANAGER_STATE_ERROR:
        s_ssid[0] = s_ip[0] = s_status_msg[0] = '\0';
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
    s_ip  [sizeof(s_ip)   - 1] = '\0';

    char index_url[32];
    snprintf(index_url, sizeof(index_url), "http://%s/", s_ip);
    s_qr_index_ok = qr_gen_url(index_url, s_qr_index);
    if (!s_qr_index_ok)
        ESP_LOGW(TAG, "Index QR failed: %s", index_url);

    s_status_msg[0] = '\0';
    s_ws_clients    = 0;
    render_connected();
    ESP_LOGI(TAG, "Connected: SSID=%s  IP=%s", s_ssid, s_ip);
}

/**
 * Called by wifi_manager when the WebSocket client count changes.
 * Redraws the connected screen: QR when no client, plain IP when one is active.
 * @param count  Current number of open WebSocket connections.
 */
void prov_ui_set_client_count(int count)
{
    s_ws_clients = count;
    if (s_ip[0])  /* only act if we're in the connected state */
        render_connected();
}

void prov_ui_tick(void)
{
    /* No-op — all transitions are event-driven via prov_ui_on_state_change(). */
}
