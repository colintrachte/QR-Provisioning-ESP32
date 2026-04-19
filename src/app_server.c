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
 *
 * All files are opened "rb". Text mode would corrupt byte counts on any
 * platform that translates LF→CRLF, including some newlib configurations.
 *
 * WebSocket client tracking
 * ─────────────────────────
 * s_ws_fds[] is a fixed array of open socket fds, protected by s_ws_mutex.
 * app_server_push_telemetry() snapshots the live fd list under the mutex,
 * releases the mutex, then sends to each fd. httpd_ws_send_frame_async
 * posts to the httpd send queue and must NOT be called while holding a
 * mutex that httpd handlers could also need.
 */

#include "app_server.h"
#include "ctrl_drive.h"
#include "o_led.h"
#include "health_monitor.h"
#include "prov_ui.h"
#include "ota_server.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_server";

/* ── LittleFS ───────────────────────────────────────────────────────────────*/
#define FS_BASE     "/littlefs"
#define FS_PART     "storage"    /* must match partitions.csv label           */
#define SERVE_CHUNK  1024

static void fs_mount(void)
{
    static bool s_mounted = false;
    if (s_mounted) return;
    esp_vfs_littlefs_conf_t cfg = {
        .base_path              = FS_BASE,
        .partition_label        = FS_PART,
        .format_if_mount_failed = true,
    };
    if (esp_vfs_littlefs_register(&cfg) == ESP_OK)
        s_mounted = true;
    else
        ESP_LOGE(TAG, "LittleFS mount failed (run: pio run -t uploadfs)");
}

static esp_err_t serve_file(httpd_req_t *req, const char *path, const char *mime)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "serve: not found: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }
    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char *buf = malloc(SERVE_CHUNK);
    if (!buf) { fclose(f); httpd_resp_send_500(req); return ESP_OK; }
    size_t n;
    while ((n = fread(buf, 1, SERVE_CHUNK, f)) > 0)
        httpd_resp_send_chunk(req, buf, (ssize_t)n);
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    fclose(f);
    return ESP_OK;
}

/* ── File route handlers ─────────────────────────────────────────────────────
 * One handler per known file. No URI→path construction.
 */
static esp_err_t handle_index(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/index.html", "text/html"); }

static esp_err_t handle_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/style.css",  "text/css"); }

static esp_err_t handle_js(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/script.js",  "application/javascript"); }

static esp_err_t handle_settings(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/settings.html", "text/html"); }

static esp_err_t handle_settings_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/settings.css", "text/css"); }

static esp_err_t handle_settings_js(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/settings.js",  "application/javascript"); }

static esp_err_t handle_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_404(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

/* ── Settings API endpoints ─────────────────────────────────────────────────
 *
 * GET  /api/status      — JSON: current WiFi state (ssid, ip, rssi, connected)
 * GET  /api/scan        — JSON: cached AP list, or {"status":"scanning"}.
 *                         ?refresh=1 triggers a fresh scan before returning.
 * POST /api/connect     — body: ssid=&pass=  → save to NVS and reboot
 * POST /api/erase       — erase credentials and reboot into provisioning
 *
 * Scan results are cached from the last esp_wifi_scan. In STA mode a scan
 * causes a brief (~100 ms) disconnection on some APs — acceptable for a
 * settings page triggered by user action. The cached result is served
 * immediately; a background task rebuilds it when ?refresh=1 is passed. */

#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

/* NVS namespace/keys must match wifi_manager.c */
#define SETTINGS_NVS_NAMESPACE  "wifi_mgr"
#define SETTINGS_NVS_KEY_SSID   "ssid"
#define SETTINGS_NVS_KEY_PASS   "pass"

/* ── Scan cache ─────────────────────────────────────────────────────────────*/
#define MAX_SCAN_APS  20

typedef struct {
    char ssid[33];
    int  rssi;
    int  quality;      /* 0-100 */
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
                /* JSON-escape backslash and quote in SSID */
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

/* GET /api/scan  — ?refresh=1 triggers a new scan */
static esp_err_t handle_api_scan(httpd_req_t *req)
{
    /* Check for ?refresh=1 */
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

/* POST /api/connect — body: ssid=<url-encoded>&pass=<url-encoded> */
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

    /* Extract and decode ssid= and pass= fields */
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

    /* Write to NVS — same namespace/keys as wifi_manager uses */
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

/* POST /api/erase — erase credentials and reboot into provisioning */
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

/* ── WebSocket client tracking ──────────────────────────────────────────────*/
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

/* ── WebSocket message dispatch ─────────────────────────────────────────────*/

static void dispatch(httpd_req_t *req, const char *msg)
{
    /* Every recognised message resets the command watchdog, including ping.
     * This keeps the robot alive as long as the browser tab is open and
     * reachable — even if the user isn't actively driving. */
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
    float x = 0.0f, y = 0.0f;
    if (sscanf(msg, "x:%f,y:%f", &x, &y) == 2) {
        ctrl_drive_feed_watchdog();
        ctrl_drive_set_axes(x, y);
        return;
    }
    ESP_LOGD(TAG, "Unknown WS msg: %s", msg);
}

/* ── WebSocket handler ──────────────────────────────────────────────────────*/

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        /* Upgrade handshake — new connection */
        ws_client_add(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};

    /* First call with len=0 populates frame.type and frame.len */
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) { ws_client_remove(fd); return err; }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        /* Echo close frame per RFC 6455 §5.5.1 */
        httpd_ws_frame_t close_frame = { .type = HTTPD_WS_TYPE_CLOSE };
        httpd_ws_send_frame(req, &close_frame);
        ws_client_remove(fd);
        return ESP_OK;
    }

    if (frame.len == 0 || frame.type != HTTPD_WS_TYPE_TEXT)
        return ESP_OK;

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) { free(buf); ws_client_remove(fd); return err; }
    buf[frame.len] = '\0';

    dispatch(req, (char *)buf);
    free(buf);
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

esp_err_t app_server_start(void)
{
    s_ws_mutex   = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_WS_CLIENTS; i++) s_ws_fds[i] = -1;

    fs_mount();

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;   /* app + settings + API + OTA endpoints */
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;  /* required for wildcard */

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        /* Static files */
        { .uri="/",              .method=HTTP_GET,  .handler=handle_index       },
        { .uri="/style.css",     .method=HTTP_GET,  .handler=handle_css         },
        { .uri="/script.js",     .method=HTTP_GET,  .handler=handle_js          },
        { .uri="/settings",      .method=HTTP_GET,  .handler=handle_settings    },
        { .uri="/settings.css",  .method=HTTP_GET,  .handler=handle_settings_css},
        { .uri="/settings.js",   .method=HTTP_GET,  .handler=handle_settings_js },
        { .uri="/favicon.ico",   .method=HTTP_GET,  .handler=handle_favicon     },
        /* Settings API */
        { .uri="/api/status",    .method=HTTP_GET,  .handler=handle_api_status  },
        { .uri="/api/scan",      .method=HTTP_GET,  .handler=handle_api_scan    },
        { .uri="/api/connect",   .method=HTTP_POST, .handler=handle_api_connect },
        { .uri="/api/erase",     .method=HTTP_POST, .handler=handle_api_erase   },
        /* WebSocket */
        { .uri="/ws",            .method=HTTP_GET,  .handler=ws_handler,
          .is_websocket=true                                                     },
        /* Catch-all */
        { .uri="/*",             .method=HTTP_GET,  .handler=handle_404         },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_httpd, &routes[i]);

    /* OTA update endpoints: POST /ota/firmware and POST /ota/filesystem.
     * Registered after app routes so the wildcard 404 handler doesn't
     * shadow them — OTA routes are POST, the wildcard only matches GET. */
    ota_server_register(s_httpd);

    /* Mark running firmware as valid — cancels any pending rollback.
     * Reaching this point means WiFi connected and the HTTP server started
     * successfully, which is a reasonable "healthy" signal. */
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

    /* Snapshot fd list under lock, then release before sending.
     * httpd_ws_send_frame_async posts to the httpd send queue and must not
     * be called while holding a mutex that httpd task handlers also acquire. */
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
            /* Prune the dead fd */
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
