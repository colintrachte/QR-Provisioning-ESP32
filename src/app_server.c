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
    if (strcmp(msg, "ping") == 0) {
        httpd_ws_frame_t pong = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)"pong",
            .len     = 4,
        };
        httpd_ws_send_frame(req, &pong);
        return;
    }
    if (strcmp(msg, "arm") == 0) {
        ctrl_drive_set_armed(true);
        o_led_blink(LED_PATTERN_HEARTBEAT);
        return;
    }
    if (strcmp(msg, "disarm") == 0) {
        ctrl_drive_set_armed(false);
        o_led_blink(LED_PATTERN_SLOW_BLINK);
        return;
    }
    if (strcmp(msg, "stop") == 0) {
        ctrl_drive_set_axes(0.0f, 0.0f);
        return;
    }
    if (strncmp(msg, "led:", 4) == 0) {
        o_led_set(strtof(msg + 4, NULL));
        return;
    }
    float x = 0.0f, y = 0.0f;
    if (sscanf(msg, "x:%f,y:%f", &x, &y) == 2) {
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
    s_ws_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_WS_CLIENTS; i++) s_ws_fds[i] = -1;

    fs_mount();

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;  /* required for wildcard */

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri="/",            .method=HTTP_GET, .handler=handle_index   },
        { .uri="/style.css",   .method=HTTP_GET, .handler=handle_css     },
        { .uri="/script.js",   .method=HTTP_GET, .handler=handle_js      },
        { .uri="/favicon.ico", .method=HTTP_GET, .handler=handle_favicon },
        { .uri="/ws",          .method=HTTP_GET, .handler=ws_handler,
          .is_websocket=true                                              },
        { .uri="/*",           .method=HTTP_GET, .handler=handle_404     },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_httpd, &routes[i]);

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
            xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
            for (int j = 0; j < MAX_WS_CLIENTS; j++) {
                if (s_ws_fds[j] == live[i]) {
                    s_ws_fds[j] = -1;
                    s_ws_count--;
                    break;
                }
            }
            xSemaphoreGive(s_ws_mutex);
            ESP_LOGD(TAG, "Pruned dead WS fd=%d", live[i]);
        }
    }
}

int app_server_get_client_count(void)
{
    return s_ws_count;
}
