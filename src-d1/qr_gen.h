/**
 * qr_gen.h — QR code generation helpers for WiFi provisioning.
 *
 * Pure C interface so it can be called from C++ Arduino code.
 * Wraps Nayuki's qrcodegen library.
 */

#pragma once
#include <Arduino.h>
#include <qrcodegen.hpp>

int qr_pixel_size(const qrcodegen::QrCode &qr, int scale);

qrcodegen::QrCode qr_gen_wifi(const char *ssid, const char *password);
qrcodegen::QrCode qr_gen_url(const char *url);
