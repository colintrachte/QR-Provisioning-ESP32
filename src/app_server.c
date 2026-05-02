/**
 * app_server.c — Robot control HTTP file server and WebSocket dispatcher.
 *
 * File serving
 * ────────────
 * Explicit named routes call web_serve_file() with a literal path. Dynamic
 * URI→path concatenation is avoided because httpd URIs can be up to
 * CONFIG_HTTPD_MAX_URI_LEN (512 B) and would overflow any fixed stack buffer
 * when prepending "/littlefs". A wildcard catch-all returns 404 for
 * any URI not in the route table. Requires uri_match_fn = wildcard.
 *
 * JSON
 * ────
 * All JSON responses are built with cJSON. Hand-rolled snprintf JSON is gone.
 * vfs_build_status_json() and vfs_build_scan_json() (utils_filesystem) own
 * the shared status/scan shape used by both portal and app_server.
 */

#include "app_server.h"
#include "ctrl_drive.h"
#include "o_led.h"
#include "health_monitor.h"
#include "prov_ui.h"
#include "ota_server.h"
#include "utils_filesystem.h"
#include "utils_web.h"
#include "utils_json.h"
#include "config.h"

#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_server";

#define FS_BASE  "/littlefs"

/* ── Misc API handlers ─────────────────────────────────────────────────── */

/* POST /api/jserror — forward frontend exceptions to the serial log */
static esp_err_t handle_api_jserror(httpd_req_t *req)
{
    char body[256] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n > 0) {
        body[n] = '\0';
        ESP_LOGW(TAG, "JS error: %s", body);
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* ── File route handlers ────────────────────────────────────────────────── */

static esp_err_t handle_index(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/index.html",    "text/html",              false); }

static esp_err_t handle_base_css(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/base.css",      "text/css",               true); }

static esp_err_t handle_index_css(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/index.css",     "text/css",               true); }

static esp_err_t handle_index_js(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/index.js",      "application/javascript", true); }

static esp_err_t handle_settings(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/settings.html", "text/html",              false); }

static esp_err_t handle_settings_css(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/settings.css",  "text/css",               true); }

static esp_err_t handle_settings_js(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/settings.js",   "application/javascript", true); }

static esp_err_t handle_favicon(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/favicon.svg",   "image/svg+xml",          true); }

static esp_err_t handle_404(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

/* ── Settings API ──────────────────────────────────────────────────────── */

#define SETTINGS_NVS_NAMESPACE  "wifi_mgr"
#define SETTINGS_NVS_KEY_SSID   "ssid"
#define SETTINGS_NVS_KEY_PASS   "pass"

#define MAX_SCAN_APS  20

/* Scan state — raw records kept so vfs_build_scan_json() can process them */
static SemaphoreHandle_t  s_scan_mutex   = NULL;
static wifi_ap_record_t   s_scan_recs[MAX_SCAN_APS];
static int                s_scan_count   = 0;
static bool               s_scan_ready   = false;
static volatile bool      s_scan_running = false;

static EventGroupHandle_t s_scan_evt     = NULL;
#define SCAN_DONE_BIT  BIT0
static bool s_events_registered = false;

static void scan_init(void)
{
    if (!s_scan_evt) s_scan_evt = xEventGroupCreate();
}

static void scan_event_cb(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
        if (s_scan_evt) xEventGroupSetBits(s_scan_evt, SCAN_DONE_BIT);
}

static void scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    s_scan_running = true;

    if (esp_wifi_scan_start(&cfg, false) != ESP_OK) {
        ESP_LOGW(TAG, "Scan start failed");
        s_scan_running = false;
        vTaskDelete(NULL);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_scan_evt, SCAN_DONE_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(6000));

    if (!(bits & SCAN_DONE_BIT)) {
        ESP_LOGW(TAG, "Scan timeout");
        s_scan_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint16_t count = MAX_SCAN_APS;
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_scan_recs);
    if (err == ESP_OK) {
        s_scan_count = (int)count;
        s_scan_ready = true;
        ESP_LOGI(TAG, "Scan complete: %d APs", s_scan_count);
    } else {
        ESP_LOGW(TAG, "Scan get records failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_scan_mutex);

    s_scan_running = false;
    vTaskDelete(NULL);
}

static void trigger_scan(void)
{
    if (!s_scan_running)
        xTaskCreate(scan_task, "settings_scan", 4096, NULL, 3, NULL);
}

/* ── JSON response helper ──────────────────────────────────────────────── */

/**
 * Serialise a cJSON object to a heap buffer, send it, then free both.
 * Sets Content-Type: application/json and Cache-Control: no-cache.
 * Returns ESP_OK even on cJSON serialisation failure (sends 500 instead).
 */
static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
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
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

/* GET /api/status */
static esp_err_t handle_api_status(httpd_req_t *req)
{
    return send_json(req, vfs_build_status_json());
}

/* GET /api/scan  —  ?refresh=1 triggers a new scan */
static esp_err_t handle_api_scan(httpd_req_t *req)
{
    char query[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        strstr(query, "refresh=1"))
        trigger_scan();

    if (!s_scan_ready || s_scan_running) {
        cJSON *scanning = cJSON_CreateObject();
        cJSON_AddStringToObject(scanning, "status", "scanning");
        return send_json(req, scanning);
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    cJSON *arr = vfs_build_scan_json(s_scan_recs, s_scan_count);
    xSemaphoreGive(s_scan_mutex);

    return send_json(req, arr);
}

/* POST /api/connect */
static esp_err_t handle_api_connect(httpd_req_t *req)
{
    char body[320] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_OK;
    }

    char ssid_enc[128] = {0}, pass_enc[128] = {0};
    char ssid[64]      = {0}, pass[64]      = {0};

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
    web_url_decode(ssid_enc, ssid, sizeof(ssid));
    web_url_decode(pass_enc, pass, sizeof(pass));

    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_OK;
    }

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

/* ── WebSocket client tracking ─────────────────────────────────────────── */
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
 * TEXT frames  —  infrequent control words (arm, disarm, ping, stop, led:N)
 * BINARY frames — high-frequency axes packets (up to 100 Hz)
 *   Format: 5 bytes, little-endian
 *     [0]     uint8  type = 0x01
 *     [1..2]  int16  x * 10000   (range -10000..10000 → -1.0..1.0)
 *     [3..4]  int16  y * 10000
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

    int16_t ix = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
    int16_t iy = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));

    ctrl_drive_feed_watchdog();
    ctrl_drive_set_axes(ix * 0.0001f, iy * 0.0001f);
}

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

    if (frame.type == HTTPD_WS_TYPE_BINARY) {
        if (frame.len > 16) return ESP_OK;
        uint8_t bin[16];
        frame.payload = bin;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err == ESP_OK) dispatch_binary(bin, frame.len);
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

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

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t app_server_start(void)
{
    s_ws_mutex   = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();
    scan_init();
    for (int i = 0; i < MAX_WS_CLIENTS; i++) s_ws_fds[i] = -1;

    if (!s_events_registered) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                   scan_event_cb, NULL);
        s_events_registered = true;
    }

    esp_err_t ret = vfs_mount_littlefs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS — aborting server start");
        if (s_ws_mutex)   { vSemaphoreDelete(s_ws_mutex);   s_ws_mutex   = NULL; }
        if (s_scan_mutex) { vSemaphoreDelete(s_scan_mutex); s_scan_mutex = NULL; }
        return ret;
    }
    vfs_log_inventory();   /* replaces log_fs_inventory() — now in utils_filesystem */

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 20;
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.backlog_conn      = 5;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri="/",              .method=HTTP_GET,  .handler=handle_index        },
        { .uri="/base.css",      .method=HTTP_GET,  .handler=handle_base_css     },
        { .uri="/index.css",     .method=HTTP_GET,  .handler=handle_index_css    },
        { .uri="/index.js",      .method=HTTP_GET,  .handler=handle_index_js     },
        { .uri="/settings",      .method=HTTP_GET,  .handler=handle_settings     },
        { .uri="/settings.css",  .method=HTTP_GET,  .handler=handle_settings_css },
        { .uri="/settings.js",   .method=HTTP_GET,  .handler=handle_settings_js  },
        { .uri="/favicon.ico",   .method=HTTP_GET,  .handler=handle_favicon      },
        { .uri="/favicon.svg",   .method=HTTP_GET,  .handler=handle_favicon      },
        { .uri="/api/status",    .method=HTTP_GET,  .handler=handle_api_status   },
        { .uri="/api/scan",      .method=HTTP_GET,  .handler=handle_api_scan     },
        { .uri="/api/connect",   .method=HTTP_POST, .handler=handle_api_connect  },
        { .uri="/api/erase",     .method=HTTP_POST, .handler=handle_api_erase    },
        { .uri="/ws",            .method=HTTP_GET,  .handler=ws_handler,
          .is_websocket=true                                                      },
        { .uri="/api/jserror",   .method=HTTP_POST, .handler=handle_api_jserror  },
        { .uri="/*",             .method=HTTP_GET,  .handler=handle_404          },
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
