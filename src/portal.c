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

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
} dns_header_t;

static void dns_reply(int sock, struct sockaddr_in *client,
                      const uint8_t *query, int qlen)
{
    if (qlen < 12 || qlen > 496) return;

    /* Only respond to queries (QR bit = 0); drop responses. */
    uint16_t flags_in;
    memcpy(&flags_in, query + 2, 2);
    if (ntohs(flags_in) & 0x8000) return;

    /* Resolve AP IP dynamically */
    uint32_t gw = inet_addr(AP_GW_IP);
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            gw = ip_info.ip.addr;
    }

    uint8_t resp[512];
    memcpy(resp, query, qlen);

    /* Patch header using proper struct — clear and set fields explicitly */
    dns_header_t *hdr = (dns_header_t *)resp;
    hdr->flags   = htons(0x8180);   /* QR=1, Opcode=0000, AA=1, TC=0, RD=1, RA=1, Z=000, RCODE=0 */
    hdr->answers = htons(1);         /* One A-record answer */
    hdr->authority  = htons(0);
    hdr->additional = htons(0);

    /* Append A-record answer at end of query */
    int pos = qlen;

    /* NAME: compressed pointer to question name at offset 12 */
    resp[pos++] = 0xC0;
    resp[pos++] = 0x0C;

    /* TYPE A (1), CLASS IN (1) */
    resp[pos++] = 0x00; resp[pos++] = 0x01;
    resp[pos++] = 0x00; resp[pos++] = 0x01;

    /* TTL: 300 seconds (big-endian) */
    resp[pos++] = 0x00; resp[pos++] = 0x00;
    resp[pos++] = 0x01; resp[pos++] = 0x2C;

    /* RDLENGTH: 4 bytes */
    resp[pos++] = 0x00; resp[pos++] = 0x04;

    /* RDATA: IPv4 address (network byte order — already in .addr) */
    memcpy(resp + pos, &gw, 4);
    pos += 4;

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

        if (n < 0) {
            /* Socket closed by stop_dns() or timeout — check if we're stopping */
            if (!s_dns_run) break;
            if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
            /* Other error (e.g. EBADF after close) — exit task */
            ESP_LOGW(TAG, "DNS recvfrom error %d, exiting", errno);
            break;
        }
        if (n > 0) dns_reply(s_dns_sock, &client, buf, n);
    }

    s_dns_run = false;  /* Signal clean exit */
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

    /* Close socket FIRST to unblock recvfrom() immediately with EBADF/EINTR */
    int sock = s_dns_sock;
    s_dns_sock = -1;
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);  /* Unblock recvfrom */
        close(sock);
    }

    /* Brief delay for task self-deletion */
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ── DHCP Option 114 — RFC 8910 captive-portal URI advertisement ─────────────
 *
 * Injects the portal URL into DHCP offers/acks so that RFC-8910-aware clients
 * (modern iOS, Android 11+) display the "Sign in to network" prompt immediately
 * on association, without waiting for HTTP probe heuristics.
 *
 * Called once from portal_start() after the AP netif is up.
 */
static void set_dhcp_option_114(void)
{
    /* Build the portal URI string once. */
    char uri[64];
    snprintf(uri, sizeof(uri), "http://" AP_GW_IP "/");

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ESP_LOGW(TAG, "DHCP opt114: AP netif not found, skipping");
        return;
    }

    /* Stop DHCP server temporarily to apply options, then restart. */
    esp_netif_dhcps_stop(ap_netif);

    /* Option 114 value: length-prefixed string as expected by esp_netif. */
    uint8_t opt_buf[65];
    size_t  uri_len = strlen(uri);
    if (uri_len > 63) uri_len = 63;
    opt_buf[0] = (uint8_t)uri_len;
    memcpy(opt_buf + 1, uri, uri_len);

    esp_err_t err = esp_netif_dhcps_option(
        ap_netif,
        ESP_NETIF_OP_SET,
        ESP_NETIF_CAPTIVEPORTAL_URI,   /* DHCP option 114 */
        opt_buf,
        (uint32_t)(uri_len + 1)
    );
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP opt114 set failed: %s (non-fatal)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DHCP Option 114 set → %s", uri);
    }

    esp_netif_dhcps_start(ap_netif);
}



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

/* ── Windows NCSI probe ──────────────────────────────────────────────────
 * Windows expects 200 + "Microsoft Connect Test" for real internet.
 * For captive portal: return 302 redirect, BUT the response must be
 * valid enough that NCSI's "ActiveHttpProbeFailedHotspotDetected" fires.
 */
static esp_err_t handle_connecttest(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Probe: Windows NCSI %s", req->uri);

    /* Try to serve setup.html from LittleFS first */
    const char *setup_path = FS_BASE "/setup.html";
    FILE *f = fopen(setup_path, "r");
    if (f) {
        /* Get file size */
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (fsize > 0 && fsize < 16384) {  /* Sanity check */
            char *buf = malloc(fsize + 1);
            if (buf) {
                size_t n = fread(buf, 1, fsize, f);
                fclose(f);

                if (n == (size_t)fsize) {
                    buf[fsize] = '\0';

                    httpd_resp_set_status(req, "200 OK");
                    httpd_resp_set_type(req, "text/html");
                    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
                    httpd_resp_set_hdr(req, "Pragma", "no-cache");
                    httpd_resp_set_hdr(req, "Connection", "close");

                    httpd_resp_sendstr(req, buf);
                    free(buf);

                    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
                    return ESP_OK;
                }
                free(buf);
            } else {
                fclose(f);
            }
        } else {
            fclose(f);
        }
    }

    /* Fallback: inline minimal portal HTML if file read fails */
    ESP_LOGW(TAG, "Failed to read %s, using fallback HTML", setup_path);

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    const char *fallback =
        "<!DOCTYPE html>"
        "<html><head>"
        "<meta charset=\"UTF-8\">"
        "<title>Robot Setup</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<style>"
        "body{font-family:system-ui,sans-serif;max-width:400px;margin:2rem auto;padding:0 1rem}"
        "input{width:100%;padding:.5rem;margin:.5rem 0;box-sizing:border-box}"
        "button{width:100%;padding:.75rem;background:#0078d4;color:#fff;border:none;cursor:pointer}"
        "</style></head><body>"
        "<h1>Robot WiFi Setup</h1>"
        "<form action=\"/api/connect\" method=\"POST\">"
        "<label>SSID:<input type=\"text\" name=\"ssid\" required></label>"
        "<label>Password:<input type=\"password\" name=\"password\"></label>"
        "<button type=\"submit\">Connect</button>"
        "</form>"
        "<p><a href=\"/api/scan\">Scan for networks</a></p>"
        "</body></html>";

    httpd_resp_sendstr(req, fallback);
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return ESP_OK;
}

/* ── Android/Google probe ────────────────────────────────────────────────
 * Android expects 204 No Content for real internet.
 * For captive portal: ANY non-204 status triggers detection.
 * But Android 10+ requires the response to be "followable" —
 * a 302 with Location header should work, but some OEMs (Samsung)
 * need a 200 with portal HTML to auto-launch the captive portal app.
 */
static esp_err_t handle_generate_204(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Probe: Android %s", req->uri);

    /* Return 302 redirect — CaptivePortalLogin follows this automatically */
    char url[64];
    snprintf(url, sizeof(url), "http://" AP_GW_IP "/");

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    /* Minimal body — some Android versions check content-length */
    httpd_resp_sendstr(req, "<html><body>Redirecting...</body></html>");
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return ESP_OK;
}

/* ── Generic catch-all (iOS, Firefox, Chrome, unknown) ─────────────────── */
static esp_err_t handle_generic(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Probe: Generic %s", req->uri);

    char url[64];
    snprintf(url, sizeof(url), "http://" AP_GW_IP "/");

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    /* Non-empty body required — empty body causes iOS/Android to ignore */
    const char *body =
        "<!DOCTYPE html><html><head>"
        "<meta http-equiv=\"refresh\" content=\"0;url=%s\">"
        "<title>Redirecting</title></head><body>"
        "<p>Redirecting to <a href=\"%s\">captive portal</a>...</p>"
        "<script>window.location.replace('%s');</script>"
        "</body></html>";

    char html[512];
    snprintf(html, sizeof(html), body, url, url, url);
    httpd_resp_sendstr(req, html);

    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
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
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "Saved. Connecting...");
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));

    xEventGroupSetBits(s_event_group, CREDS_RECEIVED_BIT);
    return ESP_OK;
}

/* ── httpd lifecycle ─────────────────────────────────────────────────────── */
static httpd_handle_t s_httpd = NULL;

static void start_httpd(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 36;
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;

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

        /* === OS-SPECIFIC CAPTIVE PORTAL PROBES === */
        /* Android — MUST be before wildcard. Returns 200 (not 302) for /generate_204 */
        { .uri="/generate_204",  .method=HTTP_GET,  .handler=handle_generate_204 },
        { .uri="/generate204",   .method=HTTP_GET,  .handler=handle_generate_204 },
        { .uri="/gen_204",       .method=HTTP_GET,  .handler=handle_generate_204 },

        /* Windows NCSI — MUST be before wildcard */
        { .uri="/connecttest.txt", .method=HTTP_GET,  .handler=handle_connecttest },
        { .uri="/ncsi.txt",        .method=HTTP_GET,  .handler=handle_connecttest },

        /* iOS/macOS */
        { .uri="/hotspot-detect.html",       .method=HTTP_GET,  .handler=handle_generic },
        { .uri="/library/test/success.html", .method=HTTP_GET,  .handler=handle_generic },

        /* Firefox */
        { .uri="/success.txt", .method=HTTP_GET,  .handler=handle_generic },
        { .uri="/canonical.html", .method=HTTP_GET, .handler=handle_generic },

        /* Chrome/Chromium network time */
        { .uri="/browsernetworktime/*", .method=HTTP_GET, .handler=handle_generic },
        /* In your routes array, add /redirect */
        { .uri="/redirect",        .method=HTTP_GET,  .handler=handle_connecttest },

        /* Wildcard catch-all — MUST BE LAST */
        { .uri="/*", .method=HTTP_GET,  .handler=handle_generic  },
        { .uri="/*", .method=HTTP_HEAD, .handler=handle_generic  },
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
    set_dhcp_option_114(); /* RFC 8910: advertise portal URI in DHCP offers */

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
