#include "web_utils.h"
#include "esp_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
web_utils should own:
HTTP response patterns
file serving
query parsing
small utilities
It should NOT own:
complex JSON formatting → delegate to cJSON
*/

static const char *TAG = "web_utils";
#define SERVE_CHUNK 8192

//small response helper
esp_err_t web_send_text(httpd_req_t *req, const char *text)
{
    web_set_standard_headers(req, "text/plain", false);
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
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
    char gz_path[128];
    bool use_gz = false;
    FILE *f = NULL;

    // Check for Gzip variant
    if (strlen(path) < sizeof(gz_path) - 4)
    {
        snprintf(gz_path, sizeof(gz_path), "%s.gz", path);
        f = fopen(gz_path, "rb");
        if (f) use_gz = true;
    }

    if (!f)
    {
        f = fopen(path, "rb");
    }

    if (!f)
    {
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
    if (file_size > 256 * 1024) {   // pick a sane upper bound
        ESP_LOGE(TAG, "File too large to serve: %s (%ld B)", path, file_size);
        fclose(f);
        return httpd_resp_send_500(req);
    }
    rewind(f);

    web_set_standard_headers(req, mime, is_asset);
    if (use_gz)
    {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    // Optimization: Single send for small files (avoids chunking overhead)
    // Most your assets are <8KB gzipped[cite: 3]
    if (file_size <= SERVE_CHUNK)
    {
        char *buf = malloc(file_size);
        if (buf)
        {
            size_t r = fread(buf, 1, file_size, f);
            if (r != file_size) {
                free(buf);
                fclose(f);
                return httpd_resp_send_500(req);
            }
            httpd_resp_send(req, buf, file_size);
            free(buf);
        }
        else
        {
            fclose(f);
            return httpd_resp_send_500(req);
        }
    }
    else
    {
        // Chunked transfer for larger files
        // NOTE: No Content-Length set here to avoid HTTP/1.1 conflicts[cite: 3]
        char *buf = malloc(SERVE_CHUNK);
        if (!buf)
        {
            fclose(f);
            return httpd_resp_send_500(req);
        }

        size_t n;
        while ((n = fread(buf, 1, SERVE_CHUNK, f)) > 0)
        {
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
    while (*src && out < dst_size - 1)
    {
        if (src[0] == '%' && src[1] && src[2])
        {
            char hex[3] = { src[1], src[2], '\0' };
            dst[out++] = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else if (src[0] == '+')
        {
            dst[out++] = ' ';
            src++;
        }
        else
        {
            dst[out++] = *src++;
        }
    }
    dst[out] = '\0';
}
