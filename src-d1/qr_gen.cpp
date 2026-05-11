
#include "qr_gen.h"
#include <qrcodegen.hpp>
#include <stdio.h>
#include <string.h>

qrcodegen::QrCode qr_gen_wifi(const char *ssid, const char *password)
{
    char uri[128];
    if (password && password[0] != '\0') {
        snprintf(uri, sizeof(uri), "WIFI:S:%s;T:WPA;P:%s;;", ssid, password);
    } else {
        snprintf(uri, sizeof(uri), "WIFI:S:%s;T:nopass;;", ssid);
    }
    return qrcodegen::QrCode::encodeText(uri, qrcodegen::QrCode::Ecc::LOW);
}

qrcodegen::QrCode qr_gen_url(const char *url)
{
    return qrcodegen::QrCode::encodeText(url, qrcodegen::QrCode::Ecc::MEDIUM);
}

int qr_pixel_size(const qrcodegen::QrCode &qr, int scale)
{
    return qr.getSize() * scale;
}
