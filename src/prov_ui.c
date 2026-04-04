/**
 * prov_ui.c — Provisioning display state machine (u8g2 version)
 *
 * Logic identical to the previous version; only drawing calls updated:
 *   display_draw_text(x, y, str, FONT_6X8)  →  display_draw_text(x, y, str, DISP_FONT_NORMAL)
 *   display_fill_rect(x, y, w, h, false)   →  display_clear_region(x, y, w, h)
 *   display_flush_page(7)                  →  display_flush()
 *     (u8g2 sends the full frame; we still gate it on our explicit call)
 *
 * Font choices:
 *   DISP_FONT_SMALL  (5x7)   — status bar (page 7 equivalent)
 *   DISP_FONT_NORMAL (6x10)  — body labels
 *   DISP_FONT_MEDIUM (7x13)  — section headers
 *   DISP_FONT_BOLD   (8x13B) — boot title, success heading
 *
 * Screen layout (128x64):
 *   Left 52px: QR code (scale=2, version 1 = 21 modules → 42px + 4px quiet = 50px)
 *   x=52: vertical divider
 *   Right 75px (x=53..127): label text
 *   y=55: horizontal rule
 *   y=56..63: status bar (8px)
 */

#include "prov_ui.h"
#include "display.h"
#include "qr_gen.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "prov_ui";

/* ── QR buffers ─────────────────────────────────────────────────────────*/
static uint8_t s_qr_wifi[QR_BUF_LEN];
static uint8_t s_qr_url [QR_BUF_LEN];
static bool    s_qr_wifi_ready = false;
static bool    s_qr_url_ready  = false;

/* ── Cycling state ───────────────────────────────────────────────────────*/
typedef enum { QR_SHOW_WIFI = 0, QR_SHOW_URL = 1 } qr_which_t;

static qr_which_t s_qr_current     = QR_SHOW_WIFI;
static bool       s_cycling_active = false;
static TickType_t s_last_switch    = 0;

/* ── Provisioning state ──────────────────────────────────────────────────*/
static wifi_prov_state_t s_prov_state = WIFI_PROV_STATE_CONNECTING_SAVED;
static char s_connected_ssid[33] = {0};
static char s_connected_ip[16]   = {0};

/* ═══════════════════════════════════════════════════════════════════════*/
/*  Internal render helpers                                               */
/* ═══════════════════════════════════════════════════════════════════════*/

/* Draw QR code into the left 52px column */
static void render_qr_left(qr_which_t which)
{
    display_clear_region(0, 0, 70, 70);//only need quiet zone padding on the right side.
    bool     ready = (which == QR_SHOW_WIFI) ? s_qr_wifi_ready : s_qr_url_ready;
    uint8_t *qr    = (which == QR_SHOW_WIFI) ? s_qr_wifi       : s_qr_url;

    if (!ready) {
        display_draw_text(0, 20, "QR N/A", DISP_FONT_SMALL);
        return;
    }
    /* QR at x=1, y=3, scale=2: version-1 QR (21 mod) → 42px + 4px quiet = 50px */
    display_draw_qr(0, 0, 2, qr);
}

/* Right panel labels for WIFI QR screen */
static void render_wifi_labels(void)
{
    display_clear_region(70, 0, 75, 66);
    //display_clear_region(70, 0, 1, 66);   /* clear divider column */
    display_draw_text(70,  0, "Scan Me",   DISP_FONT_NORMAL);
    display_draw_text(70, 11, "to setup",   DISP_FONT_NORMAL);
}

/* Right panel labels for URL QR screen */
static void render_url_labels(void)
{
    display_clear_region(70, 0, 75, 56);
    display_draw_text(70,  0, "Scan Me",   DISP_FONT_NORMAL);
    display_draw_text(70, 11, "to join",   DISP_FONT_NORMAL);
}

/* Status bar — y=56..63, separated by a rule at y=55.
   Erases just that region, redraws text, and flushes the full buffer.
   u8g2 doesn't support partial-page sends, so we flush the whole thing.
   This is fine: the call is still gated on our explicit decision. */
static void set_status_bar(const char *msg)
{
    display_clear_region(0, 55, 128, 9);
    display_draw_hline(0, 55, 128);
    display_draw_text(2, 57, msg, DISP_FONT_SMALL);
    display_flush();   /* push full frame — status bar change triggers the flush */
}

/* ═══════════════════════════════════════════════════════════════════════*/
/*  Screen renderers                                                      */
/* ═══════════════════════════════════════════════════════════════════════*/

void prov_ui_show_boot(void)
{
    display_clear();
    display_draw_text(14,  0, "RobotOS",         DISP_FONT_BOLD);
    display_draw_hline(0, 15, 128);
    display_draw_text( 4, 18, "WiFi Provisioning", DISP_FONT_NORMAL);
    display_draw_text(22, 32, "Starting...",       DISP_FONT_NORMAL);
    display_draw_hline(0, 53, 128);
    display_draw_text( 2, 55, "Heltec V3  ESP32-S3", DISP_FONT_SMALL);
    display_flush();
    ESP_LOGI(TAG, "Boot screen up");
}

static void show_ap_qr_screen(qr_which_t which)
{
    render_qr_left(which);
    if (which == QR_SHOW_WIFI) render_wifi_labels();
    else                        render_url_labels();
    /* Status bar */
    //display_clear_region(0, 55, 128, 9);
    //display_draw_hline(0, 55, 128);
    //display_draw_text(2, 57, "Waiting for phone...", DISP_FONT_SMALL);
    display_flush();
}

static void show_connecting_screen(const char *ssid)
{
    display_clear();
    display_draw_text(0,  0, "Connecting to:",  DISP_FONT_NORMAL);
    display_draw_hline(0, 12, 128);
    char trunc[22];
    snprintf(trunc, sizeof(trunc), "%.21s", ssid[0] ? ssid : "...");
    display_draw_text(0, 15, trunc, DISP_FONT_MEDIUM);
    display_draw_text(0, 36, "Please wait...", DISP_FONT_NORMAL);
    display_flush();
}

static void show_success_screen(const char *ssid, const char *ip)
{
    display_clear();
    display_draw_text(40,  0, "OK!",          DISP_FONT_BOLD);
    display_draw_hline(0, 13, 128);
    display_draw_text( 0, 16, "Connected to:", DISP_FONT_NORMAL);
    char trunc[22];
    snprintf(trunc, sizeof(trunc), "%.21s", ssid);
    display_draw_text( 0, 27, trunc,          DISP_FONT_MEDIUM);
    display_draw_hline(0, 42, 128);
    display_draw_text( 0, 45, "IP address:",  DISP_FONT_NORMAL);
    display_draw_text( 0, 54, ip,             DISP_FONT_BOLD);
    display_flush();
    ESP_LOGI(TAG, "Success: %s  %s", ssid, ip);
}

static void show_failed_screen(void)
{
    set_status_bar("Failed! Check password");
}

static void show_client_connected_screen(void)
{
    set_status_bar("Phone connected!");
}

static void show_creds_received_screen(void)
{
    set_status_bar("Credentials received");
}

/* ═══════════════════════════════════════════════════════════════════════*/
/*  Public API                                                            */
/* ═══════════════════════════════════════════════════════════════════════*/

void prov_ui_init(const char *ap_ssid, const char *ap_password)
{
    ESP_LOGI(TAG, "Generating QR codes...");

    s_qr_wifi_ready = qr_gen_wifi(ap_ssid, ap_password, s_qr_wifi);
    if (!s_qr_wifi_ready) ESP_LOGW(TAG, "WIFI QR generation failed");

    char url[32];
    snprintf(url, sizeof(url), "http://192.168.4.1");
    s_qr_url_ready = qr_gen_url(url, s_qr_url);
    if (!s_qr_url_ready) ESP_LOGW(TAG, "URL QR generation failed");

    s_qr_current    = QR_SHOW_WIFI;
    s_cycling_active = false;
    s_last_switch    = xTaskGetTickCount();
}

void prov_ui_on_state_change(wifi_prov_state_t state, void *ctx)
{
    (void)ctx;
    s_prov_state = state;

    switch (state) {
    case WIFI_PROV_STATE_CONNECTING_SAVED:
        set_status_bar("Connecting...");
        break;

    case WIFI_PROV_STATE_AP_STARTED:
        s_qr_current    = QR_SHOW_WIFI;
        s_cycling_active = true;
        s_last_switch    = xTaskGetTickCount();
        show_ap_qr_screen(s_qr_current);
        break;

    case WIFI_PROV_STATE_CLIENT_CONNECTED:
        s_cycling_active = false;
        s_qr_current = QR_SHOW_URL;
        show_ap_qr_screen(s_qr_current);
        //no need for status bar to be displayed because the QR and text already change
        break;

    case WIFI_PROV_STATE_CLIENT_GONE:
        s_cycling_active = true;
        s_last_switch    = xTaskGetTickCount();
        break;

    case WIFI_PROV_STATE_CREDS_RECEIVED:
        s_cycling_active = false;
        show_creds_received_screen();
        break;

    case WIFI_PROV_STATE_STA_CONNECTING:
        s_cycling_active = false;
        show_connecting_screen(s_connected_ssid);
        break;

    case WIFI_PROV_STATE_STA_CONNECTED:
        s_cycling_active = false;
        /* show_success_screen called by on_connected with correct ssid/ip */
        break;

    case WIFI_PROV_STATE_STA_FAILED:
        s_cycling_active = true;
        s_last_switch    = xTaskGetTickCount();
        show_failed_screen();
        break;

    case WIFI_PROV_STATE_ERROR:
        s_cycling_active = false;
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
    strncpy(s_connected_ssid, ssid ? ssid : "", sizeof(s_connected_ssid) - 1);
    strncpy(s_connected_ip,   ip   ? ip   : "", sizeof(s_connected_ip)   - 1);
    show_success_screen(s_connected_ssid, s_connected_ip);
}

void prov_ui_tick(void)
{
    if (!s_cycling_active) return;

    TickType_t now     = xTaskGetTickCount();
    TickType_t elapsed = (now - s_last_switch) * portTICK_PERIOD_MS;

    if (elapsed < QR_CYCLE_MS) return;

    //s_qr_current = (s_qr_current == QR_SHOW_WIFI) ? QR_SHOW_URL : QR_SHOW_WIFI;
    s_last_switch = now;

    ESP_LOGD(TAG, "QR cycle → %s", s_qr_current == QR_SHOW_WIFI ? "WIFI" : "URL");

    //render_qr_left(s_qr_current);
    //if (s_qr_current == QR_SHOW_WIFI) render_wifi_labels();
    //else                               render_url_labels();
    /*Status bar content unchanged — dirty flag is set by the draw calls above */
    //display_flush();
}
