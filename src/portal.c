/**
 * portal.c — SoftAP captive portal.
 *
 * Scan model: blocking, manual refresh only.
 *   - One scan at portal startup (safe: no clients yet).
 *   - GET /api/scan returns cached result immediately.
 *   - GET /api/rescan runs a fresh blocking scan (~3 s) and returns JSON.
 *
 * No background scan task = no radio channel-hopping while a phone is
 * trying to associate. This eliminates the missed-probe reboot bug.
 */

#include "portal.h"
#include "config.h"
#include "vfs_mount.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "portal";

#define FS_BASE       "/littlefs"
#define SERVE_CHUNK   1024

/* ── File serving helper ───────────────────────────────────────────────────*/
static esp_err_t serve_file(httpd_req_t *req, const char *path, const char *mime)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "serve: not found: %s  (run pio run -t uploadfs)", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }
    httpd_resp_set_type(req, mime);
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

/* ── Credentials ───────────────────────────────────────────────────────────*/
static char s_ssid[33] = {0};
static char s_pass[65] = {0};

/* ── Synchronisation ──────────────────────────────────────────────────────*/
#define CREDS_RECEIVED_BIT BIT0
static EventGroupHandle_t s_event_group = NULL;

/* ── Scan cache ────────────────────────────────────────────────────────────*/
#define SCAN_MAX_APS     20

typedef struct {
    char ssid[64];   /* JSON-escaped */
    int  quality;    /* 0–100 derived from RSSI */
    bool secure;
} ap_cache_t;

static SemaphoreHandle_t  s_scan_mutex  = NULL;
static ap_cache_t         s_scan_aps[SCAN_MAX_APS];
static int                s_scan_count  = 0;
static bool               s_scan_ready  = false;

/* ── Blocking scan ─────────────────────────────────────────────────────────
 * Called once at startup and on explicit GET /api/rescan.
 * Radio is off-channel for ~3 s; no AP clients can be present because
 * we only call this before the phone joins or when user clicks Rescan.
 */
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
    wifi_ap_record_t *recs = calloc(SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    if (!recs) return;

    esp_wifi_scan_get_ap_records(&count, recs);

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    s_scan_count = 0;
    for (int i = 0; i < (int)count && s_scan_count < SCAN_MAX_APS; i++) {
        if (recs[i].ssid[0] == '\0') continue;

        int si = 0;
        for (int j = 0; recs[i].ssid[j] && j < 32; j++) {
            char c = (char)recs[i].ssid[j];
            if ((c == '"' || c == '\\') &&
                si < (int)sizeof(s_scan_aps[0].ssid) - 2)
                s_scan_aps[s_scan_count].ssid[si++] = '\\';
            if (si < (int)sizeof(s_scan_aps[0].ssid) - 1)
                s_scan_aps[s_scan_count].ssid[si++] = c;
        }
        s_scan_aps[s_scan_count].ssid[si] = '\0';

        int rssi = recs[i].rssi;
        s_scan_aps[s_scan_count].quality =
            rssi <= -100 ? 0 : rssi >= -40 ? 100 : 2 * (rssi + 100);
        s_scan_aps[s_scan_count].secure =
            (recs[i].authmode != WIFI_AUTH_OPEN);
        s_scan_count++;
    }
    s_scan_ready = true;
    xSemaphoreGive(s_scan_mutex);

    ESP_LOGI(TAG, "scan: %d APs cached", s_scan_count);
    free(recs);
}

/* ── DNS hijack ────────────────────────────────────────────────────────────*/
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

/* ── httpd handlers ────────────────────────────────────────────────────────*/

static esp_err_t handle_root(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/setup.html", "text/html"); }

static esp_err_t handle_base_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/base.css", "text/css"); }

static esp_err_t handle_setup_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/setup.css", "text/css"); }

static esp_err_t handle_setup_js(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/setup.js", "application/javascript"); }

static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/scan — returns cached AP list immediately */
static esp_err_t handle_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (!s_scan_ready) {
        httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
        return ESP_OK;
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < s_scan_count; i++) {
        char entry[128];
        snprintf(entry, sizeof(entry),
                 "%s{\"ssid\":\"%s\",\"quality\":%d,\"secure\":%s}",
                 i ? "," : "",
                 s_scan_aps[i].ssid,
                 s_scan_aps[i].quality,
                 s_scan_aps[i].secure ? "true" : "false");
        httpd_resp_sendstr_chunk(req, entry);
    }
    xSemaphoreGive(s_scan_mutex);
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/rescan — blocking refresh, returns fresh JSON */
static esp_err_t handle_rescan(httpd_req_t *req)
{
    run_scan_once();   /* blocks httpd task ~3 s; acceptable for explicit action */
    return handle_scan(req);
}

/* ── URL decode ────────────────────────────────────────────────────────────*/
static void url_decode(const char *src, char *dst, int dst_size)
{
    int out = 0;
    while (*src && out < dst_size - 1) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[out++] = (char)strtol(hex, NULL, 16);
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
    url_decode(tmp, out, out_size);
    free(tmp);
}

/* POST /api/connect — body: ssid=<url-encoded>&password=<url-encoded> */
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

/* ── httpd lifecycle ───────────────────────────────────────────────────────*/
static httpd_handle_t s_httpd = NULL;

static void start_httpd(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 18;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed"); return;
    }

    static const httpd_uri_t routes[] = {
        { .uri="/",                          .method=HTTP_GET,  .handler=handle_root              },
        { .uri="/base.css",                  .method=HTTP_GET,  .handler=handle_base_css          },
        { .uri="/setup.css",                 .method=HTTP_GET,  .handler=handle_setup_css         },
        { .uri="/setup.js",                  .method=HTTP_GET,  .handler=handle_setup_js          },
        { .uri="/hotspot-detect.html",       .method=HTTP_GET,  .handler=handle_redirect       },
        { .uri="/library/test/success.html", .method=HTTP_GET,  .handler=handle_redirect       },
        { .uri="/connecttest.txt",           .method=HTTP_GET,  .handler=handle_redirect  },
        { .uri="/connecttest.txt",           .method=HTTP_HEAD, .handler=handle_redirect  },
        { .uri="/ncsi.txt",                  .method=HTTP_GET,  .handler=handle_redirect  },
        { .uri="/ncsi.txt",                  .method=HTTP_HEAD, .handler=handle_redirect  },
        { .uri="/redirect",                  .method=HTTP_GET,  .handler=handle_redirect},
        { .uri="/generate_204",              .method=HTTP_GET,  .handler=handle_redirect     },
        { .uri="/gen_204",                   .method=HTTP_GET,  .handler=handle_redirect     },
        { .uri="/generate_204",              .method=HTTP_GET,  .handler=handle_redirect },
        { .uri="/gen_204",                   .method=HTTP_GET,  .handler=handle_redirect },
        { .uri="/mobile/status.php",         .method=HTTP_GET,  .handler=handle_redirect }, /* Samsung */
        { .uri="/connectivitycheck.mobile.txt", .method=HTTP_GET, .handler=handle_redirect }, /* some carriers */
        { .uri="/api/scan",                  .method=HTTP_GET,  .handler=handle_scan              },
        { .uri="/api/rescan",                .method=HTTP_GET,  .handler=handle_rescan            },
        { .uri="/api/connect",               .method=HTTP_POST, .handler=handle_connect           },
        { .uri="/*",                         .method=HTTP_GET,  .handler=handle_redirect          },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_httpd, &routes[i]);

    ESP_LOGI(TAG, "Portal httpd started");
}

/* ── Public API ────────────────────────────────────────────────────────────*/

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
