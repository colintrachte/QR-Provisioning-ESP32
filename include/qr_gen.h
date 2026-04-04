#pragma once
/**
 * qr_gen.h — Thin wrappers around nayuki qrcodegen for provisioning use.
 *
 * Two QR types are used during provisioning:
 *
 *  AP_WIFI  — WIFI URI scheme.  Phone camera recognises it and offers to join
 *              the ESP32's SoftAP without the user typing the SSID/password.
 *              Format: WIFI:S:<ssid>;T:WPA;P:<password>;;
 *
 *  PORTAL   — Plain URL pointing at the captive portal.  Fallback for phones
 *              where the captive-portal redirect did not fire automatically.
 *              Format: http://192.168.4.1
 *
 * Buffers are static — callers must not free them.
 * Thread safety: these functions are not re-entrant; call from one task only.
 */

#include <stdint.h>
#include <stdbool.h>

#define QR_BUF_LEN 3918   /* qrcodegen_BUFFER_LEN_MAX */

/**
 * Build the WIFI: URI and encode it as a QR code.
 *
 * @param ssid      AP SSID (max 32 chars)
 * @param password  AP password (8-63 chars for WPA2; empty string = open)
 * @param out_buf   Caller-supplied buffer of at least QR_BUF_LEN bytes
 * @return true on success, false if the data was too long or encoding failed
 */
bool qr_gen_wifi(const char *ssid, const char *password, uint8_t *out_buf);

/**
 * Encode a URL as a QR code (used for the portal fallback http://x.x.x.x).
 *
 * @param url      Null-terminated URL string
 * @param out_buf  Caller-supplied buffer of at least QR_BUF_LEN bytes
 * @return true on success
 */
bool qr_gen_url(const char *url, uint8_t *out_buf);

/**
 * Return the pixel dimensions (width == height) that display_draw_qr will
 * occupy for the given qrcode buffer at the given scale, including quiet zone.
 * Returns 0 if qrcode is invalid.
 */
int qr_pixel_size(const uint8_t *qrcode, int scale);
