#pragma once

#include "esp_http_server.h"
#include <stdbool.h>

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
