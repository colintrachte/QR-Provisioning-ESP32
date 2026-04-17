/**
 * portal.c — SoftAP captive portal.
 *
 * DNS hijack
 * ──────────
 * A raw UDP socket on port 53 answers every A-record query with AP_GW_IP.
 * SO_RCVTIMEO of 1 s lets dns_task check s_dns_run and exit cleanly
 * when portal_stop() sets the flag and closes the socket.
 *
 * OS captive-portal probes
 * ────────────────────────
 *   Windows:  GET /connecttest.txt   → "Microsoft Connect Test"
 *             GET /ncsi.txt          → "Microsoft NCSI"
 *             GET /redirect          → 302 to msftconnecttest.com
 *   iOS:      GET /hotspot-detect.html → Success HTML
 *             GET /library/test/success.html
 *   Android:  GET /generate_204, /gen_204 → 204
 *
 * Catch-all: any unrecognised URI → 302 to http://AP_GW_IP/
 * Requires cfg.uri_match_fn = httpd_uri_match_wildcard so matches last.
 *
 * Network scan
 * ────────────
 * scan_task runs in the background and refreshes the AP list every
 * SCAN_INTERVAL_MS. GET /api/scan returns the cached result immediately —
 * it never blocks the httpd task for the ~3 s a live scan takes.
 * The scan_task pauses while a STA connection attempt is in progress.
 *
 * File serving
 * ────────────
 * Uses a serve_file() helper called from explicit named route handlers.
 * Dynamic URI→path concatenation is deliberately avoided: httpd URIs can be
 * up to CONFIG_HTTPD_MAX_URI_LEN (512 B) and would overflow any fixed buffer
 * when prepending the "/littlefs" base path.
 */

#include "portal.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "portal";

/* ── LittleFS ───────────────────────────────────────────────────────────────
 * Partition label must match partitions.csv — original used "storage".
 */
#define FS_BASE  "/littlefs"
#define FS_PART  "storage"
#define SERVE_CHUNK 1024

static void fs_mount(void)
{
    static bool s_mounted = false;
    if (s_mounted) return;
    esp_vfs_littlefs_conf_t cfg = {
        .base_path              = FS_BASE,
        .partition_label        = FS_PART,
        .format_if_mount_failed = true,   /* don't silently fail on blank flash */
    };
    if (esp_vfs_littlefs_register(&cfg) == ESP_OK)
        s_mounted = true;
    else
        ESP_LOGE(TAG, "LittleFS mount failed (run: pio run -t uploadfs)");
}

/* ── File serving helper ────────────────────────────────────────────────────
 * "rb" mode: LittleFS stores files with LF endings; text mode would corrupt
 * byte counts on platforms that translate line endings.
 */
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

/* ── Credentials ────────────────────────────────────────────────────────────*/
static char s_ssid[33] = {0};
static char s_pass[65] = {0};

/* ── Synchronisation ────────────────────────────────────────────────────────*/
#define CREDS_RECEIVED_BIT BIT0
static EventGroupHandle_t s_event_group = NULL;

/* ── Background scan task ───────────────────────────────────────────────────*/
#define SCAN_MAX_APS     20
#define SCAN_INTERVAL_MS 12000

typedef struct {
    char ssid[64];   /* JSON-escaped, safe for direct embedding */
    int  quality;    /* 0–100 derived from RSSI                 */
    bool secure;
} ap_cache_t;

static SemaphoreHandle_t  s_scan_mutex  = NULL;
static ap_cache_t         s_scan_aps[SCAN_MAX_APS];
static int                s_scan_count  = 0;
static bool               s_scan_ready  = false;
static volatile bool      s_scan_run    = false;

/* Written by wifi_manager event handler, read by scan_task to avoid
 * scanning while a STA connection attempt is in progress. */
volatile int portal_retry_count = 0;

static void scan_task(void *arg)
{
    (void)arg;
    while (s_scan_run) {
        while (portal_retry_count > 0 && s_scan_run)
            vTaskDelay(pdMS_TO_TICKS(500));
        if (!s_scan_run) break;

        wifi_scan_config_t sc = {
            .show_hidden = false,
            .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        };
        if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        uint16_t count = SCAN_MAX_APS;
        wifi_ap_record_t *recs = calloc(SCAN_MAX_APS, sizeof(wifi_ap_record_t));
        if (!recs) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
        esp_wifi_scan_get_ap_records(&count, recs);

        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
        s_scan_count = 0;
        for (int i = 0; i < (int)count && s_scan_count < SCAN_MAX_APS; i++) {
            if (recs[i].ssid[0] == '\0') continue;

            /* JSON-escape " and \ so the SSID is safe inside a JSON string.
             * The output buffer is 64 bytes; a 32-char SSID with every char
             * escaped needs at most 64 bytes + null — exactly fits. */
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

        ESP_LOGI(TAG, "scan_task: %d APs cached", s_scan_count);
        free(recs);
        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
    vTaskDelete(NULL);
}

/* ── DNS hijack ─────────────────────────────────────────────────────────────*/
static int           s_dns_sock = -1;
static volatile bool s_dns_run  = false;

static void dns_reply(int sock, struct sockaddr_in *client,
                      const uint8_t *query, int qlen)
{
    if (qlen < 12 || qlen > 496) return;   /* 496 + 16 answer = 512 max */

    uint8_t  resp[512];
    uint32_t gw = inet_addr(AP_GW_IP);
    memcpy(resp, query, qlen);
    resp[2] = 0x81; resp[3] = 0x80;            /* QR=1 RA=1, RCODE=0    */
    resp[6] = 0x00; resp[7] = 0x01;            /* ANCOUNT = 1           */
    resp[8] = resp[9] = resp[10] = resp[11] = 0;

    int pos = qlen;
    resp[pos++] = 0xC0; resp[pos++] = 0x0C;   /* name ptr to offset 12 */
    resp[pos++] = 0x00; resp[pos++] = 0x01;   /* TYPE A                */
    resp[pos++] = 0x00; resp[pos++] = 0x01;   /* CLASS IN              */
    resp[pos++] = 0x00; resp[pos++] = 0x00;
    resp[pos++] = 0x00; resp[pos++] = 0x3C;   /* TTL 60 s              */
    resp[pos++] = 0x00; resp[pos++] = 0x04;   /* RDLENGTH              */
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
        /* SO_RCVTIMEO = 1 s — loops here to check s_dns_run */
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
    xTaskCreate(dns_task, "portal_dns", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "DNS hijack → " AP_GW_IP);
}

static void stop_dns(void)
{
    s_dns_run = false;
    if (s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    vTaskDelay(pdMS_TO_TICKS(150));   /* let dns_task wake from SO_RCVTIMEO and exit */
}

/* ── httpd handlers ─────────────────────────────────────────────────────────*/

static esp_err_t handle_root(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/setup.html", "text/html"); }

static esp_err_t handle_setup_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/setup.css", "text/css"); }

static esp_err_t handle_setup_js(httpd_req_t *req)
    { return serve_file(req, FS_BASE "/setup.js", "application/javascript"); }

static esp_err_t handle_probe_apple(httpd_req_t *req)
{
    /* iOS/macOS expects <TITLE>Success</TITLE> to consider the portal detected.
     * The JS redirect ensures the browser then opens the setup page. */
    static const char BODY[] =
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
        "<BODY><script>window.location.replace('http://" AP_GW_IP "/');</script>"
        "</BODY></HTML>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, BODY, sizeof(BODY) - 1);
    return ESP_OK;
}

static esp_err_t handle_probe_win_modern(httpd_req_t *req)
    { httpd_resp_set_type(req, "text/plain");
      httpd_resp_sendstr(req, "Microsoft Connect Test"); return ESP_OK; }

static esp_err_t handle_probe_win_legacy(httpd_req_t *req)
    { httpd_resp_set_type(req, "text/plain");
      httpd_resp_sendstr(req, "Microsoft NCSI"); return ESP_OK; }

static esp_err_t handle_probe_win_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location",
                       "http://www.msftconnecttest.com/redirect");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_probe_android(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Catch-all: redirect any unrecognised URI to the setup page.
 * Registered as wildcard requires httpd_uri_match_wildcard in httpd config. */
static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* GET /api/scan — returns cached AP list; never blocks the httpd task */
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
        /* Worst-case entry: 1(,)+1({)+9("ssid":")+64(ssid)+1(")+
         *   12(,"quality":)+3(100)+12(,"secure":)+5(false)+1(}) = 109 bytes.
         * 128 bytes is comfortable. */
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

/* ── URL decode ─────────────────────────────────────────────────────────────*/
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

    /* Allocate a temp buffer for the raw (still URL-encoded) field value.
     * URL encoding expands each byte to at most 3 chars, so the decoded
     * result always fits in out_size. The temp buffer only needs to hold
     * the raw encoded bytes, which is at most len+1. */
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

/* ── httpd lifecycle ────────────────────────────────────────────────────────*/
static httpd_handle_t s_httpd = NULL;

static void start_httpd(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;  /* required for wildcard */

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed"); return;
    }

    /* Routes are checked in registration order. Specific paths first,
     * wildcard catch-all last. HEAD variants for Windows NCSI probes
     * must be registered separately — httpd matches method exactly. */
    static const httpd_uri_t routes[] = {
        { .uri="/",                          .method=HTTP_GET,  .handler=handle_root              },
        { .uri="/setup.css",                 .method=HTTP_GET,  .handler=handle_setup_css         },
        { .uri="/setup.js",                  .method=HTTP_GET,  .handler=handle_setup_js          },
        { .uri="/hotspot-detect.html",       .method=HTTP_GET,  .handler=handle_probe_apple       },
        { .uri="/library/test/success.html", .method=HTTP_GET,  .handler=handle_probe_apple       },
        { .uri="/connecttest.txt",           .method=HTTP_GET,  .handler=handle_probe_win_modern  },
        { .uri="/connecttest.txt",           .method=HTTP_HEAD, .handler=handle_probe_win_modern  },
        { .uri="/ncsi.txt",                  .method=HTTP_GET,  .handler=handle_probe_win_legacy  },
        { .uri="/ncsi.txt",                  .method=HTTP_HEAD, .handler=handle_probe_win_legacy  },
        { .uri="/redirect",                  .method=HTTP_GET,  .handler=handle_probe_win_redirect},
        { .uri="/generate_204",              .method=HTTP_GET,  .handler=handle_probe_android     },
        { .uri="/gen_204",                   .method=HTTP_GET,  .handler=handle_probe_android     },
        { .uri="/api/scan",                  .method=HTTP_GET,  .handler=handle_scan              },
        { .uri="/api/connect",               .method=HTTP_POST, .handler=handle_connect           },
        { .uri="/*",                         .method=HTTP_GET,  .handler=handle_redirect          },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_httpd, &routes[i]);

    ESP_LOGI(TAG, "Portal httpd started");
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

void portal_start(const char *ap_ssid)
{
    (void)ap_ssid;

    s_event_group = xEventGroupCreate();
    if (!s_scan_mutex) s_scan_mutex = xSemaphoreCreateMutex();

    fs_mount();
    start_httpd();
    start_dns();

    s_scan_run = true;
    xTaskCreate(scan_task, "portal_scan", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "Portal running at http://" AP_GW_IP "/");
    xEventGroupWaitBits(s_event_group, CREDS_RECEIVED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "Credentials received — portal unblocking");
}

void portal_stop(void)
{
    s_scan_run = false;
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
