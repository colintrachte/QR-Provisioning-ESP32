/**
 * prov_ui.c — OLED state machine for the WiFi provisioning flow.
 *
 * Layout (128×64 SSD1306): QR on left (scale 2), text labels on right
 * (x=62+), status bar at y=54-63.
 *
 * QR codes are always rendered at scale 2. Scale 1 is unreadable on most
 * phone cameras; scale 3 overflows the panel for typical SSID/URL lengths.
 *
 * QR slots: QR_WIFI (join AP), QR_SETUP (portal URL), QR_INDEX (robot URL).
 * QR_INDEX is generated in prov_ui_on_connected() once the STA IP is known.
 * On the connected screen the QR is centred above a full-width IP address
 * row so the address never clips regardless of digit count.
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

/* ── Layout ─────────────────────────────────────────────────────────────────
 *
 * AP / setup screens (QR_WIFI, QR_SETUP):
 *   QR on the left half (≤60 px), text labels on the right (x=62+).
 *   These QR codes encode short strings and fit easily in ≤60 px.
 *
 * Connected screen (QR_INDEX):
 *   QR centred at top, IP address on its own full-width line below.
 *   The IP can be up to 15 chars (e.g. "192.168.100.200") — we need the
 *   full 128 px width to avoid clipping; DISP_FONT_SMALL is ~6 px/char so
 *   15 chars = 90 px, which fits comfortably at x=0.
 *   We do NOT use the status-bar chrome row on this screen.
 */
#define QR_X          0
#define QR_Y          0
#define TEXT_X       62   /* right column for AP/setup screens               */
#define STATUS_CLR_Y 54
#define STATUS_TEXT_Y 56

/* Scale 1 is unreadable on most phone cameras. Fixed at 2. */
#define QR_SCALE      2

/* Height of the IP address row on the connected (index) screen. */
#define IP_ROW_H     12

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
        display_draw_qr(QR_X, QR_Y, QR_SCALE, qr);
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
 *
 * No client: QR centred at top, IP address on a full-width line below it.
 *   The IP must not be word-wrapped or split; we use the full 128 px row.
 *   DISP_FONT_SMALL renders at ~6 px/char — 15 chars (worst-case IP) = 90 px.
 *
 * Client present: plain IP + SSID, QR omitted — the user is already there.
 */
static void render_connected(void)
{
    if (!s_ip[0]) return;

    display_clear();

    if (s_ws_clients > 0)
    {
        display_draw_text(0,  0, "Connected:",  DISP_FONT_NORMAL);
        display_draw_text(0, 16, s_ip,          DISP_FONT_MEDIUM);
        display_draw_text(0, 36, s_ssid,        DISP_FONT_SMALL);
    }
    else
    {
        /* QR in upper portion, IP address on its own row below — no side column. */
        if (s_qr_index_ok)
        {
            int qr_px = qrcodegen_getSize(s_qr_index) * QR_SCALE;
            int qr_x  = (DISP_WIDTH - qr_px) / 2;   /* centre horizontally */
            display_draw_qr(qr_x, QR_Y, QR_SCALE, s_qr_index);
        }
        else
        {
            display_draw_text(0, QR_Y, "Scan to open:", DISP_FONT_SMALL);
        }

        /* IP on a dedicated full-width row — guaranteed no wrapping. */
        int ip_y = DISP_HEIGHT - IP_ROW_H;
        //do NOT draw an hline above the IP row — it would bisect the QR code and make it unscannable
        display_draw_text(0, ip_y, s_ip, DISP_FONT_SMALL);
    }

    display_flush();
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

    case WIFI_MANAGER_STATE_CLIENT_DISCONNECTED:
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
