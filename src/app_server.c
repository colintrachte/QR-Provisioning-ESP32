/**
 * app_server.c — Robot control HTTP file server and WebSocket dispatcher.
 *
 * File serving
 * ────────────
 * Explicit named routes call serve_file() with a literal path. Dynamic
 * URI→path concatenation is avoided because httpd URIs can be up to
 * CONFIG_HTTPD_MAX_URI_LEN (512 B) and would overflow any fixed stack buffer
 * when prepending "/littlefs". A wildcard catch-all returns 404 for
 * any URI not in the route table. Requires uri_match_fn = wildcard.
 */

#include "app_server.h"
#include "ctrl_drive.h"
#include "o_led.h"
#include "health_monitor.h"
#include "prov_ui.h"
#include "ota_server.h"
#include "vfs_mount.h"
#include "config.h"
#include <dirent.h>
#include <sys/stat.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_server";

#define FS_BASE     "/littlefs"
#define SERVE_CHUNK  4096

/* serve_file — gzip-transparent static file server.
 *
 * Gzip strategy (build-time compression, zero runtime CPU):
 *   Before running `pio run -t uploadfs`, compress each web asset:
 *     gzip -9 index.js index.css base.css index.html settings.{js,css,html}
 *           setup.{js,css,html} favicon.svg
 *   This produces index.js.gz, index.css.gz, etc. alongside the originals.
 *   Upload ALL files (both .gz and originals) to LittleFS.
 *
 *   On each request we first try to open path.gz. If it exists we serve it
 *   with Content-Encoding: gzip — the browser decompresses transparently.
 *   If it doesn't exist we serve the plain file unchanged.
 *
 *   This means:
 *   - No runtime decompression on the ESP32 (zero CPU cost)
 *   - No routing changes — URIs stay the same (/index.js, not /index.js.gz)
 *   - No change to OTA filesystem update — just include .gz files in the image
 *   - Falls back gracefully if .gz files are absent (e.g. first upload)
 *   - Browsers that don't support gzip (essentially none in 2024) get plain
 *
 * Content-Length: set from fseek/ftell (O(1) on LittleFS) so the browser
 * can reuse the TCP connection rather than waiting for server-close.
 *
 * is_asset: true → CSS/JS/SVG get Cache-Control: public, max-age=3600
 *           false → HTML gets no-cache so provisioning state is always fresh
 */
static esp_err_t serve_file(httpd_req_t *req, const char *path,
                             const char *mime, bool is_asset)
{
    /* Try gzip variant first — path is at most MAXPATH chars, +3 for ".gz" */
    char gz_path[128];
    bool use_gz = false;
    FILE *f = NULL;

    if (strlen(path) < sizeof(gz_path) - 4) {
        snprintf(gz_path, sizeof(gz_path), "%s.gz", path);
        f = fopen(gz_path, "rb");
        if (f) use_gz = true;
    }

    if (!f) {
        f = fopen(path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "serve: not found: %s", path);
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
            return ESP_OK;
        }
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Cache-Control",
                       is_asset ? "public, max-age=3600" : "no-cache");

    if (use_gz)
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    if (file_size > 0) {
        char len_str[16];
        snprintf(len_str, sizeof(len_str), "%ld", file_size);
        httpd_resp_set_hdr(req, "Content-Length", len_str);
    }

    char *buf = malloc(SERVE_CHUNK);
    if (!buf) { fclose(f); httpd_resp_send_500(req); return ESP_OK; }

    size_t n;
    while ((n = fread(buf, 1, SERVE_CHUNK, f)) > 0)
        httpd_resp_send_chunk(req, buf, (ssize_t)n);
    httpd_resp_send_chunk(req, NULL, 0);

    free(buf);
    fclose(f);

    ESP_LOGI(TAG, "serve %s [%s, %ld B]", path, use_gz ? "gz" : "plain", file_size);
    return ESP_OK;
}

/* ── File route handlers ───────────────────────────────────────────────────*/
static esp_err_t handle_index(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/index.html",    "text/html",             false); }

static esp_err_t handle_base_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/base.css",      "text/css",              true); }

static esp_err_t handle_index_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/index.css",     "text/css",              true); }

static esp_err_t handle_index_js(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/index.js",      "application/javascript", true); }

static esp_err_t handle_settings(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/settings.html", "text/html",             false); }

static esp_err_t handle_settings_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/settings.css",  "text/css",              true); }

static esp_err_t handle_settings_js(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/settings.js",   "application/javascript", true); }

static esp_err_t handle_favicon(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/favicon.svg",   "image/svg+xml",         true); }

static esp_err_t handle_404(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

/* ── Settings API endpoints ────────────────────────────────────────────────*/
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

#define SETTINGS_NVS_NAMESPACE  "wifi_mgr"
#define SETTINGS_NVS_KEY_SSID   "ssid"
#define SETTINGS_NVS_KEY_PASS   "pass"

#define MAX_SCAN_APS  20

typedef struct {
    char ssid[33];
    int  rssi;
    int  quality;
    bool secure;
} scan_ap_t;

static SemaphoreHandle_t s_scan_mutex   = NULL;
static scan_ap_t         s_scan_aps[MAX_SCAN_APS];
static int               s_scan_count  = 0;
static bool              s_scan_ready  = false;
static volatile bool     s_scan_running = false;

static void scan_task(void *arg)
{
    wifi_scan_config_t cfg = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    s_scan_running = true;

    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
        uint16_t count = MAX_SCAN_APS;
        wifi_ap_record_t *recs = calloc(MAX_SCAN_APS, sizeof(wifi_ap_record_t));
        if (recs) {
            esp_wifi_scan_get_ap_records(&count, recs);

            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            s_scan_count = 0;
            for (int i = 0; i < (int)count && s_scan_count < MAX_SCAN_APS; i++) {
                if (recs[i].ssid[0] == '\0') continue;
                int si = 0;
                for (int j = 0; recs[i].ssid[j] && j < 32; j++) {
                    char c = (char)recs[i].ssid[j];
                    if ((c == '"' || c == '\\') && si < 31)
                        s_scan_aps[s_scan_count].ssid[si++] = '\\';
                    if (si < 32) s_scan_aps[s_scan_count].ssid[si++] = c;
                }
                s_scan_aps[s_scan_count].ssid[si] = '\0';
                s_scan_aps[s_scan_count].rssi    = recs[i].rssi;
                s_scan_aps[s_scan_count].quality =
                    recs[i].rssi <= -100 ? 0 : recs[i].rssi >= -40 ? 100 :
                    2 * (recs[i].rssi + 100);
                s_scan_aps[s_scan_count].secure  =
                    (recs[i].authmode != WIFI_AUTH_OPEN);
                s_scan_count++;
            }
            s_scan_ready = true;
            xSemaphoreGive(s_scan_mutex);
            free(recs);
            ESP_LOGI(TAG, "Scan complete: %d APs", s_scan_count);
        }
    }
    s_scan_running = false;
    vTaskDelete(NULL);
}

static void trigger_scan(void)
{
    if (!s_scan_running)
        xTaskCreate(scan_task, "settings_scan", 4096, NULL, 3, NULL);
}

/* GET /api/status */
static esp_err_t handle_api_status(httpd_req_t *req)
{
    wifi_ap_record_t ap  = {0};
    bool connected       = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    char ip[16]          = {0};
    char ssid[33]        = {0};

    if (connected) {
        esp_netif_ip_info_t info = {0};
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) esp_netif_get_ip_info(netif, &info);
        snprintf(ip,   sizeof(ip),   IPSTR, IP2STR(&info.ip));
        snprintf(ssid, sizeof(ssid), "%s",  (char *)ap.ssid);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
        connected ? "true" : "false", ssid, ip,
        connected ? (int)ap.rssi : 0);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* GET /api/scan   -  ?refresh=1 triggers a new scan */
static esp_err_t handle_api_scan(httpd_req_t *req)
{
    char query[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        strstr(query, "refresh=1"))
        trigger_scan();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (!s_scan_ready || s_scan_running) {
        httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
        return ESP_OK;
    }

    char *buf = malloc(MAX_SCAN_APS * 100 + 8);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }

    int pos = 0;
    buf[pos++] = '[';
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    for (int i = 0; i < s_scan_count; i++) {
        pos += snprintf(buf + pos, MAX_SCAN_APS * 100 - pos,
            "%s{\"ssid\":\"%s\",\"quality\":%d,\"secure\":%s}",
            i ? "," : "",
            s_scan_aps[i].ssid,
            s_scan_aps[i].quality,
            s_scan_aps[i].secure ? "true" : "false");
    }
    xSemaphoreGive(s_scan_mutex);
    buf[pos++] = ']';
    buf[pos]   = '\0';

    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

/* POST /api/connect */
static void url_decode(const char *src, char *dst, int dst_size)
{
    int out = 0;
    while (*src && out < dst_size - 1) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[out++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (src[0] == '+') { dst[out++] = ' '; src++; }
        else { dst[out++] = *src++; }
    }
    dst[out] = '\0';
}

static esp_err_t handle_api_connect(httpd_req_t *req)
{
    char body[320] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty"); return ESP_OK; }

    char ssid_enc[128] = {0}, pass_enc[128] = {0};
    char ssid[64] = {0}, pass[64] = {0};

    const char *p;
    if ((p = strstr(body, "ssid=")) != NULL) {
        p += 5;
        const char *e = strchr(p, '&');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len >= (int)sizeof(ssid_enc)) len = sizeof(ssid_enc) - 1;
        memcpy(ssid_enc, p, len);
    }
    if ((p = strstr(body, "pass=")) != NULL) {
        p += 5;
        const char *e = strchr(p, '&');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len >= (int)sizeof(pass_enc)) len = sizeof(pass_enc) - 1;
        memcpy(pass_enc, p, len);
    }
    url_decode(ssid_enc, ssid, sizeof(ssid));
    url_decode(pass_enc, pass, sizeof(pass));

    if (!ssid[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required"); return ESP_OK; }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_str(nvs, SETTINGS_NVS_KEY_SSID, ssid);
        nvs_set_str(nvs, SETTINGS_NVS_KEY_PASS, pass);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Credentials updated via settings page: SSID=%s", ssid);
    } else {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "Saved — rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* POST /api/erase */
static esp_err_t handle_api_erase(httpd_req_t *req)
{
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, SETTINGS_NVS_KEY_SSID);
        nvs_erase_key(nvs, SETTINGS_NVS_KEY_PASS);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_sendstr(req, "Erased — rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* ── WebSocket client tracking ─────────────────────────────────────────────*/
#define MAX_WS_CLIENTS 4

static httpd_handle_t    s_httpd    = NULL;
static int               s_ws_fds[MAX_WS_CLIENTS];
static int               s_ws_count = 0;
static SemaphoreHandle_t s_ws_mutex = NULL;

static void ws_client_add(int fd)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] < 0) { s_ws_fds[i] = fd; s_ws_count++; break; }
    }
    int count = s_ws_count;
    xSemaphoreGive(s_ws_mutex);
    prov_ui_set_client_count(count);
    ESP_LOGI(TAG, "WS client fd=%d connected (total=%d)", fd, count);
}

static void ws_client_remove(int fd)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) { s_ws_fds[i] = -1; s_ws_count--; break; }
    }
    int count = s_ws_count;
    xSemaphoreGive(s_ws_mutex);
    prov_ui_set_client_count(count);
    ctrl_drive_emergency_stop();
    ESP_LOGI(TAG, "WS client fd=%d disconnected (total=%d)", fd, count);
}

/* ── WebSocket protocol ────────────────────────────────────────────────────
 *
 * Two message types share the same handler:
 *
 * TEXT frames  -  infrequent control words (arm, disarm, ping, stop, led:N)
 *   These are sent rarely enough that string parsing cost is irrelevant.
 *   pong response is also TEXT.
 *
 * BINARY frames  -  high-frequency axes packets (every send tick, up to 100 Hz)
 *   Format: 5 bytes, little-endian
 *     [0]     uint8  type = 0x01
 *     [1..2]  int16  x * 10000   (range -10000..10000 = -1.0..1.0, 0.0001 res)
 *     [3..4]  int16  y * 10000
 *   No string allocation, no sscanf  -  just two array reads and two multiplies.
 *   Resolution of 0.0001 is well below the motor driver's meaningful step size.
 */
#define WS_MSG_AXES  0x01

static void dispatch_text(httpd_req_t *req, const char *msg)
{
    if (strcmp(msg, "ping") == 0) {
        ctrl_drive_feed_watchdog();
        httpd_ws_frame_t pong = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)"pong",
            .len     = 4,
        };
        httpd_ws_send_frame(req, &pong);
        return;
    }
    if (strcmp(msg, "arm") == 0) {
        ctrl_drive_feed_watchdog();
        ctrl_drive_set_armed(true);
        o_led_blink(LED_PATTERN_HEARTBEAT);
        return;
    }
    if (strcmp(msg, "disarm") == 0) {
        ctrl_drive_feed_watchdog();
        ctrl_drive_set_armed(false);
        o_led_blink(LED_PATTERN_SLOW_BLINK);
        return;
    }
    if (strcmp(msg, "stop") == 0) {
        ctrl_drive_feed_watchdog();
        ctrl_drive_set_axes(0.0f, 0.0f);
        return;
    }
    if (strncmp(msg, "led:", 4) == 0) {
        ctrl_drive_feed_watchdog();
        o_led_set(strtof(msg + 4, NULL));
        return;
    }
    ESP_LOGD(TAG, "Unknown WS text: %s", msg);
}

static void dispatch_binary(const uint8_t *data, size_t len)
{
    if (len < 5 || data[0] != WS_MSG_AXES) return;

    /* Read two little-endian int16 values */
    int16_t ix = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
    int16_t iy = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));

    float x = ix * 0.0001f;
    float y = iy * 0.0001f;

    ctrl_drive_feed_watchdog();
    ctrl_drive_set_axes(x, y);
}

/* ── WebSocket handler ─────────────────────────────────────────────────────*/
static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        ws_client_add(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) { ws_client_remove(fd); return err; }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        httpd_ws_frame_t close_frame = { .type = HTTPD_WS_TYPE_CLOSE };
        httpd_ws_send_frame(req, &close_frame);
        ws_client_remove(fd);
        return ESP_OK;
    }

    if (frame.len == 0) return ESP_OK;

    /* Binary axes packet  -  no allocation, direct dispatch */
    if (frame.type == HTTPD_WS_TYPE_BINARY) {
        if (frame.len > 16) return ESP_OK;   /* sanity: axes packet is 5 bytes */
        uint8_t bin[16];
        frame.payload = bin;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err == ESP_OK) dispatch_binary(bin, frame.len);
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    /* Text control message  -  allocate only for infrequent words */
    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) { free(buf); ws_client_remove(fd); return err; }
    buf[frame.len] = '\0';

    dispatch_text(req, (char *)buf);
    free(buf);
    return ESP_OK;
}

/* ── Public API ────────────────────────────────────────────────────────────*/

/* ── LittleFS inventory (called once at startup) ───────────────────────────
 * Logs every file in /littlefs so you can confirm .gz assets are present.
 * Output appears in the serial monitor tagged [app_srv] at INFO level.
 * Example:
 *   [app_srv] FS: /littlefs/index.js        ( 20325 B)
 *   [app_srv] FS: /littlefs/index.js.gz     (  5706 B)  ← gz present, will be served
 *   [app_srv] FS: /littlefs/base.css         (  7822 B)
 *   [app_srv] FS: /littlefs/base.css.gz      (  1704 B)  ← gz present, will be served
 */
static void log_fs_inventory(void)
{
    DIR *dir = opendir("/littlefs");
    if (!dir) {
        ESP_LOGW(TAG, "FS: cannot open /littlefs - is LittleFS mounted?");
        return;
    }

    ESP_LOGI(TAG, "FS: --- LittleFS contents ---");

    /* Declare all locals before the loop body (C89/C99 compat) */
    size_t         total    = 0;
    int            count    = 0;
    struct dirent *entry    = NULL;
    char           full[300];  /* big enough for "/littlefs/" + filename + ".gz" */
    struct stat    st;
    size_t         sz       = 0;
    size_t         dname_len = 0;
    bool           is_gz    = false;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;  /* skip . .. and subdirs */

        snprintf(full, sizeof(full), "/littlefs/%s", entry->d_name);

        sz = 0;
        if (stat(full, &st) == 0) sz = (size_t)st.st_size;

        /* Guard strlen subtraction — size_t underflows if name is < 3 chars */
        dname_len = strlen(entry->d_name);
        is_gz     = (dname_len > 3 &&
                     strcmp(entry->d_name + dname_len - 3, ".gz") == 0);

        ESP_LOGI(TAG, "FS:   %-30s %6zu B%s",
                 entry->d_name, sz,
                 is_gz ? "  [gz - will be served compressed]" : "");
        total += sz;
        count++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "FS: --- %d files, %zu B total ---", count, total);
}

esp_err_t app_server_start(void)
{
    s_ws_mutex   = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_WS_CLIENTS; i++) s_ws_fds[i] = -1;

    vfs_mount_littlefs();
    log_fs_inventory();   /* logs all files + gz status to serial */

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 20;
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    /* Performance: allow browser to open concurrent asset connections.
     * Tight recv/send timeouts prevent dead connections blocking the task.
     * Default max_open_sockets is 7 on ESP32  -  keep it; just make timeouts fast. */
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.backlog_conn      = 5;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri="/",              .method=HTTP_GET,  .handler=handle_index       },
        { .uri="/base.css",      .method=HTTP_GET,  .handler=handle_base_css     },
        { .uri="/index.css",     .method=HTTP_GET,  .handler=handle_index_css         },
        { .uri="/index.js",     .method=HTTP_GET,  .handler=handle_index_js          },
        { .uri="/settings",      .method=HTTP_GET,  .handler=handle_settings    },
        { .uri="/settings.css",  .method=HTTP_GET,  .handler=handle_settings_css},
        { .uri="/settings.js",   .method=HTTP_GET,  .handler=handle_settings_js },
        { .uri="/favicon.ico",   .method=HTTP_GET,  .handler=handle_favicon     },
        { .uri="/favicon.svg",   .method=HTTP_GET,  .handler=handle_favicon     },
        { .uri="/api/status",    .method=HTTP_GET,  .handler=handle_api_status  },
        { .uri="/api/scan",      .method=HTTP_GET,  .handler=handle_api_scan    },
        { .uri="/api/connect",   .method=HTTP_POST, .handler=handle_api_connect },
        { .uri="/api/erase",     .method=HTTP_POST, .handler=handle_api_erase   },
        { .uri="/ws",            .method=HTTP_GET,  .handler=ws_handler,
          .is_websocket=true                                                     },
        { .uri="/*",             .method=HTTP_GET,  .handler=handle_404         },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_httpd, &routes[i]);

    ota_server_register(s_httpd);
    ota_server_mark_valid();

    ESP_LOGI(TAG, "App server started (port 80)");
    return ESP_OK;
}

void app_server_stop(void)
{
    ctrl_drive_emergency_stop();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    ESP_LOGI(TAG, "App server stopped");
}

void app_server_push_telemetry(void)
{
    if (!s_httpd || s_ws_count == 0) return;

    const char *json = health_monitor_get_telemetry_json();
    size_t      jlen = strlen(json);

    int live[MAX_WS_CLIENTS];
    int nlive = 0;
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] >= 0) live[nlive++] = s_ws_fds[i];
    xSemaphoreGive(s_ws_mutex);

    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = jlen,
    };
    for (int i = 0; i < nlive; i++) {
        esp_err_t err = httpd_ws_send_frame_async(s_httpd, live[i], &frame);
        if (err != ESP_OK) {
            int count = 0;
            xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
            for (int j = 0; j < MAX_WS_CLIENTS; j++) {
                if (s_ws_fds[j] == live[i]) {
                    s_ws_fds[j] = -1;
                    s_ws_count--;
                    break;
                }
            }
            count = s_ws_count;
            xSemaphoreGive(s_ws_mutex);
            prov_ui_set_client_count(count);
            ctrl_drive_emergency_stop();
            ESP_LOGD(TAG, "Pruned dead WS fd=%d (total=%d)", live[i], count);
        }
    }
}

int app_server_get_client_count(void)
{
    return s_ws_count;
}
