#include "utils_web.h"
#include "esp_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

/*
 * utils_web.c — HTTP response helpers and file serving.
 *
 * Owns:
 *   - Standard response headers
 *   - File serving with optional gzip (Accept-Encoding-aware)
 *   - URL decode
 *   - Small text response helper
 *   - Shared JSON response helpers (web_send_json, web_send_error)
 *     used by settings_server.c and any future JSON endpoint, so the
 *     Content-Type / Cache-Control / CORS header set is defined once.
 *
 * Does NOT own:
 *   - JSON formatting → use cJSON via utils_filesystem helpers
 *   - Filesystem mounting → utils_filesystem.c
 *
 * ── gzip and Android ────────────────────────────────────────────────────────
 * Android's captive-portal WebView (CaptivePortalLogin APK) and some mobile
 * browsers send requests WITHOUT "Accept-Encoding: gzip", or with a
 * restricted accept list. Serving a gzip body to such a client produces
 * garbled binary output. Desktop Chrome always advertises gzip support, which
 * is why the issue only appears on Android.
 *
 * Fix: check the Accept-Encoding request header before preferring the .gz
 * file. If the client did not advertise gzip support, fall through to the
 * plain file even when a .gz sibling exists.
 */

static const char *TAG = "web_utils";
#define SERVE_CHUNK 8192

/* ── Internal: Accept-Encoding check ───────────────────────────────────── */

/**
 * Returns true if the request's Accept-Encoding header contains "gzip".
 * Falls back to false on any error (header absent, buffer too small, etc.)
 * so we always degrade gracefully to the uncompressed file.
 */
static bool client_accepts_gzip(httpd_req_t *req)
{
    /* 128 bytes is enough for any realistic Accept-Encoding value.
     * "gzip, deflate, br, zstd" is ~24 chars; even padded with extras it fits. */
    char ae[128] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Accept-Encoding",
                                                 ae, sizeof(ae));
    if (err != ESP_OK) return false;          /* header absent → no gzip */
    return (strstr(ae, "gzip") != NULL);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t web_send_text(httpd_req_t *req, const char *text)
{
    web_set_standard_headers(req, "text/plain", false);
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

/**
 * web_send_json — serialise a cJSON object, send it, then free it.
 *
 * Sets Content-Type: application/json, Cache-Control: no-cache, and
 * Access-Control-Allow-Origin: * on every response. Frees obj even on
 * serialisation failure (sends 500 instead). This is the single canonical
 * JSON response helper shared by all HTTP endpoint modules.
 */
esp_err_t web_send_json(httpd_req_t *req, cJSON *obj)
{
    if (!obj) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    char *body = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

/**
 * web_send_error — send a 400 Bad Request with { "error": "<msg>" }.
 */
esp_err_t web_send_error(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *obj = cJSON_CreateObject();
    if (obj) cJSON_AddStringToObject(obj, "error", msg ? msg : "unknown error");
    return web_send_json(req, obj);
}

void web_set_standard_headers(httpd_req_t *req, const char *mime, bool is_asset)
{
    httpd_resp_set_type(req, mime);

    httpd_resp_set_hdr(req,
        "Cache-Control",
        is_asset ? "public, max-age=3600" : "no-cache");

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

esp_err_t web_serve_file(httpd_req_t *req, const char *path,
                         const char *mime, bool is_asset)
{
    char  gz_path[128];
    bool  use_gz = false;
    FILE *f      = NULL;

    /* Only attempt the .gz variant when the client declared gzip support.
     * This is the fix for Android WebView / captive-portal browser sending
     * requests without Accept-Encoding: gzip. */
    if (client_accepts_gzip(req) && strlen(path) < sizeof(gz_path) - 4) {
        snprintf(gz_path, sizeof(gz_path), "%s.gz", path);
        f = fopen(gz_path, "rb");
        if (f) use_gz = true;
    }
    ESP_LOGI(TAG, "Serving %s (%s)", path, use_gz ? "gz" : "plain");
    if (!f) {
        f = fopen(path, "rb");
    }

    if (!f) {
        ESP_LOGE(TAG, "File not found: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size < 0) {
        fclose(f);
        return httpd_resp_send_500(req);
    }
    if (file_size > 256 * 1024) {
        ESP_LOGE(TAG, "File too large to serve: %s (%ld B)", path, file_size);
        fclose(f);
        return httpd_resp_send_500(req);
    }
    rewind(f);

    web_set_standard_headers(req, mime, is_asset);
    if (use_gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    /* Single send for small files — avoids chunked-transfer overhead.
     * Most assets are <8 KB gzipped. */
    if (file_size <= SERVE_CHUNK) {
        char *buf = malloc((size_t)file_size);
        if (!buf) {
            fclose(f);
            return httpd_resp_send_500(req);
        }
        size_t r = fread(buf, 1, (size_t)file_size, f);
        if (r != (size_t)file_size) {
            free(buf);
            fclose(f);
            return httpd_resp_send_500(req);
        }
        httpd_resp_send(req, buf, (ssize_t)file_size);
        free(buf);
    } else {
        /* Chunked transfer for larger files.
         * No Content-Length header set — avoids HTTP/1.1 conflicts with
         * chunked encoding. */
        char *buf = malloc(SERVE_CHUNK);
        if (!buf) {
            fclose(f);
            return httpd_resp_send_500(req);
        }
        size_t n;
        while ((n = fread(buf, 1, SERVE_CHUNK, f)) > 0) {
            httpd_resp_send_chunk(req, buf, (ssize_t)n);
        }
        httpd_resp_send_chunk(req, NULL, 0);
        free(buf);
    }

    fclose(f);
    return ESP_OK;
}

void web_url_decode(const char *src, char *dst, int dst_size)
{
    int out = 0;
    while (*src && out < dst_size - 1) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[out++]  = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (src[0] == '+') {
            dst[out++] = ' ';
            src++;
        } else {
            dst[out++] = *src++;
        }
    }
    dst[out] = '\0';
}
