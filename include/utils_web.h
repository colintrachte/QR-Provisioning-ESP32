#pragma once

#include "cJSON.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include "esp_err.h"

esp_err_t web_serve_file(httpd_req_t *req, const char *path,
                         const char *mime, bool is_asset);

void web_url_decode(const char *src, char *dst, int dst_size);

void web_set_standard_headers(httpd_req_t *req,
                              const char *mime,
                              bool is_asset);

esp_err_t web_send_404(httpd_req_t *req);
esp_err_t web_send_500(httpd_req_t *req);

bool web_get_query_param(httpd_req_t *req,
                         const char *key,
                         char *out,
                         size_t out_size);

esp_err_t web_send_text(httpd_req_t *req, const char *text);

/**
 * web_send_json — serialise obj, send with JSON headers, free obj.
 *
 * Sets Content-Type: application/json, Cache-Control: no-cache,
 * Access-Control-Allow-Origin: *. Frees obj even on failure (sends 500).
 * Pass NULL to get a 500 response with no body.
 */
esp_err_t web_send_json(httpd_req_t *req, cJSON *obj);

/**
 * web_send_error — send HTTP 400 with { "error": "<msg>" }.
 *
 * Convenience wrapper around web_send_json for error responses.
 */
esp_err_t web_send_error(httpd_req_t *req, const char *msg);
