/**
 * qr_gen.c — QR code generation wrappers for provisioning.
 */

#include "qr_gen.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "qr_gen";

/* Temporary buffer required by qrcodegen_encodeText alongside the output. */
static uint8_t s_temp[QR_BUF_LEN];

bool qr_gen_wifi(const char *ssid, const char *password, uint8_t *out_buf)
{
    char uri[128];
    if (password && password[0] != '\0') {
        snprintf(uri, sizeof(uri), "WIFI:S:%s;T:WPA;P:%s;;", ssid, password);
    } else {
        snprintf(uri, sizeof(uri), "WIFI:S:%s;T:nopass;;", ssid);
    }

    ESP_LOGI(TAG, "Encoding WIFI QR: %s", uri);

    bool ok = qrcodegen_encodeText(
        uri,
        s_temp,
        out_buf,
        qrcodegen_Ecc_LOW,       /* lowest ECC → smallest QR for small screen */
        qrcodegen_VERSION_MIN,
        qrcodegen_VERSION_MAX,
        qrcodegen_Mask_AUTO,
        true                     /* boostEcl — allow upgrading if space permits */
    );

    if (!ok) {
        ESP_LOGE(TAG, "qr_gen_wifi: encoding failed (string too long?)");
    } else {
        ESP_LOGI(TAG, "WIFI QR size: %d modules", qrcodegen_getSize(out_buf));
    }
    return ok;
}

bool qr_gen_url(const char *url, uint8_t *out_buf)
{
    ESP_LOGI(TAG, "Encoding URL QR: %s", url);

    bool ok = qrcodegen_encodeText(
        url,
        s_temp,
        out_buf,
        qrcodegen_Ecc_MEDIUM,    /* medium ECC — URL is short, can afford it */
        qrcodegen_VERSION_MIN,
        qrcodegen_VERSION_MAX,
        qrcodegen_Mask_AUTO,
        true
    );

    if (!ok) {
        ESP_LOGE(TAG, "qr_gen_url: encoding failed");
    } else {
        ESP_LOGI(TAG, "URL QR size: %d modules", qrcodegen_getSize(out_buf));
    }
    return ok;
}

int qr_pixel_size(const uint8_t *qrcode, int scale)
{
    int size = qrcodegen_getSize(qrcode);
    if (size <= 0) return 0;
    int quiet = scale * 2;
    return size * scale + quiet * 2;
}
