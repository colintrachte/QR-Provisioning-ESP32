/**
 * portal.c — SoftAP captive portal.
 *
 * Scan model: blocking, manual refresh only.
 *   - One scan at portal startup (safe: no clients yet).
 *   - GET /api/scan returns cached result immediately.
 *   - GET /api/rescan triggers a background scan and returns current cache.
 *
 * No background scan task = no radio channel-hopping while a phone is
 * trying to associate. This eliminates the missed-probe reboot bug.
 *
 * JSON
 * ────
 * Scan results and status responses are built with vfs_build_scan_json() and
 * vfs_build_status_json() (utils_filesystem). No hand-rolled snprintf JSON.
 */

#include "portal.h"
#include "config.h"
#include "utils_filesystem.h"
#include "utils_web.h"
#include "utils_json.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "portal";

#define FS_BASE  "/littlefs"

/* ── Credentials ───────────────────────────────────────────────────────── */
static char s_ssid[33] = {0};
static char s_pass[65] = {0};

/* ── Synchronisation ────────────────────────────────────────────────────── */
#define CREDS_RECEIVED_BIT  BIT0
static EventGroupHandle_t s_event_group = NULL;

/* ── Scan cache ─────────────────────────────────────────────────────────── */
#define SCAN_MAX_APS  20

/* Store raw wifi_ap_record_t so vfs_build_scan_json() can process them.
 * No more duplicated SSID-escaping or quality-conversion logic here. */
static SemaphoreHandle_t s_scan_mutex   = NULL;
static wifi_ap_record_t  s_scan_recs[SCAN_MAX_APS];
static int               s_scan_count   = 0;
static bool              s_scan_ready   = false;
static volatile bool     s_scan_pending = false;

/* ── Blocking scan ──────────────────────────────────────────────────────── */

static void run_scan_once(void)
{
    ESP_LOGI(TAG, "scan: starting blocking scan");

    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan: start failed");
        return;
    }

    uint16_t count = SCAN_MAX_APS;
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_scan_recs);
    if (err == ESP_OK) {
        s_scan_count = (int)count;
        s_scan_ready = true;
        ESP_LOGI(TAG, "scan: %d APs cached", s_scan_count);
    } else {
        ESP_LOGW(TAG, "scan: get records failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_scan_mutex);
}

static void scan_bg_task(void *arg)
{
    (void)arg;
    run_scan_once();
    s_scan_pending = false;
    vTaskDelete(NULL);
}

/* ── DNS hijack ─────────────────────────────────────────────────────────── */
static int           s_dns_sock = -1;
static volatile bool s_dns_run  = false;

static void dns_reply(int sock, struct sockaddr_in *client,
                      const uint8_t *query, int qlen)
{
    if (qlen < 12 || qlen > 496) return;

    uint8_t  resp[512];
    uint32_t gw = inet_addr(AP_GW_IP);
    memcpy(resp, query, qlen);
    resp[2] = 0x81; resp[3] = 0x80;
    resp[6] = 0x00; resp[7] = 0x01;
    resp[8] = resp[9] = resp[10] = resp[11] = 0;

    int pos = qlen;
    resp[pos++] = 0xC0; resp[pos++] = 0x0C;
    resp[pos++] = 0x00; resp[pos++] = 0x01;
    resp[pos++] = 0x00; resp[pos++] = 0x01;
    resp[pos++] = 0x00; resp[pos++] = 0x00;
    resp[pos++] = 0x00; resp[pos++] = 0x3C;
    resp[pos++] = 0x00; resp[pos++] = 0x04;
    memcpy(resp + pos, &gw, 4); pos += 4;

    sendto(sock, resp, pos, 0,
           (struct sockaddr *)client, sizeof(*client));
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (s_dns_run) {
        int n = recvfrom(s_dns_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client, &clen);
        if (n > 0) dns_reply(s_dns_sock, &client, buf, n);
    }
    vTaskDelete(NULL);
}

static void start_dns(void)
{
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) { ESP_LOGE(TAG, "DNS socket failed"); return; }

    int yes = 1;
    setsockopt(s_dns_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(s_dns_sock); s_dns_sock = -1; return;
    }
    s_dns_run = true;
    xTaskCreate(dns_task, "portal_dns", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "DNS hijack → " AP_GW_IP);
}

static void stop_dns(void)
{
    s_dns_run = false;
    if (s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    vTaskDelay(pdMS_TO_TICKS(150));
}

/* ── JSON response helper ───────────────────────────────────────────────── */

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    if (!obj) { httpd_resp_send_500(req); return ESP_OK; }
    char *body = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!body) { httpd_resp_send_500(req); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

/* ── httpd handlers ─────────────────────────────────────────────────────── */

static esp_err_t handle_root(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/setup.html", "text/html",              false); }

static esp_err_t handle_favicon(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/favicon.svg", "image/svg+xml",         true); }

static esp_err_t handle_base_css(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/base.css",    "text/css",              true); }

static esp_err_t handle_setup_css(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/setup.css",   "text/css",              true); }

static esp_err_t handle_setup_js(httpd_req_t *req)
    { return web_serve_file(req, FS_BASE "/setup.js",    "application/javascript", true); }

/* ── Captive-portal probe handler ───────────────────────────────────────────
 *
 * Triggers the system captive-portal UI on iOS, Android, Windows, Linux,
 * and Firefox. See inline comments for per-OS behaviour.
 */
static esp_err_t handle_probe(httpd_req_t *req)
{
    const char *uri = req->uri;

    /* iOS / macOS CNA */
    if (strcmp(uri, "/hotspot-detect.html")       == 0 ||
        strcmp(uri, "/library/test/success.html") == 0 ||
        strcmp(uri, "/ captive.apple.com")        == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
        httpd_resp_set_hdr(req, "Content-Length", "0");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* Android — 302 (not 204) to trigger captive portal notification */
    if (strcmp(uri, "/generate_204") == 0 ||
        strcmp(uri, "/gen_204")      == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
        httpd_resp_set_hdr(req, "Content-Length", "0");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* Windows NCSI — wrong body forces captive detection */
    if (strcmp(uri, "/connecttest.txt") == 0 ||
        strcmp(uri, "/ncsi.txt")        == 0) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "captive");
        return ESP_OK;
    }
    if (strcmp(uri, "/redirect") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://www.msftconnecttest.com/redirect");
        httpd_resp_set_hdr(req, "Content-Length", "0");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* Windows 11 */
    if (strcmp(uri, "/captiveportal/generate_204") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
        httpd_resp_set_hdr(req, "Content-Length", "0");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* Linux / NetworkManager */
    if (strcmp(uri, "/nm-check.txt")             == 0 ||
        strcmp(uri, "/check_network_status.txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "captive");
        return ESP_OK;
    }

    /* Firefox */
    if (strcmp(uri, "/canonical.html") == 0 ||
        strcmp(uri, "/success.txt")    == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
        httpd_resp_set_hdr(req, "Content-Length", "0");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* Wildcard — any other request → redirect to setup page */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/status */
static esp_err_t handle_api_status(httpd_req_t *req)
{
    return send_json(req, vfs_build_status_json());
}

/* GET /api/scan — returns cached AP list immediately */
static esp_err_t handle_scan(httpd_req_t *req)
{
    if (!s_scan_ready) {
        cJSON *scanning = cJSON_CreateObject();
        cJSON_AddStringToObject(scanning, "status", "scanning");
        return send_json(req, scanning);
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    cJSON *arr = vfs_build_scan_json(s_scan_recs, s_scan_count);
    xSemaphoreGive(s_scan_mutex);

    return send_json(req, arr);
}

/* GET /api/rescan — triggers background scan, returns current cache */
static esp_err_t handle_rescan(httpd_req_t *req)
{
    if (!s_scan_pending) {
        s_scan_pending = true;
        xTaskCreate(scan_bg_task, "portal_rescan", 4096, NULL, 3, NULL);
    }
    return handle_scan(req);
}

/* ── URL field extraction ───────────────────────────────────────────────── */

static void extract_field(const char *body, const char *key,
                           char *out, int out_size)
{
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    int len = end ? (int)(end - p) : (int)strlen(p);

    char *tmp = malloc(len + 1);
    if (!tmp) { out[0] = '\0'; return; }
    memcpy(tmp, p, len);
    tmp[len] = '\0';
    web_url_decode(tmp, out, out_size);
    free(tmp);
}

/* POST /api/connect */
static esp_err_t handle_connect(httpd_req_t *req)
{
    char body[320] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    body[n] = '\0';

    extract_field(body, "ssid",     s_ssid, sizeof(s_ssid));
    extract_field(body, "password", s_pass, sizeof(s_pass));

    if (!s_ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Credentials received: SSID='%s'", s_ssid);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Saved. Connecting...");

    xEventGroupSetBits(s_event_group, CREDS_RECEIVED_BIT);
    return ESP_OK;
}

/* ── httpd lifecycle ─────────────────────────────────────────────────────── */
static httpd_handle_t s_httpd = NULL;

static void start_httpd(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 24;
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.backlog_conn      = 5;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    static const httpd_uri_t routes[] = {
        /* Setup page assets — specific routes before wildcard */
        { .uri="/",            .method=HTTP_GET,  .handler=handle_root      },
        { .uri="/base.css",    .method=HTTP_GET,  .handler=handle_base_css  },
        { .uri="/setup.css",   .method=HTTP_GET,  .handler=handle_setup_css },
        { .uri="/setup.js",    .method=HTTP_GET,  .handler=handle_setup_js  },
        { .uri="/favicon.svg", .method=HTTP_GET,  .handler=handle_favicon   },
        { .uri="/favicon.ico", .method=HTTP_GET,  .handler=handle_favicon   },

        /* WiFi API */
        { .uri="/api/status",  .method=HTTP_GET,  .handler=handle_api_status },
        { .uri="/api/scan",    .method=HTTP_GET,  .handler=handle_scan       },
        { .uri="/api/rescan",  .method=HTTP_GET,  .handler=handle_rescan     },
        { .uri="/api/connect", .method=HTTP_POST, .handler=handle_connect    },

        /* OS captive portal probes — explicit before wildcard */
        { .uri="/hotspot-detect.html",       .method=HTTP_GET, .handler=handle_probe },
        { .uri="/library/test/success.html", .method=HTTP_GET, .handler=handle_probe },
        { .uri="/generate_204",              .method=HTTP_GET, .handler=handle_probe },
        { .uri="/gen_204",                   .method=HTTP_GET, .handler=handle_probe },
        { .uri="/connecttest.txt",           .method=HTTP_GET, .handler=handle_probe },
        { .uri="/ncsi.txt",                  .method=HTTP_GET, .handler=handle_probe },
        { .uri="/redirect",                  .method=HTTP_GET, .handler=handle_probe },
        { .uri="/captiveportal/generate_204",.method=HTTP_GET, .handler=handle_probe },
        { .uri="/nm-check.txt",              .method=HTTP_GET, .handler=handle_probe },
        { .uri="/canonical.html",            .method=HTTP_GET, .handler=handle_probe },

        /* Wildcard catch-all — MUST BE LAST */
        { .uri="/*", .method=HTTP_GET,  .handler=handle_probe },
        { .uri="/*", .method=HTTP_HEAD, .handler=handle_probe },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_httpd, &routes[i]);

    ESP_LOGI(TAG, "Portal httpd started (%d routes)",
             (int)(sizeof(routes)/sizeof(routes[0])));
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void portal_start(const char *ap_ssid)
{
    (void)ap_ssid;

    s_event_group = xEventGroupCreate();
    if (!s_scan_mutex) s_scan_mutex = xSemaphoreCreateMutex();

    vfs_mount_littlefs();
    start_httpd();
    start_dns();

    /* One blocking scan at startup — safe, no clients yet */
    run_scan_once();

    ESP_LOGI(TAG, "Portal running at http://" AP_GW_IP "/");
    xEventGroupWaitBits(s_event_group, CREDS_RECEIVED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "Credentials received — portal unblocking");
}

void portal_stop(void)
{
    stop_dns();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    if (s_event_group) { vEventGroupDelete(s_event_group); s_event_group = NULL; }
    ESP_LOGI(TAG, "Portal stopped");
}

void portal_get_credentials(char *out_ssid, char *out_pass)
{
    strncpy(out_ssid, s_ssid, 33); out_ssid[32] = '\0';
    strncpy(out_pass, s_pass, 65); out_pass[64] = '\0';
}
