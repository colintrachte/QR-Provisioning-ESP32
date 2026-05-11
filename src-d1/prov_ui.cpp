// prov_ui.cpp
#include "prov_ui.h"
#include "display.h"
#include "qr_gen.h"

static qrcodegen::QrCode s_qr_wifi(qrcodegen::QrCode::encodeText("", qrcodegen::QrCode::Ecc::LOW));
static qrcodegen::QrCode s_qr_setup(qrcodegen::QrCode::encodeText("", qrcodegen::QrCode::Ecc::LOW));
static qrcodegen::QrCode s_qr_index(qrcodegen::QrCode::encodeText("", qrcodegen::QrCode::Ecc::LOW));
static bool s_qr_wifi_ok  = false;
static bool s_qr_setup_ok = false;
static bool s_qr_index_ok = false;
static int s_qr_index = 0;

static char s_ssid[33] = {0};
static char s_ip[16] = {0};
static volatile int s_ws_clients = 0;

void prov_ui_init(const char *ap_ssid, const char *ap_password) {
    if (!display_is_available()) return;
    s_qr_wifi  = qr_gen_wifi(ap_ssid, ap_password);
    s_qr_setup = qr_gen_url("http://192.168.4.1/");
    s_qr_wifi_ok  = (s_qr_wifi.getSize() > 0);
    s_qr_setup_ok = (s_qr_setup.getSize() > 0);
}

void prov_ui_show_boot(void) {
    if (!display_is_available()) return;
    display_clear();
    display_draw_text(0, 10, "RC-Tank booting...");
    display_flush();
}

void prov_ui_on_state_change(int state, void *ctx) {
    (void)ctx;
    if (!display_is_available()) return;

    display_clear();
    switch (state) {
        case 1: /* CONNECTING_SAVED */
            display_draw_text(0, 10, "Trying saved WiFi...");
            break;
        case 2: /* AP_STARTED */
            display_draw_text(0, 10, "Setup mode:");
            display_draw_text(0, 22, "Scan QR to join AP");
            if (s_qr_wifi_ok) display_draw_qr(34, 0, 2, s_qr_wifi);
            break;
        case 6: /* STA_CONNECTING */
            display_draw_text(0, 10, "Connecting...");
            display_draw_text(0, 22, s_ssid);
            break;
        case 7: /* STA_CONNECTED */
            display_draw_text(0, 10, "Connected!");
            display_draw_text(0, 22, s_ip);
            if (s_qr_index_ok) display_draw_qr(34, 0, 2, s_qr_index);
            break;
        case 8: /* STA_FAILED */
            display_draw_text(0, 10, "WiFi failed");
            display_draw_text(0, 22, "Restarting portal...");
            break;
    }
    display_flush();
}

void prov_ui_on_connected(const char *ssid, const char *ip, void *ctx) {
    (void)ctx;
    strlcpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid));
    strlcpy(s_ip, ip ? ip : "", sizeof(s_ip));

    char url[48];
    snprintf(url, sizeof(url), "http://%s/", s_ip);
    s_qr_index = qr_gen_url(url);         // ✅
    s_qr_index_ok = (s_qr_index.getSize() > 0);

    prov_ui_on_state_change(7, ctx);
}

void prov_ui_set_client_count(int count) {
    s_ws_clients = count;
    if (s_ip[0] && display_is_available()) {
        display_clear();
        if (count > 0) {
            display_draw_text(0, 10, "Client connected");
            display_draw_text(0, 22, s_ip);
        } else {
            display_draw_text(0, 10, "Connected:");
            display_draw_text(0, 22, s_ip);
            if (s_qr_index_ok) display_draw_qr(34, 0, 2, s_qr_index);
        }
        display_flush();
    }
}

void prov_ui_tick(void) {
    /* Event-driven — nothing needed here */
}
