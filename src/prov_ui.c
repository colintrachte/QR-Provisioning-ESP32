/**
 * prov_ui.c — OLED display state machine for the WiFi provisioning flow.
 *
 * Screen layout (128 × 64 px, SSD1306)
 * ─────────────────────────────────────
 *   Left  68 px  QR code (scale 2, v1 QR = 21 modules → 42 px + 4 px quiet)
 *   Right 58 px  Label text  (x = 70 … 127)
 *   y = 55       Separator line
 *   y = 56-63    Status bar
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
static bool    s_qr_wifi_ok = false;
static bool    s_qr_url_ok  = false;

/* ── Display state ──────────────────────────────────────────────────────*/
typedef enum { QR_WIFI = 0, QR_URL = 1 } qr_slot_t;

static qr_slot_t s_qr_shown = QR_WIFI;

/* ── Stored connection info (written by on_connected callback) ──────────*/
static char s_ssid[33] = {0};
static char s_ip[16]   = {0};

/* ═══════════════════════════════════════════════════════════════════════
 *  INTERNAL SCREEN RENDERERS
 *  Each function owns one complete screen: clear → draw → flush.
 *  Nothing outside these functions calls display_flush() directly.
 * ═══════════════════════════════════════════════════════════════════════*/

static void render_qr_screen(qr_slot_t slot)
{
    static const char *line1[] = {"Scan to", "Scan to"};
    static const char *line2[] = {"join AP", "open pg"};

    uint8_t *qr    = (slot == QR_WIFI) ? s_qr_wifi    : s_qr_url;
    bool     ready = (slot == QR_WIFI) ? s_qr_wifi_ok : s_qr_url_ok;

    display_clear();
    if (ready) {
        display_draw_qr(0, 0, 2, qr);
    } else {
        display_draw_text(2, 20, "QR N/A", DISP_FONT_SMALL);
    }
    display_draw_text(70,  0, line1[slot], DISP_FONT_NORMAL);
    display_draw_text(70, 12, line2[slot], DISP_FONT_NORMAL);
    display_flush();

    s_qr_shown = slot;
}

static void render_status_bar(const char *msg)
{
    display_clear_region(0, 54, DISP_WIDTH, 10);
    display_draw_hline(0, 55, DISP_WIDTH);
    display_draw_text(2, 57, msg, DISP_FONT_SMALL);
    display_flush();
}

static void render_connecting(const char *ssid)
{
    char trunc[22];
    snprintf(trunc, sizeof(trunc), "%.21s", ssid[0] ? ssid : "...");

    display_clear();
    display_draw_text(0,  0, "Connecting to:", DISP_FONT_NORMAL);
    display_draw_hline(0, 12, DISP_WIDTH);
    display_draw_text(0, 15, trunc,            DISP_FONT_MEDIUM);
    display_draw_text(0, 36, "Please wait...", DISP_FONT_NORMAL);
    display_flush();
}

static void render_connected(const char *ssid, const char *ip)
{
    char trunc[22];
    snprintf(trunc, sizeof(trunc), "%.21s", ssid);

    display_clear();
    display_draw_text(0,  0, ip,          DISP_FONT_BOLD);
    display_draw_hline(0, 13, DISP_WIDTH);
    display_draw_text( 0, 16, "Connected to:", DISP_FONT_NORMAL);
    display_draw_text( 0, 27, trunc,           DISP_FONT_MEDIUM);
    display_flush();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════*/

void prov_ui_init(const char *ap_ssid, const char *ap_password)
{
    ESP_LOGI(TAG, "Generating QR codes");
    s_qr_wifi_ok = qr_gen_wifi(ap_ssid, ap_password, s_qr_wifi);
    s_qr_url_ok  = qr_gen_url("http://" WIFI_MANAGER_AP_IP, s_qr_url);
    if (!s_qr_wifi_ok) ESP_LOGW(TAG, "WiFi QR generation failed");
    if (!s_qr_url_ok)  ESP_LOGW(TAG, "URL QR generation failed");
}

void prov_ui_show_boot(void)
{
    display_clear();
    display_draw_text(14,  0, "WiFi Manager",      DISP_FONT_BOLD);
    display_draw_hline(0, 15, DISP_WIDTH);
    display_draw_text( 4, 18, "WiFi Provisioning", DISP_FONT_NORMAL);
    display_draw_text(22, 32, "Starting...",        DISP_FONT_NORMAL);
    display_draw_hline(0, 53, DISP_WIDTH);
    display_draw_text( 2, 55, "Heltec V3  ESP32-S3", DISP_FONT_SMALL);
    display_flush();
    ESP_LOGI(TAG, "Boot screen shown");
}

void prov_ui_on_state_change(wifi_manager_state_t state, void *ctx)
{
    (void)ctx;
    switch (state) {

    case WIFI_MANAGER_STATE_CONNECTING_SAVED:
        render_status_bar("Connecting...");
        break;

    case WIFI_MANAGER_STATE_AP_STARTED:
        render_qr_screen(QR_WIFI);
        break;

    case WIFI_MANAGER_STATE_CLIENT_CONNECTED:
        /* Client joined — stop cycling, show URL QR so they can open portal. */
        render_qr_screen(QR_URL);
        break;

    case WIFI_MANAGER_STATE_CLIENT_GONE:
        /* Client left — resume cycling so next person can join. */
        render_qr_screen(QR_WIFI);
        break;

    case WIFI_MANAGER_STATE_CREDS_RECEIVED:
        render_status_bar("Credentials saved");
        break;

    case WIFI_MANAGER_STATE_STA_CONNECTING:
        render_connecting(s_ssid[0] ? s_ssid : "...");
        break;

    case WIFI_MANAGER_STATE_STA_CONNECTED:
        /* on_connected fires right after with ssid/ip — handled there. */
        break;

    case WIFI_MANAGER_STATE_STA_FAILED:
        /* Resume portal — client can try a different password. */
        render_status_bar("Failed - check password");
        break;

    case WIFI_MANAGER_STATE_ERROR:
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
    render_connected(s_ssid, s_ip);
    ESP_LOGI(TAG, "Connected: SSID=%s  IP=%s", s_ssid, s_ip);
}

void prov_ui_tick(void)
{
    /* Intentional no-op.
     * QR transitions are driven by wifi_manager state changes only — never
     * by a timer.  The display is only redrawn when something meaningful
     * changes (client connects, credentials received, etc.).  Callers are
     * still expected to call this from their main loop so the API contract
     * is stable if cycling is ever needed for a specific deployment. */
}
