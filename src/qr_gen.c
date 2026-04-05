/**
 * qr_gen.c — QR code generation helpers for WiFi provisioning.
 */

#include "qr_gen.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "qr_gen";

/*
 * qrcodegen_encodeText() requires a temporary buffer the same size as the
 * output buffer.  We allocate it statically — these functions are called
 * once at boot from a single task, so re-entrancy is not a concern.
 */
static uint8_t s_temp[QR_BUF_LEN];

bool qr_gen_wifi(const char *ssid, const char *password, uint8_t *out_buf)
{
    char uri[128];
    if (password && password[0] != '\0') {
        snprintf(uri, sizeof(uri), "WIFI:S:%s;T:WPA;P:%s;;", ssid, password);
    } else {
        snprintf(uri, sizeof(uri), "WIFI:S:%s;T:nopass;;", ssid);
    }
    ESP_LOGI(TAG, "Encoding WiFi QR: %s", uri);

    bool ok = qrcodegen_encodeText(
        uri, s_temp, out_buf,
        qrcodegen_Ecc_LOW,       /* lowest ECC → smallest QR for cramped display */
        qrcodegen_VERSION_MIN,
        qrcodegen_VERSION_MAX,
        qrcodegen_Mask_AUTO,
        true);                   /* boostEcl: upgrade ECC if it fits */

    if (ok) {
        ESP_LOGI(TAG, "WiFi QR: %d modules", qrcodegen_getSize(out_buf));
    } else {
        ESP_LOGE(TAG, "WiFi QR encoding failed (SSID/password too long?)");
    }
    return ok;
}

bool qr_gen_url(const char *url, uint8_t *out_buf)
{
    ESP_LOGI(TAG, "Encoding URL QR: %s", url);

    bool ok = qrcodegen_encodeText(
        url, s_temp, out_buf,
        qrcodegen_Ecc_MEDIUM,    /* medium ECC — short URL can afford better recovery */
        qrcodegen_VERSION_MIN,
        qrcodegen_VERSION_MAX,
        qrcodegen_Mask_AUTO,
        true);

    if (ok) {
        ESP_LOGI(TAG, "URL QR: %d modules", qrcodegen_getSize(out_buf));
    } else {
        ESP_LOGE(TAG, "URL QR encoding failed");
    }
    return ok;
}

int qr_pixel_size(const uint8_t *qrcode, int scale)
{
    int modules = qrcodegen_getSize(qrcode);
    if (modules <= 0) return 0;
    int quiet = (scale >= 3) ? 0 : (scale * 2);  /* matches display_draw_qr logic */
    return modules * scale + quiet * 2;
}
