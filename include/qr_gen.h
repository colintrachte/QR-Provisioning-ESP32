#pragma once
/**
 * qr_gen.h — QR code generation helpers for WiFi provisioning.
 *
 * Thin wrappers around the nayuki qrcodegen library for the two QR types
 * used during provisioning:
 *
 *   WiFi join QR  — WIFI: URI scheme.  Phone cameras recognise it and offer
 *                   to join the SoftAP without the user typing credentials.
 *                   Format: WIFI:S:<ssid>;T:WPA;P:<password>;;
 *                           WIFI:S:<ssid>;T:nopass;;  (open network)
 *
 *   Portal URL QR — Plain URL pointing at the captive portal.  Fallback for
 *                   phones where the OS captive-portal redirect did not fire.
 *                   Format: http://192.168.4.1
 *
 * Thread safety: these functions share a static temporary buffer and are NOT
 * re-entrant.  Call them from a single task (typically before WiFi starts).
 */

#include <stdint.h>
#include <stdbool.h>

/** Size of each QR output buffer — qrcodegen_BUFFER_LEN_MAX for v40 QR. */
#define QR_BUF_LEN 3918

/**
 * Encode a WiFi join URI as a QR code.
 *
 * @param ssid      SoftAP SSID (max 32 chars).
 * @param password  Password (8–63 chars for WPA2; NULL or "" = open network).
 * @param out_buf   Caller-supplied buffer of at least QR_BUF_LEN bytes.
 * @return true on success; false if the URI was too long or encoding failed.
 */
bool qr_gen_wifi(const char *ssid, const char *password, uint8_t *out_buf);

/**
 * Encode a URL as a QR code.
 *
 * @param url      Null-terminated URL (e.g. "http://192.168.4.1").
 * @param out_buf  Caller-supplied buffer of at least QR_BUF_LEN bytes.
 * @return true on success.
 */
bool qr_gen_url(const char *url, uint8_t *out_buf);

/**
 * Calculate the pixel dimensions of a rendered QR code.
 *
 * Returns the width (== height) in pixels that display_draw_qr() will
 * occupy for the given scale, including the quiet zone.
 * Returns 0 if qrcode is invalid.
 *
 * @param qrcode  Buffer produced by qr_gen_wifi() or qr_gen_url().
 * @param scale   Pixels per module (pass the same value as display_draw_qr).
 */
int qr_pixel_size(const uint8_t *qrcode, int scale);
