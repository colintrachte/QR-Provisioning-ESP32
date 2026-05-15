/**
 * app_server.c — Robot control HTTP server, WebSocket dispatcher,
 *                and captive-portal host.
 *
 * Single-server architecture
 * ──────────────────────────
 * app_server owns port 80 from the moment app_server_start() is called
 * (at boot, before wifi_manager).  It never stops and never shares the
 * port with portal.c.
 *
 * Two operating modes
 * ───────────────────
 *  NORMAL  — only control-page routes and WebSocket are registered.
 *            This is the steady state once STA is connected.
 *
 *  PROVISIONING — entered by app_server_enter_provisioning_mode():
 *            • Setup-page asset routes  (/setup, /setup.css, /setup.js)
 *            • WiFi API  (/api/scan, /api/rescan, /api/connect)
 *            • Captive-portal probe handlers (generate_204, ncsi.txt …)
 *            • DNS hijack task (port 53 → AP GW)
 *            • DHCP option 114
 *            Exited by app_server_exit_provisioning_mode() which
 *            unregisters those routes and kills the DNS task.
 *
 * Control routes (/, /index.js, /base.css, /index.css, /ws, /api) are
 * always registered and work on both AP and STA interfaces.  A browser
 * connected to the SoftAP that navigates to / gets the control page
 * immediately — no handoff or port-split required.
 *
 * File serving
 * ────────────
 * All assets are embedded in flash (EMBED_FILES in CMakeLists.txt).
 * Explicit named routes only — no dynamic URI→path concatenation.
 * A wildcard catch-all returns 404 for any unregistered URI.
 *
 * JSON
 * ────
 * All JSON is built with cJSON.  vfs_build_status_json() /
 * vfs_build_scan_json() (utils_json.c) are shared with the old portal.
 */

#include "app_server.h"
#include "ctrl_drive.h"
#include "o_led.h"
#include "health_monitor.h"
#include "prov_ui.h"
#include "ota_server.h"
#include "settings_server.h"
#include "utils_filesystem.h"
#include "utils_web.h"
#include "utils_json.h"
#include "wifi_manager.h"
#include "config.h"
#include "nvs_keys.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "app_server";

/* ── Embedded assets ────────────────────────────────────────────────────── */
/* Control page */
extern const uint8_t index_html_gz_start[]    asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]      asm("_binary_index_html_gz_end");
extern const uint8_t index_js_gz_start[]      asm("_binary_index_js_gz_start");
extern const uint8_t index_js_gz_end[]        asm("_binary_index_js_gz_end");
extern const uint8_t index_css_gz_start[]     asm("_binary_index_css_gz_start");
extern const uint8_t index_css_gz_end[]       asm("_binary_index_css_gz_end");
extern const uint8_t base_css_gz_start[]      asm("_binary_base_css_gz_start");
extern const uint8_t base_css_gz_end[]        asm("_binary_base_css_gz_end");
/* Settings page */
extern const uint8_t settings_html_gz_start[] asm("_binary_settings_html_gz_start");
extern const uint8_t settings_html_gz_end[]   asm("_binary_settings_html_gz_end");
extern const uint8_t settings_js_gz_start[]   asm("_binary_settings_js_gz_start");
extern const uint8_t settings_js_gz_end[]     asm("_binary_settings_js_gz_end");
extern const uint8_t settings_css_gz_start[]  asm("_binary_settings_css_gz_start");
extern const uint8_t settings_css_gz_end[]    asm("_binary_settings_css_gz_end");
/* Setup / provisioning page */
extern const uint8_t setup_html_gz_start[]    asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_gz_end[]      asm("_binary_setup_html_gz_end");
extern const uint8_t setup_js_gz_start[]      asm("_binary_setup_js_gz_start");
extern const uint8_t setup_js_gz_end[]        asm("_binary_setup_js_gz_end");
extern const uint8_t setup_css_gz_start[]     asm("_binary_setup_css_gz_start");
extern const uint8_t setup_css_gz_end[]       asm("_binary_setup_css_gz_end");
/* Shared */
extern const uint8_t favicon_svg_gz_start[]   asm("_binary_favicon_svg_gz_start");
extern const uint8_t favicon_svg_gz_end[]     asm("_binary_favicon_svg_gz_end");

/* ── Serve-embedded macro ───────────────────────────────────────────────── */
#define SERVE_EMBEDDED(req, mime, start_sym, end_sym, is_gz, is_asset)     \
    do {                                                                    \
        httpd_resp_set_type((req), (mime));                                 \
        if (is_gz)                                                          \
            httpd_resp_set_hdr((req), "Content-Encoding", "gzip");         \
        httpd_resp_set_hdr((req), "Cache-Control",                         \
            (is_asset) ? "public, max-age=3600" : "no-cache");             \
        httpd_resp_set_hdr((req), "Access-Control-Allow-Origin", "*");     \
        httpd_resp_send((req), (const char *)(start_sym),                  \
                        (ssize_t)((end_sym) - (start_sym)));               \
    } while (0)

/* ── Server handle ──────────────────────────────────────────────────────── */
static httpd_handle_t s_httpd = NULL;

/* ── Provisioning mode state ────────────────────────────────────────────── */
static volatile bool          s_provisioning      = false;
static app_server_creds_cb_t  s_creds_cb          = NULL;

/* ── DNS hijack (captive portal) ────────────────────────────────────────── */
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

    uint16_t flags_in;
    memcpy(&flags_in, query + 2, 2);
    if (ntohs(flags_in) & 0x8000) return;  /* drop responses */

    /* Resolve AP GW IP dynamically */
    uint32_t gw = inet_addr(AP_GW_IP);
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            gw = ip_info.ip.addr;
    }

    uint8_t resp[512];
    memcpy(resp, query, qlen);

    dns_header_t *hdr = (dns_header_t *)resp;
    hdr->flags      = htons(0x8180);
    hdr->answers    = htons(1);
    hdr->authority  = htons(0);
    hdr->additional = htons(0);

    int pos = qlen;
    resp[pos++] = 0xC0; resp[pos++] = 0x0C;  /* compressed name ptr */
    resp[pos++] = 0x00; resp[pos++] = 0x01;  /* TYPE A  */
    resp[pos++] = 0x00; resp[pos++] = 0x01;  /* CLASS IN */
    resp[pos++] = 0x00; resp[pos++] = 0x00;
    resp[pos++] = 0x01; resp[pos++] = 0x2C;  /* TTL 300 s */
    resp[pos++] = 0x00; resp[pos++] = 0x04;  /* RDLENGTH 4 */
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
        if (n < 0) {
            if (!s_dns_run) break;
            if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
            ESP_LOGW(TAG, "DNS recvfrom error %d, exiting", errno);
            break;
        }
        if (n > 0) dns_reply(s_dns_sock, &client, buf, n);
    }

    s_dns_run = false;
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
    xTaskCreate(dns_task, "app_dns", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "DNS hijack active → " AP_GW_IP);
}

static void stop_dns(void)
{
    s_dns_run = false;
    int sock = s_dns_sock;
    s_dns_sock = -1;
    if (sock >= 0) { shutdown(sock, SHUT_RDWR); close(sock); }
    vTaskDelay(pdMS_TO_TICKS(120));  /* let task self-delete */
}

/* ── DHCP option 114 (RFC 8910 captive-portal URI) ─────────────────────── */
static void set_dhcp_option_114(void)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "http://" AP_GW_IP "/");

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) return;

    esp_netif_dhcps_stop(ap_netif);

    uint8_t opt_buf[65];
    size_t uri_len = strlen(uri);
    if (uri_len > 63) uri_len = 63;
    opt_buf[0] = (uint8_t)uri_len;
    memcpy(opt_buf + 1, uri, uri_len);

    esp_err_t err = esp_netif_dhcps_option(
        ap_netif, ESP_NETIF_OP_SET,
        ESP_NETIF_CAPTIVEPORTAL_URI,
        opt_buf, (uint32_t)(uri_len + 1));
    if (err != ESP_OK)
        ESP_LOGW(TAG, "DHCP opt114 set failed: %s (non-fatal)", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "DHCP option 114 → %s", uri);

    esp_netif_dhcps_start(ap_netif);
}

/* ── JSON helper ────────────────────────────────────────────────────────── */
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

/* ════════════════════════════════════════════════════════════════════════════
 * CONTROL PAGE HANDLERS — always registered
 * ════════════════════════════════════════════════════════════════════════════*/

/* GET /
 * During provisioning: setup page (captive portal lands here).
 * After provisioning exits: control page. */
static esp_err_t handle_index(httpd_req_t *req)
{
    if (s_provisioning) {
        SERVE_EMBEDDED(req, "text/html",
                       setup_html_gz_start, setup_html_gz_end, true, false);
    } else {
        SERVE_EMBEDDED(req, "text/html",
                       index_html_gz_start, index_html_gz_end, true, false);
    }
    return ESP_OK;
}

/* GET /index.html — explicit control-page entry point.
 * Always serves the control page regardless of provisioning state.
 * A user on the SoftAP can navigate here while STA is still connecting. */
static esp_err_t handle_index_html(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "text/html",
                   index_html_gz_start, index_html_gz_end, true, false);
    return ESP_OK;
}

static esp_err_t handle_base_css(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "text/css", base_css_gz_start, base_css_gz_end, true, true);
    return ESP_OK;
}
static esp_err_t handle_index_css(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "text/css", index_css_gz_start, index_css_gz_end, true, true);
    return ESP_OK;
}
static esp_err_t handle_index_js(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "application/javascript",
                   index_js_gz_start, index_js_gz_end, true, true);
    return ESP_OK;
}
static esp_err_t handle_settings(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "text/html",
                   settings_html_gz_start, settings_html_gz_end, true, false);
    return ESP_OK;
}
static esp_err_t handle_settings_css(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "text/css",
                   settings_css_gz_start, settings_css_gz_end, true, true);
    return ESP_OK;
}
static esp_err_t handle_settings_js(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "application/javascript",
                   settings_js_gz_start, settings_js_gz_end, true, true);
    return ESP_OK;
}
static esp_err_t handle_favicon(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "image/svg+xml",
                   favicon_svg_gz_start, favicon_svg_gz_end, true, true);
    return ESP_OK;
}

static esp_err_t handle_404(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

static esp_err_t handle_api_reboot(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_api_erase(httpd_req_t *req)
{
    wifi_manager_erase_credentials();
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_api_jserror(httpd_req_t *req)
{
    char body[256] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n > 0) { body[n] = '\0'; ESP_LOGW(TAG, "JS error: %s", body); }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* GET /api/status — used by both control page and setup page */
static esp_err_t handle_api_status(httpd_req_t *req)
{
    return send_json(req, vfs_build_status_json());
}

/* ════════════════════════════════════════════════════════════════════════════
 * PROVISIONING HANDLERS — registered only while s_provisioning == true
 * ════════════════════════════════════════════════════════════════════════════*/

/* Setup page assets */
static esp_err_t handle_setup_html(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "text/html",
                   setup_html_gz_start, setup_html_gz_end, true, false);
    return ESP_OK;
}
static esp_err_t handle_setup_css(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "text/css",
                   setup_css_gz_start, setup_css_gz_end, true, true);
    return ESP_OK;
}
static esp_err_t handle_setup_js(httpd_req_t *req)
{
    SERVE_EMBEDDED(req, "application/javascript",
                   setup_js_gz_start, setup_js_gz_end, true, true);
    return ESP_OK;
}

/* ── WiFi scan ──────────────────────────────────────────────────────────── */
#define MAX_SCAN_APS  20
static SemaphoreHandle_t  s_scan_mutex   = NULL;
static wifi_ap_record_t   s_scan_recs[MAX_SCAN_APS];
static int                s_scan_count   = 0;
static bool               s_scan_ready   = false;
static volatile bool      s_scan_running = false;
static EventGroupHandle_t s_scan_evt     = NULL;
static bool               s_events_registered = false;
#define SCAN_DONE_BIT  BIT0

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
        s_scan_evt, SCAN_DONE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(6000));

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
        ESP_LOGW(TAG, "Scan records failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_scan_mutex);

    s_scan_running = false;
    vTaskDelete(NULL);
}

/* Blocking scan — used for the very first scan in portal mode when no
 * client is associated yet (safe to block the caller task briefly). */
static void run_scan_once_blocking(void)
{
    wifi_scan_config_t sc = { .show_hidden = false,
                              .scan_type   = WIFI_SCAN_TYPE_ACTIVE };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;

    uint16_t count = MAX_SCAN_APS;
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_scan_recs);
    if (err == ESP_OK) { s_scan_count = (int)count; s_scan_ready = true; }
    xSemaphoreGive(s_scan_mutex);
    ESP_LOGI(TAG, "Initial scan: %d APs", s_scan_count);
}

static void trigger_scan(void)
{
    if (!s_scan_running)
        xTaskCreate(scan_task, "app_scan", 4096, NULL, 3, NULL);
}

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

static esp_err_t handle_api_rescan(httpd_req_t *req)
{
    trigger_scan();
    return handle_api_scan(req);
}

/* POST /api/connect — called by setup page; also available from settings
 * page while STA-connected (to switch networks without re-flashing). */
static esp_err_t handle_api_connect(httpd_req_t *req)
{
    char body[320] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_OK;
    }
    body[n] = '\0';
    ESP_LOGI(TAG, "POST /api/connect body: '%s'", body);

    char ssid_enc[128] = {0}, pass_enc[128] = {0};
    char ssid[64]      = {0}, pass[64]      = {0};

    /* Simple form-field extraction (same pattern as before) */
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

    ESP_LOGI(TAG, "Decoded SSID='%s'", ssid);

    /* Always persist immediately — the user typed these intentionally.
     * wifi_manager will read them on the next connect attempt. */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_WIFI, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_SSID, ssid);
        nvs_set_str(nvs, NVS_KEY_PASS, pass);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Credentials saved to NVS: SSID='%s'", ssid);
    } else {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
        return ESP_OK;
    }

    /* If we're in provisioning mode, notify portal.c so mgr_task unblocks.
     * If called from the settings page while already connected, just reboot
     * so the new credentials take effect. */
    if (s_provisioning && s_creds_cb) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Saved. Connecting...");
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
        s_creds_cb(ssid, pass);  /* unblocks portal_start() */
    } else {
        /* Settings-page network switch while already connected */
        httpd_resp_sendstr(req, "Saved — rebooting");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    return ESP_OK;
}

/* ── Internal serve helper ──────────────────────────────────────────────── */
static esp_err_t serve_embedded_internal(httpd_req_t *req, const char *mime,
                                         const uint8_t *start, const uint8_t *end,
                                         bool is_gz, bool is_asset)
{
    httpd_resp_set_type(req, mime);
    if (is_gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control",
                       is_asset ? "public, max-age=3600" : "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, (const char *)start, (ssize_t)(end - start));
    return ESP_OK;
}

/* ── Captive portal probes ──────────────────────────────────────────────── */

/* Returns 200 + setup page body.  Most captive-portal detectors accept a
 * non-empty 200 as "portal present" and open the sign-in sheet. */
static esp_err_t handle_captive_generic(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Captive probe: %s", req->uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t err = serve_embedded_internal(req, "text/html",
        setup_html_gz_start, setup_html_gz_end, true, false);
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return err;
}

/* Android /generate_204 — 302 to AP IP tells Android it's a captive portal
 * and triggers the "Sign in to network" notification. */
static esp_err_t handle_captive_android(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Android captive probe: %s", req->uri);
    char url[64];
    snprintf(url, sizeof(url), "http://" AP_GW_IP "/");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "<html><body>Redirecting...</body></html>");
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════════════
 * WebSocket
 * ════════════════════════════════════════════════════════════════════════════*/

#define MAX_WS_CLIENTS 4
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
        ctrl_drive_feed_watchdog(); ctrl_drive_set_armed(true);
        o_led_blink(LED_PATTERN_HEARTBEAT); return;
    }
    if (strcmp(msg, "disarm") == 0) {
        ctrl_drive_feed_watchdog(); ctrl_drive_set_armed(false);
        o_led_blink(LED_PATTERN_SLOW_BLINK); return;
    }
    if (strcmp(msg, "stop") == 0) {
        ctrl_drive_feed_watchdog(); ctrl_drive_set_axes(0.0f, 0.0f); return;
    }
    if (strncmp(msg, "led:", 4) == 0) {
        ctrl_drive_feed_watchdog(); o_led_set(strtof(msg + 4, NULL)); return;
    }
    ESP_LOGD(TAG, "Unknown WS text: %s", msg);
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
        if (err != ESP_OK) return err;

        switch (bin[0]) {
        case 0x01:  /* WS_MSG_AXES */
            if (frame.len >= 5) {
                int16_t x_raw, y_raw;
                memcpy(&x_raw, &bin[1], 2);
                memcpy(&y_raw, &bin[3], 2);
                ctrl_drive_set_axes((float)x_raw / 32767.0f,
                                    (float)y_raw / 32767.0f);
                ctrl_drive_feed_watchdog();
            }
            break;
        case 0x02:  /* WS_MSG_ARM */
            if (frame.len >= 2) {
                bool arm = (bin[1] == 0x01);
                ctrl_drive_set_armed(arm);
                if (arm) { ctrl_drive_feed_watchdog();
                           o_led_blink(LED_PATTERN_HEARTBEAT); }
                else      { o_led_blink(LED_PATTERN_SLOW_BLINK); }
                app_server_push_arm_state(arm, NULL);
                ESP_LOGI(TAG, "Drive: %s", arm ? "ARMED" : "DISARMED");
            }
            break;
        }
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

/* ════════════════════════════════════════════════════════════════════════════
 * Route tables
 * ════════════════════════════════════════════════════════════════════════════*/

/* Always-on routes.  Registered once at app_server_start().
 *
 * Setup-page assets (/setup.css, /setup.js) and scan APIs (/api/scan,
 * /api/rescan) are in this table rather than s_prov_routes because the
 * browser requests them the moment it receives setup.html — before
 * enter_provisioning_mode() has necessarily finished registering the
 * provisioning-only routes.  Serving them from here prevents the race
 * that caused an unstyled page and empty network list. */
static const httpd_uri_t s_ctrl_routes[] = {
    { .uri="/",             .method=HTTP_GET,  .handler=handle_index        },
    { .uri="/index.html",   .method=HTTP_GET,  .handler=handle_index_html   },
    { .uri="/base.css",     .method=HTTP_GET,  .handler=handle_base_css     },
    { .uri="/index.css",    .method=HTTP_GET,  .handler=handle_index_css    },
    { .uri="/index.js",     .method=HTTP_GET,  .handler=handle_index_js     },
    /* Setup page assets — always registered so they load the instant
     * setup.html is served, regardless of provisioning-mode timing. */
    { .uri="/setup",        .method=HTTP_GET,  .handler=handle_setup_html   },
    { .uri="/setup.css",    .method=HTTP_GET,  .handler=handle_setup_css    },
    { .uri="/setup.js",     .method=HTTP_GET,  .handler=handle_setup_js     },
    { .uri="/settings",     .method=HTTP_GET,  .handler=handle_settings     },
    { .uri="/settings.css", .method=HTTP_GET,  .handler=handle_settings_css },
    { .uri="/settings.js",  .method=HTTP_GET,  .handler=handle_settings_js  },
    { .uri="/favicon.ico",  .method=HTTP_GET,  .handler=handle_favicon      },
    { .uri="/favicon.svg",  .method=HTTP_GET,  .handler=handle_favicon      },
    { .uri="/api/status",   .method=HTTP_GET,  .handler=handle_api_status   },
    /* Scan APIs — always registered so setup.js can call /api/scan the
     * moment it runs, without waiting for provisioning-mode registration. */
    { .uri="/api/scan",     .method=HTTP_GET,  .handler=handle_api_scan     },
    { .uri="/api/rescan",   .method=HTTP_GET,  .handler=handle_api_rescan   },
    { .uri="/api/connect",  .method=HTTP_POST, .handler=handle_api_connect  },
    { .uri="/api/erase",    .method=HTTP_POST, .handler=handle_api_erase    },
    { .uri="/api/reboot",   .method=HTTP_POST, .handler=handle_api_reboot   },
    { .uri="/api/jserror",  .method=HTTP_POST, .handler=handle_api_jserror  },
    { .uri="/ws",           .method=HTTP_GET,  .handler=ws_handler,
                            .is_websocket=true                               },
};

/* Provisioning-only routes.  Registered by enter_provisioning_mode(),
 * unregistered by exit_provisioning_mode().
 * Only captive-portal probe handlers belong here — they should only
 * intercept OS connectivity checks while the AP is up for provisioning,
 * not during normal STA operation where they would steal legitimate GETs. */
static const httpd_uri_t s_prov_routes[] = {
    { .uri="/generate_204", .method=HTTP_GET,  .handler=handle_captive_android },
    { .uri="/generate204",  .method=HTTP_GET,  .handler=handle_captive_android },
    { .uri="/gen_204",      .method=HTTP_GET,  .handler=handle_captive_android },
    { .uri="/connecttest.txt",               .method=HTTP_GET, .handler=handle_captive_generic },
    { .uri="/ncsi.txt",                      .method=HTTP_GET, .handler=handle_captive_generic },
    { .uri="/hotspot-detect.html",           .method=HTTP_GET, .handler=handle_captive_generic },
    { .uri="/library/test/success.html",     .method=HTTP_GET, .handler=handle_captive_generic },
    { .uri="/success.txt",                   .method=HTTP_GET, .handler=handle_captive_generic },
    { .uri="/canonical.html",                .method=HTTP_GET, .handler=handle_captive_generic },
    { .uri="/browsernetworktime/*",          .method=HTTP_GET, .handler=handle_captive_generic },
    { .uri="/redirect",                      .method=HTTP_GET, .handler=handle_captive_generic },
};

/* Catch-all — registered last so named routes always win. */
static const httpd_uri_t s_catchall_get  = {
    .uri="/*", .method=HTTP_GET,  .handler=handle_404 };
static const httpd_uri_t s_catchall_post = {
    .uri="/*", .method=HTTP_POST, .handler=handle_404 };

/* ════════════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════════════*/

esp_err_t app_server_start(void)
{
    s_ws_mutex   = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();
    if (!s_scan_evt) s_scan_evt = xEventGroupCreate();
    for (int i = 0; i < MAX_WS_CLIENTS; i++) s_ws_fds[i] = -1;

    if (!s_events_registered) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                   scan_event_cb, NULL);
        s_events_registered = true;
    }

    esp_err_t ret = vfs_mount_littlefs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed — aborting");
        return ret;
    }
    vfs_log_inventory();

    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 48;   /* ctrl(21) + prov(11) + OTA(2) + settings(3) + catchall(2) + headroom */
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register always-on routes */
    for (size_t i = 0; i < sizeof(s_ctrl_routes)/sizeof(s_ctrl_routes[0]); i++)
        httpd_register_uri_handler(s_httpd, &s_ctrl_routes[i]);

    ota_server_register(s_httpd);
    ota_server_mark_valid();
    settings_server_register(s_httpd);

    /* Catch-all last */
    httpd_register_uri_handler(s_httpd, &s_catchall_get);
    httpd_register_uri_handler(s_httpd, &s_catchall_post);

    ESP_LOGI(TAG, "App server started on port 80");
    return ESP_OK;
}

void app_server_stop(void)
{
    ctrl_drive_emergency_stop();
    if (s_provisioning) app_server_exit_provisioning_mode();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    ESP_LOGI(TAG, "App server stopped");
}

/* Called by portal.c (via wifi_manager) when the AP is up and provisioning
 * should begin.  Registers setup routes, starts DNS, runs initial scan. */
void app_server_enter_provisioning_mode(app_server_creds_cb_t creds_cb)
{
    if (s_provisioning) {
        ESP_LOGW(TAG, "Already in provisioning mode");
        return;
    }
    s_creds_cb    = creds_cb;
    s_provisioning = true;

    /* Register provisioning routes before the catch-all sees them.
     * The catch-all was registered last, so new specific routes still win
     * because esp_http_server matches in registration order with wildcard
     * enabled — specific URIs are checked before */
    for (size_t i = 0; i < sizeof(s_prov_routes)/sizeof(s_prov_routes[0]); i++)
        httpd_register_uri_handler(s_httpd, &s_prov_routes[i]);

    /* Initial blocking scan — no clients on AP yet, safe to block briefly */
    if (!s_scan_ready) run_scan_once_blocking();

    start_dns();
    set_dhcp_option_114();

    ESP_LOGI(TAG, "Provisioning mode ON — setup routes registered, DNS active");
}

/* Called by portal.c (via wifi_manager) once credentials are accepted and
 * STA connect succeeded.  Tears down provisioning-only infrastructure. */
void app_server_exit_provisioning_mode(void)
{
    if (!s_provisioning) return;
    s_provisioning = false;
    s_creds_cb     = NULL;

    /* Unregister provisioning routes.  Control routes stay. */
    for (size_t i = 0; i < sizeof(s_prov_routes)/sizeof(s_prov_routes[0]); i++) {
        httpd_unregister_uri_handler(s_httpd,
                                     s_prov_routes[i].uri,
                                     s_prov_routes[i].method);
    }

    stop_dns();
    ESP_LOGI(TAG, "Provisioning mode OFF — setup routes removed, DNS stopped");
}

/* ── Telemetry / arm-state push ─────────────────────────────────────────── */

void app_server_push_telemetry(void)
{
    if (!s_httpd || s_ws_count == 0) return;

    const char *json = health_monitor_get_telemetry_json();
    size_t      jlen = strlen(json);

    int live[MAX_WS_CLIENTS]; int nlive = 0;
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
            xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
            for (int j = 0; j < MAX_WS_CLIENTS; j++) {
                if (s_ws_fds[j] == live[i]) {
                    s_ws_fds[j] = -1; s_ws_count--; break;
                }
            }
            int count = s_ws_count;
            xSemaphoreGive(s_ws_mutex);
            prov_ui_set_client_count(count);
            ctrl_drive_emergency_stop();
            ESP_LOGD(TAG, "Pruned dead WS fd=%d (total=%d)", live[i], count);
        }
    }
}

void app_server_push_arm_state(bool armed, const char *reason)
{
    if (!s_httpd || s_ws_count == 0) return;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return;
    cJSON_AddBoolToObject(obj, "armed", armed);
    if (!armed && reason) cJSON_AddStringToObject(obj, "disarm_reason", reason);

    char *body = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!body) return;

    int live[MAX_WS_CLIENTS]; int nlive = 0;
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] >= 0) live[nlive++] = s_ws_fds[i];
    xSemaphoreGive(s_ws_mutex);

    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)body,
        .len     = strlen(body),
    };
    for (int i = 0; i < nlive; i++)
        httpd_ws_send_frame_async(s_httpd, live[i], &frame);

    free(body);
    ESP_LOGI(TAG, "Pushed arm state: armed=%d reason=%s",
             (int)armed, reason ? reason : "none");
}

int app_server_get_client_count(void) { return s_ws_count; }
