/**
 * prov_ui.c — OLED display state machine for the WiFi provisioning flow.
 *
 * Screen layout (128 × 64 px, SSD1306)
 * ─────────────────────────────────────
 *  Left  54 px  QR code at scale 2 (v1 QR = 21 modules → 42 px + quiet zone)
 *  x=56        Divider line
 *  Right 70 px  Human-readable equivalent of the QR content (x = 58 … 127)
 *  y = 54       Separator line
 *  y = 55-63    Status bar (8 px tall)
 *
 * Three QR codes cycle through provisioning
 * ──────────────────────────────────────────
 *  QR_WIFI   — WIFI: URI to auto-join the AP (shown when AP first starts)
 *  QR_SETUP  — URL to the captive portal setup page (shown when client joins)
 *  QR_INDEX  — URL to the robot control page (shown when connected to network)
 *
 * Split layout
 * ────────────
 *  Every QR screen also shows a human-readable text equivalent on the right
 *  half so users who cannot scan QR codes are never blocked.
 */

#include "prov_ui.h"
#include "display.h"
#include "qr_gen.h"
#include "wifi_manager.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
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

/* ── Stored connection info ──────────────────────────────────────────────────
 * Written by on_connected; cleared on state transitions that leave STA mode
 * so stale data is never shown if the device drops back to AP mode.
 */
static char s_ssid[33] = {0};
static char s_ip[16]   = {0};

/* ── Layout constants ───────────────────────────────────────────────────────*/
#define QR_X          0    /* QR code left edge                              */
#define QR_Y          0    /* QR code top edge                               */
#define QR_SCALE      2    /* 2 px/module → 42 px + 4 px quiet = 46 px wide */
#define DIVIDER_X    55    /* vertical divider between QR and text           */
#define TEXT_X       58    /* text column start                              */
#define TEXT_COL_W   68    /* available text column width (58…127)           */
#define STATUS_LINE_Y 54   /* separator above status bar                     */
#define STATUS_TEXT_Y 56   /* status bar text baseline                       */
#define CONTENT_H    54    /* usable height above status bar                 */

/* ── Status bar deferred message ─────────────────────────────────────────────
 * render_status_bar() is sometimes called just before a full-screen redraw
 * (e.g. CREDS_RECEIVED then immediately AP_STARTED).  We hold the message
 * for one tick so the status text survives partial redraws.
 *
 * Implementation: the last status message is stored and re-applied at the
 * bottom of every render_qr_screen() call, so a status update that arrives
 * just before a screen transition is never lost.
 */
static char s_status_msg[32] = {0};

/* ═══════════════════════════════════════════════════════════════════════════
 *  INTERNAL SCREEN RENDERERS
 * ═══════════════════════════════════════════════════════════════════════════*/

/** We don't like lines. just show the status bar from the stored message. */
static void render_chrome(void)
{
    //display_draw_vline(DIVIDER_X, 0, CONTENT_H);
    //display_draw_hline(0, STATUS_LINE_Y, DISP_WIDTH);
    if (s_status_msg[0]) {
        display_draw_text(2, STATUS_TEXT_Y, s_status_msg, DISP_FONT_SMALL);
    }
}

/**
 * Draw a QR code on the left and human-readable text on the right.
 *
 * @param slot   Which QR to render.
 * @param label1 First label line (e.g. "Join AP").
 * @param label2 Second label line (e.g. "RobotSetup").
 * @param label3 Third label line (small, e.g. "robot1234" or an IP).
 */
static void render_qr_screen(qr_slot_t slot,
                              const char *label1,
                              const char *label2,
                              const char *label3)
{
    uint8_t *qr    = NULL;
    bool     ready = false;

    switch (slot) {
    case QR_WIFI:
        qr = s_qr_wifi;  ready = s_qr_wifi_ok;  break;
    case QR_SETUP:
        qr = s_qr_setup; ready = s_qr_setup_ok; break;
    case QR_INDEX:
        qr = s_qr_index; ready = s_qr_index_ok; break;
    }

    display_clear();

    /* Left: QR code */
    if (ready) {
        display_draw_qr(QR_X, QR_Y, QR_SCALE, qr);
    } else {
        display_draw_text(2, 20, "QR N/A", DISP_FONT_SMALL);
    }

    /* Right: human-readable text equivalent */
    if (label1) display_draw_text(TEXT_X,  1, label1, DISP_FONT_SMALL);
    if (label2) display_draw_text(TEXT_X, 12, label2, DISP_FONT_SMALL);
    if (label3) display_draw_text(TEXT_X, 23, label3, DISP_FONT_SMALL);

    /* Chrome: divider + status bar (re-applies stored status msg so it is
     * never wiped by a screen transition that happens to race with a status
     * update). */
    render_chrome();

    display_flush();
    s_qr_shown = slot;
}

static void render_status_bar(const char *msg)
{
    /* Store the message so render_qr_screen() can re-apply it after a
     * full-screen clear, preventing the race described in the header. */
    if (msg) {
        strncpy(s_status_msg, msg, sizeof(s_status_msg) - 1);
        s_status_msg[sizeof(s_status_msg) - 1] = '\0';
    } else {
        s_status_msg[0] = '\0';
    }

    display_clear_region(0, STATUS_LINE_Y, DISP_WIDTH, DISP_HEIGHT - STATUS_LINE_Y);
    //display_draw_hline(0, STATUS_LINE_Y, DISP_WIDTH);
    if (s_status_msg[0]) {
        display_draw_text(2, STATUS_TEXT_Y, s_status_msg, DISP_FONT_SMALL);
    }
    display_flush();
}

static void render_connecting(const char *ssid)
{
    char trunc[16];
    snprintf(trunc, sizeof(trunc), "%.15s", (ssid && ssid[0]) ? ssid : "...");

    display_clear();
    display_draw_text(0,  0, "Connecting:",   DISP_FONT_NORMAL);
    //display_draw_hline(0, 12, DISP_WIDTH);
    display_draw_text(0, 15, trunc,           DISP_FONT_MEDIUM);
    display_draw_text(0, 36, "Please wait...", DISP_FONT_NORMAL);
    display_flush();
}

static void render_connected(const char *ssid, const char *ip)
{
    char trunc[16];
    snprintf(trunc, sizeof(trunc), "%.15s", ssid ? ssid : "");

    display_clear();
    display_draw_text(0,  0, ip,             DISP_FONT_BOLD);
    //display_draw_hline(0, 13, DISP_WIDTH);
    display_draw_text( 0, 16, trunc,         DISP_FONT_MEDIUM);
    display_flush();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════*/

void prov_ui_init(const char *ap_ssid, const char *ap_password)
{
    ESP_LOGI(TAG, "Generating QR codes");

    /* QR 1: WIFI: URI — phone camera auto-joins the AP */
    s_qr_wifi_ok = qr_gen_wifi(ap_ssid, ap_password, s_qr_wifi);

    /* QR 2: captive portal / setup page URL */
    s_qr_setup_ok = qr_gen_url("http://" WIFI_MANAGER_AP_IP "/", s_qr_setup);

    /* QR 3: robot control index page — generated with placeholder IP for
     * now; updated in prov_ui_on_connected() when the real IP is known. */
    s_qr_index_ok = false; /* not ready until we have an IP */

    if (!s_qr_wifi_ok)  ESP_LOGW(TAG, "WiFi QR generation failed");
    if (!s_qr_setup_ok) ESP_LOGW(TAG, "Setup URL QR generation failed");
}

void prov_ui_show_boot(void)
{
    display_clear();
    display_draw_text(14,  0, "WiFi Manager",       DISP_FONT_BOLD);
    //display_draw_hline(0, 15, DISP_WIDTH);
    display_draw_text( 4, 18, "WiFi Provisioning",  DISP_FONT_NORMAL);
    display_draw_text(22, 32, "Starting...",         DISP_FONT_NORMAL);
    //display_draw_hline(0, 53, DISP_WIDTH);
    display_draw_text( 2, 55, "Heltec V3  ESP32-S3", DISP_FONT_SMALL);
    display_flush();
    ESP_LOGI(TAG, "Boot screen shown");
}

void prov_ui_on_state_change(wifi_manager_state_t state, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "state_change -> %d", (int)state);

    switch (state) {

    case WIFI_MANAGER_STATE_CONNECTING_SAVED:
        render_status_bar("Trying saved WiFi...");
        break;

    case WIFI_MANAGER_STATE_AP_STARTED:
        /* Clear stale IP/SSID from any previous connection attempt. */
        s_ssid[0] = '\0';
        s_ip[0]   = '\0';

        render_qr_screen(QR_WIFI,
                         "Scan to Connect:",
                         AP_SSID,
                         AP_PASSWORD[0] ? AP_PASSWORD : "(open)");
        break;

    case WIFI_MANAGER_STATE_CLIENT_CONNECTED:
        /* Client joined AP — show setup page QR so they can open the portal. */
        render_qr_screen(QR_SETUP,
                         "Scan Again",
                         "to setup:",
                         "192.168.4.1");
        break;

    case WIFI_MANAGER_STATE_CLIENT_GONE:
        /* Client left — back to WiFi join QR. */
        render_qr_screen(QR_WIFI,
                         "Join AP:",
                         AP_SSID,
                         AP_PASSWORD[0] ? AP_PASSWORD : "(open)");
        break;

    case WIFI_MANAGER_STATE_CREDS_RECEIVED:
        render_status_bar("Credentials saved");
        break;

    case WIFI_MANAGER_STATE_STA_CONNECTING:
        /* Pass s_ssid explicitly — it is populated by CREDS_RECEIVED just
         * before this state fires, so it will be the SSID the user chose.
         * If it is still empty (should not normally happen) show "...". */
        render_connecting(s_ssid[0] ? s_ssid : "...");
        break;

    case WIFI_MANAGER_STATE_STA_CONNECTED:
        /* on_connected fires right after with ssid/ip — handled there. */
        break;

    case WIFI_MANAGER_STATE_STA_FAILED:
        /* Clear cached SSID so a stale name is never shown next round. */
        s_ssid[0] = '\0';
        render_status_bar("Failed - check password");
        break;

    case WIFI_MANAGER_STATE_ERROR:
        /* Clear caches. */
        s_ssid[0] = '\0';
        s_ip[0]   = '\0';

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

    /* Generate the index-page QR now that we have the real IP address. */
    char index_url[32];
    snprintf(index_url, sizeof(index_url), "http://%s/", s_ip);
    s_qr_index_ok = qr_gen_url(index_url, s_qr_index);
    if (!s_qr_index_ok) {
        ESP_LOGW(TAG, "Index page QR generation failed for URL: %s", index_url);
    }

    /* Show the index-page QR so the user can connect immediately.
     * Right side shows the IP in large text — also human-readable. */
    render_qr_screen(QR_INDEX,
                     "",
                     s_ip,
                     "");

    ESP_LOGI(TAG, "Connected: SSID=%s  IP=%s", s_ssid, s_ip);
}

void prov_ui_tick(void)
{
    /* Intentional no-op for now.
     * QR transitions are driven by wifi_manager state changes only.
     * This hook exists for forward-compatibility: e.g. a future "is anyone
     * browsing?" heartbeat check or slow cycling for specific deployments. */
}
