/**
 * wifi_manager.c — SoftAP captive-portal WiFi provisioning + robot app server.
 *
 * Portal server (runs while no STA connection):
 *   GET /           setup.html   GET /scan        AP list JSON
 *   GET /setup.css  GET /setup.js
 *   POST /save      save creds to NVS, reboot
 *   Captive-portal probe URLs → redirect to /
 *
 * App server (started on STA connect, port 80):
 *   GET /           index.html   GET /style.css   GET /script.js
 *   GET /ws         WebSocket — receives: "ping", "stop", "x:F,y:F", "led:F"
 *                             sends:    "pong", JSON telemetry
 *
 * DNS hijack redirects all A-record queries to AP_GW_IP (config.h) so the
 * OS captive-portal detector fires automatically on all major platforms.
 */

#include "wifi_manager.h"
#include "prov_ui.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "cJSON.h"

#include "esp_vfs.h"
#include "esp_littlefs.h"
#include <dirent.h>

static const char *TAG = "wifi_mgr";

/* ── LittleFS ───────────────────────────────────────────────────────────────*/
#define FS_BASE_PATH  "/littlefs"
#define FS_PARTITION  "storage"
#define FS_CHUNK_SIZE 1024

static esp_err_t serve_file(httpd_req_t *req, const char *path, const char *mime)
{
    ESP_LOGI(TAG, "serve: %s", path);
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGE(TAG, "serve: not found: %s  (run pio run -t uploadfs)", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    ESP_LOGI(TAG, "serve: %s (%ld B)", path, sz);

    httpd_resp_set_type(req, mime);
    if (strstr(path, ".css") || strstr(path, ".js"))
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");

    char *buf = malloc(FS_CHUNK_SIZE);
    if (!buf) { fclose(f); httpd_resp_send_500(req); return ESP_OK; }

    size_t n;
    size_t sent   = 0;
    bool   ok     = true;
    while (ok && (n = fread(buf, 1, FS_CHUNK_SIZE, f)) > 0)
    {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK)
        {
            ESP_LOGE(TAG, "serve: chunk send failed: %s", path);
            ok = false;
        }
        else { sent += n; }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "serve: done %s sent=%u/%ld", path, (unsigned)sent, sz);

    free(buf);
    fclose(f);
    return ESP_OK;
}

/* ── NVS ────────────────────────────────────────────────────────────────────*/
#define NVS_NAMESPACE "wifi_mgr"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

/* ── Event-group bits ───────────────────────────────────────────────────────*/
#define EVT_STA_CONNECTED BIT0
#define EVT_STA_FAILED    BIT1

/* ── DNS ────────────────────────────────────────────────────────────────────*/
#define DNS_PORT 53

/* ── Internal event queue ───────────────────────────────────────────────────*/
typedef struct
{
    wifi_manager_state_t state;
    char ssid[33];
    char ip[16];
} mgr_event_t;

/* ── Scan cache ─────────────────────────────────────────────────────────────*/
#define SCAN_MAX_APS     20
#define SCAN_INTERVAL_MS 12000

typedef struct
{
    char ssid[33];
    int  quality;
    bool secure;
} ap_info_t;

static SemaphoreHandle_t s_scan_mutex = NULL;
static ap_info_t         s_scan_aps[SCAN_MAX_APS];
static int               s_scan_count = 0;
static bool              s_scan_ready = false;
static TaskHandle_t      s_scan_task  = NULL;

/* ── Module state ───────────────────────────────────────────────────────────*/
static wifi_manager_config_t s_cfg;
static httpd_handle_t        s_httpd       = NULL;
static EventGroupHandle_t    s_sta_evt_grp = NULL;
static QueueHandle_t         s_event_queue = NULL;
static TaskHandle_t          s_dns_task    = NULL;
static TaskHandle_t          s_event_task  = NULL;
static bool                  s_connected   = false;
static bool                  s_sta_failed  = false;
static char                  s_ip[16]      = {0};
static volatile int          s_retry_count = 0;
static char                  s_sta_ssid[33] = {0};
static char                  s_sta_pass[64] = {0};
static httpd_handle_t        s_app_httpd   = NULL;
static int                   s_ws_clients  = 0;  /* guarded by httpd task context */

/* ── DNS IP — parsed once from AP_GW_IP (config.h) at startup ───────────────*/
static uint8_t s_gw_ip[4] = {192, 168, 4, 1};

static void parse_ap_ip(void)
{
    unsigned a, b, c, d;
    if (sscanf(AP_GW_IP, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
    {
        s_gw_ip[0] = (uint8_t)a; s_gw_ip[1] = (uint8_t)b;
        s_gw_ip[2] = (uint8_t)c; s_gw_ip[3] = (uint8_t)d;
        ESP_LOGI(TAG, "DNS redirect → %u.%u.%u.%u", a, b, c, d);
    }
    else
    {
        ESP_LOGW(TAG, "Could not parse AP_GW_IP='%s', using default", AP_GW_IP);
    }
}

/* ── Scan task ──────────────────────────────────────────────────────────────*/
static void scan_task(void *arg)
{
    while (1)
    {
        /* Wait out any active STA retry burst before scanning */
        while (s_retry_count > 0)
            vTaskDelay(pdMS_TO_TICKS(500));

        ESP_LOGI(TAG, "scan_task: starting");
        wifi_scan_config_t scan_cfg = {
            .ssid        = NULL,
            .bssid       = NULL,
            .channel     = 0,
            .show_hidden = false,
            .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        };

        if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK)
        {
            ESP_LOGW(TAG, "scan_task: scan_start failed, retrying in 3 s");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        if (count > SCAN_MAX_APS) count = SCAN_MAX_APS;

        wifi_ap_record_t *recs = calloc(count, sizeof(wifi_ap_record_t));
        if (recs)
        {
            esp_wifi_scan_get_ap_records(&count, recs);
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            s_scan_count = 0;
            for (int i = 0; i < count; i++)
            {
                if (recs[i].ssid[0] == '\0') continue;
                int rssi    = recs[i].rssi;
                int quality = rssi <= -100 ? 0 : rssi >= -40 ? 100 : 2 * (rssi + 100);
                /* escape backslash and quote so the JSON string is safe */
                int si = 0;
                for (int j = 0; recs[i].ssid[j] && j < 32 && si < 31; j++)
                {
                    char c = (char)recs[i].ssid[j];
                    if ((c == '"' || c == '\\') && si < 31)
                        s_scan_aps[s_scan_count].ssid[si++] = '\\';
                    if (si < 32) s_scan_aps[s_scan_count].ssid[si++] = c;
                }
                s_scan_aps[s_scan_count].ssid[si] = '\0';
                s_scan_aps[s_scan_count].quality  = quality;
                s_scan_aps[s_scan_count].secure   = (recs[i].authmode != WIFI_AUTH_OPEN);
                s_scan_count++;
            }
            s_scan_ready = true;
            xSemaphoreGive(s_scan_mutex);
            ESP_LOGI(TAG, "scan_task: %d APs found", s_scan_count);
            free(recs);
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

/* ── DNS task ───────────────────────────────────────────────────────────────*/
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "dns_task: socket() failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        ESP_LOGE(TAG, "dns_task: bind() failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "dns_task: :%d → %u.%u.%u.%u",
             DNS_PORT, s_gw_ip[0], s_gw_ip[1], s_gw_ip[2], s_gw_ip[3]);

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);

    while (1)
    {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;

        uint8_t resp[512];
        memcpy(resp, buf, len);
        resp[2] = 0x81; resp[3] = 0x80;            /* QR + RA bits, RCODE=0 */
        resp[6] = 0x00; resp[7] = 0x01;             /* ANCOUNT = 1           */
        resp[8] = resp[9] = resp[10] = resp[11] = 0;

        int rlen = len;
        if (rlen + 16 > (int)sizeof(resp))
        {
            ESP_LOGW(TAG, "dns_task: query too large (%d), dropping", len);
            continue;
        }
        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C;  /* name ptr → offset 12  */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* Type A                 */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* Class IN               */
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x3C;  /* TTL 60 s               */
        resp[rlen++] = 0x00; resp[rlen++] = 0x04;  /* RDLENGTH 4             */
        resp[rlen++] = s_gw_ip[0]; resp[rlen++] = s_gw_ip[1];
        resp[rlen++] = s_gw_ip[2]; resp[rlen++] = s_gw_ip[3];
        sendto(sock, resp, rlen, 0, (struct sockaddr *)&client, client_len);
    }
}

/* ── Portal HTTP handlers ───────────────────────────────────────────────────*/

static esp_err_t handle_root(httpd_req_t *req)
    { return serve_file(req, FS_BASE_PATH "/setup.html", "text/html"); }

static esp_err_t handle_setup_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE_PATH "/setup.css", "text/css"); }

static esp_err_t handle_setup_js(httpd_req_t *req)
    { return serve_file(req, FS_BASE_PATH "/setup.js", "application/javascript"); }

static esp_err_t handle_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (!s_scan_ready)
    {
        httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
        return ESP_OK;
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    cJSON *arr = cJSON_CreateArray();
    if (!arr) { xSemaphoreGive(s_scan_mutex); httpd_resp_send_500(req); return ESP_OK; }
    for (int i = 0; i < s_scan_count; i++)
    {
        cJSON *obj = cJSON_CreateObject();
        if (!obj) break;
        cJSON_AddStringToObject(obj, "ssid",    s_scan_aps[i].ssid);
        cJSON_AddNumberToObject(obj, "quality", s_scan_aps[i].quality);
        cJSON_AddBoolToObject  (obj, "secure",  s_scan_aps[i].secure);
        cJSON_AddItemToArray(arr, obj);
    }
    xSemaphoreGive(s_scan_mutex);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) { httpd_resp_send_500(req); return ESP_OK; }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static void url_decode(const char *src, char *dst, int dst_size)
{
    int out = 0;
    for (int i = 0; src[i] && out < dst_size - 1; i++)
    {
        if (src[i] == '%' && src[i+1] && src[i+2])
        {
            char hex[3] = {src[i+1], src[i+2], '\0'};
            dst[out++] = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        else if (src[i] == '+') { dst[out++] = ' '; }
        else                    { dst[out++] = src[i]; }
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
    if (len >= out_size) len = out_size - 1;
    char tmp[256] = {0};
    if (len > (int)sizeof(tmp) - 1) len = (int)sizeof(tmp) - 1;
    memcpy(tmp, p, len);
    url_decode(tmp, out, out_size);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t handle_save(httpd_req_t *req)
{
    char body[320] = {0};
    int  n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_500(req); return ESP_OK; }
    body[n] = '\0';

    char ssid[33] = {0};
    char pass[64] = {0};
    extract_field(body, "ssid", ssid, sizeof(ssid));
    extract_field(body, "pass", pass, sizeof(pass));

    if (!ssid[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required"); return ESP_OK; }
    ESP_LOGI(TAG, "handle_save: SSID='%s'", ssid);
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);

    mgr_event_t ev = {.state = WIFI_MANAGER_STATE_CREDS_RECEIVED};
    strncpy(ev.ssid, ssid, sizeof(ev.ssid) - 1);
    xQueueSend(s_event_queue, &ev, 0);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_str(nvs, NVS_KEY_SSID, ssid);
        nvs_set_str(nvs, NVS_KEY_PASS, pass);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Saved. Rebooting...");
    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* Captive-portal OS probes */
static esp_err_t handle_probe_win_modern(httpd_req_t *req)
    { httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "Microsoft Connect Test"); return ESP_OK; }

static esp_err_t handle_probe_win_legacy(httpd_req_t *req)
    { httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "Microsoft NCSI"); return ESP_OK; }

static esp_err_t handle_probe_apple(httpd_req_t *req)
{
    static const char BODY[] =
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
        "<BODY><Success/>"
        "<script>window.location.replace('http://" AP_GW_IP "/');</script>"
        "</BODY></HTML>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, BODY, sizeof(BODY) - 1);
    return ESP_OK;
}

static esp_err_t handle_probe_android(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── LittleFS mount ─────────────────────────────────────────────────────────*/
static void fs_mount(void)
{
    static bool s_mounted = false;
    if (s_mounted) return;

    esp_vfs_littlefs_conf_t cfg = {
        .base_path              = FS_BASE_PATH,
        .partition_label        = FS_PARTITION,
        .format_if_mount_failed = true,
    };
    if (esp_vfs_littlefs_register(&cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "LittleFS mount failed (check partitions.csv)");
        return;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "LittleFS mounted at " FS_BASE_PATH);

    DIR *dp = opendir(FS_BASE_PATH);
    if (!dp) return;
    struct dirent *de;
    int  file_count  = 0;
    long total_bytes = 0;
    ESP_LOGI(TAG, "+-- LittleFS contents (" FS_BASE_PATH ") -------");
    while ((de = readdir(dp)) != NULL)
    {
        char full[300];
        snprintf(full, sizeof(full), FS_BASE_PATH "/%s", de->d_name);
        FILE *tf = fopen(full, "rb");
        long sz = 0;
        if (tf) { fseek(tf, 0, SEEK_END); sz = ftell(tf); fclose(tf); }
        ESP_LOGI(TAG, "| %-32s  %6ld B", de->d_name, sz);
        file_count++;
        total_bytes += sz;
    }
    closedir(dp);
    if (file_count == 0)
        ESP_LOGW(TAG, "|  (empty — run: pio run -t uploadfs)");
    ESP_LOGI(TAG, "| %d file(s), %ld B total", file_count, total_bytes);
    ESP_LOGI(TAG, "+------------------------------------------------");
}

/* ── Portal HTTP server ──────────────────────────────────────────────────────*/
static void http_server_start(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK)
        { ESP_LOGE(TAG, "portal httpd_start failed"); return; }

    static const httpd_uri_t routes[] = {
        {.uri="/",                          .method=HTTP_GET,  .handler=handle_root             },
        {.uri="/setup.css",                 .method=HTTP_GET,  .handler=handle_setup_css        },
        {.uri="/setup.js",                  .method=HTTP_GET,  .handler=handle_setup_js         },
        {.uri="/scan",                      .method=HTTP_GET,  .handler=handle_scan             },
        {.uri="/save",                      .method=HTTP_POST, .handler=handle_save             },
        {.uri="/connecttest.txt",           .method=HTTP_GET,  .handler=handle_probe_win_modern },
        {.uri="/connecttest.txt",           .method=HTTP_HEAD, .handler=handle_probe_win_modern },
        {.uri="/ncsi.txt",                  .method=HTTP_GET,  .handler=handle_probe_win_legacy },
        {.uri="/ncsi.txt",                  .method=HTTP_HEAD, .handler=handle_probe_win_legacy },
        {.uri="/hotspot-detect.html",       .method=HTTP_GET,  .handler=handle_probe_apple      },
        {.uri="/library/test/success.html", .method=HTTP_GET,  .handler=handle_probe_apple      },
        {.uri="/generate_204",              .method=HTTP_GET,  .handler=handle_probe_android    },
        {.uri="/*",                         .method=HTTP_GET,  .handler=handle_captive_redirect },
    };
    for (int i = 0; i < (int)(sizeof(routes)/sizeof(routes[0])); i++)
        httpd_register_uri_handler(s_httpd, &routes[i]);
    ESP_LOGI(TAG, "Portal HTTP server started");
}

static void http_server_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
}

/* ── App server handlers ─────────────────────────────────────────────────────*/

static esp_err_t handle_favicon(httpd_req_t *req)
    { httpd_resp_set_status(req, "204 No Content"); httpd_resp_send(req, NULL, 0); return ESP_OK; }

static esp_err_t handle_app_root(httpd_req_t *req)
    { return serve_file(req, FS_BASE_PATH "/index.html", "text/html"); }

static esp_err_t handle_app_css(httpd_req_t *req)
    { return serve_file(req, FS_BASE_PATH "/style.css", "text/css"); }

static esp_err_t handle_app_js(httpd_req_t *req)
    { return serve_file(req, FS_BASE_PATH "/script.js", "application/javascript"); }

static esp_err_t handle_ws(httpd_req_t *req)
{
#ifdef CONFIG_HTTPD_WS_SUPPORT
    /* Handshake — new connection */
    if (req->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "WS client connected (total=%d)", s_ws_clients + 1);
        s_ws_clients++;
        prov_ui_set_client_count(s_ws_clients);
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "WS recv header: %s", esp_err_to_name(ret));
        return ret;
    }

    if (pkt.type == HTTPD_WS_TYPE_CLOSE)
    {
        /* Echo the close frame as required by RFC 6455 §5.5.1 */
        httpd_ws_frame_t close_reply = {.type = HTTPD_WS_TYPE_CLOSE, .len = 0};
        httpd_ws_send_frame(req, &close_reply);
        ESP_LOGI(TAG, "WS client disconnected (remaining=%d)",
                 s_ws_clients > 0 ? s_ws_clients - 1 : 0);
        if (s_ws_clients > 0) s_ws_clients--;
        prov_ui_set_client_count(s_ws_clients);
        return ESP_OK;
    }

    if (pkt.len == 0) return ESP_OK;

    uint8_t *buf = malloc(pkt.len + 1);
    if (!buf) { ESP_LOGE(TAG, "WS malloc failed"); return ESP_ERR_NO_MEM; }

    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "WS recv payload: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }

    char *msg = (char *)buf;
    msg[pkt.len] = '\0';
    ESP_LOGD(TAG, "WS rx: %s", msg);

    if (pkt.type == HTTPD_WS_TYPE_TEXT)
    {
        if (strcmp(msg, "ping") == 0)
        {
            httpd_ws_frame_t reply = {
                .payload = (uint8_t *)"pong",
                .len     = 4,
                .type    = HTTPD_WS_TYPE_TEXT
            };
            httpd_ws_send_frame(req, &reply);
        }
        else if (strncmp(msg, "led:", 4) == 0)
        {
            float val;
            if (sscanf(msg + 4, "%f", &val) == 1)
            {
                /* TODO: motor_set_led(val); */
                ESP_LOGD(TAG, "LED brightness: %.2f", val);
            }
        }
        else if (strncmp(msg, "x:", 2) == 0)
        {
            float x, y;
            if (sscanf(msg, "x:%f,y:%f", &x, &y) == 2)
            {
                /* TODO: motor_drive(x, y); */
                ESP_LOGD(TAG, "Drive: x=%.2f y=%.2f", x, y);
            }
        }
        else if (strcmp(msg, "stop") == 0)
        {
            /* TODO: motor_stop(); */
            ESP_LOGD(TAG, "Stop");
        }
    }

    free(buf);
    return ESP_OK;

#else
    ESP_LOGW(TAG, "WS: CONFIG_HTTPD_WS_SUPPORT not enabled in sdkconfig");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "WebSocket support not compiled in.");
    return ESP_OK;
#endif
}

/* ── App server lifecycle ────────────────────────────────────────────────────*/

void app_server_start(void)
{
    if (s_app_httpd) return;

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 8;

    if (httpd_start(&s_app_httpd, &cfg) != ESP_OK)
        { ESP_LOGE(TAG, "app_server httpd_start failed"); return; }

    static const httpd_uri_t app_routes[] = {
        {.uri="/",            .method=HTTP_GET, .handler=handle_app_root},
        {.uri="/style.css",   .method=HTTP_GET, .handler=handle_app_css },
        {.uri="/script.js",   .method=HTTP_GET, .handler=handle_app_js  },
        {.uri="/favicon.ico", .method=HTTP_GET, .handler=handle_favicon },
#ifdef CONFIG_HTTPD_WS_SUPPORT
        {.uri="/ws", .method=HTTP_GET, .handler=handle_ws, .is_websocket=true},
#else
        {.uri="/ws", .method=HTTP_GET, .handler=handle_ws},
#endif
    };
    for (int i = 0; i < (int)(sizeof(app_routes)/sizeof(app_routes[0])); i++)
        httpd_register_uri_handler(s_app_httpd, &app_routes[i]);
    ESP_LOGI(TAG, "App server started on port 80");
}

void app_server_stop(void)
{
    if (s_app_httpd) { httpd_stop(s_app_httpd); s_app_httpd = NULL; }
}

/* ── SoftAP + portal start ───────────────────────────────────────────────────*/
static void portal_start(void)
{
    esp_wifi_disconnect();

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, s_cfg.ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(s_cfg.ap_ssid);
    ap_cfg.ap.channel        = s_cfg.ap_channel ? s_cfg.ap_channel : 1;
    ap_cfg.ap.max_connection = WIFI_MANAGER_AP_MAX_STA;

    if (s_cfg.ap_password && strlen(s_cfg.ap_password) >= 8)
    {
        strncpy((char *)ap_cfg.ap.password, s_cfg.ap_password,
                sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    else
    {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    ESP_LOGI(TAG, "SoftAP: SSID=%s", s_cfg.ap_ssid);

    if (!s_scan_mutex)  s_scan_mutex = xSemaphoreCreateMutex();
    if (!s_scan_task)   xTaskCreate(scan_task, "wifi_mgr_scan", 4096, NULL, 3, &s_scan_task);
    if (!s_dns_task)    xTaskCreate(dns_task,  "wifi_mgr_dns",  4096, NULL, 5, &s_dns_task);
    if (!s_httpd)       http_server_start();

    mgr_event_t ev = {.state = WIFI_MANAGER_STATE_AP_STARTED};
    xQueueSend(s_event_queue, &ev, 0);
}

/* ── NVS helpers ─────────────────────────────────────────────────────────────*/
static bool nvs_load_credentials(char *ssid, size_t ssid_sz,
                                  char *pass, size_t pass_sz)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    bool ok = (nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_sz) == ESP_OK) && (ssid[0] != '\0');
    nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_sz);
    nvs_close(nvs);
    ESP_LOGI(TAG, "NVS credentials: %s", ok ? "found" : "not found");
    return ok;
}

/* ── WiFi event handler ──────────────────────────────────────────────────────*/
static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            if (s_retry_count < s_cfg.sta_max_retries)
            {
                s_retry_count++;
                ESP_LOGI(TAG, "STA retry %d/%d", s_retry_count, s_cfg.sta_max_retries);
                esp_wifi_connect();
            }
            else
            {
                ESP_LOGW(TAG, "STA failed after %d retries", s_retry_count);
                s_sta_failed = true;
                xEventGroupSetBits(s_sta_evt_grp, EVT_STA_FAILED);
                mgr_event_t ev = {.state = WIFI_MANAGER_STATE_STA_FAILED};
                xQueueSend(s_event_queue, &ev, 0);
            }
            break;

        case WIFI_EVENT_AP_STACONNECTED:
            xQueueSend(s_event_queue,
                       &(mgr_event_t){.state = WIFI_MANAGER_STATE_CLIENT_CONNECTED}, 0);
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            xQueueSend(s_event_queue,
                       &(mgr_event_t){.state = WIFI_MANAGER_STATE_CLIENT_GONE}, 0);
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected  = true;
        s_sta_failed = false;
        xEventGroupSetBits(s_sta_evt_grp, EVT_STA_CONNECTED);
        ESP_LOGI(TAG, "STA connected, IP=%s", s_ip);

        mgr_event_t ev = {.state = WIFI_MANAGER_STATE_STA_CONNECTED};
        strncpy(ev.ssid, s_sta_ssid, sizeof(ev.ssid) - 1);
        strncpy(ev.ip,   s_ip,       sizeof(ev.ip)   - 1);
        xQueueSend(s_event_queue, &ev, 0);
    }
}

/* ── Event dispatch task ─────────────────────────────────────────────────────*/
static void event_task(void *arg)
{
    mgr_event_t ev;
    while (xQueueReceive(s_event_queue, &ev, portMAX_DELAY) == pdTRUE)
    {
        ESP_LOGI(TAG, "event_task: state → %d", (int)ev.state);

        if (ev.state == WIFI_MANAGER_STATE_STA_CONNECTING && ev.ssid[0])
            strncpy(s_sta_ssid, ev.ssid, sizeof(s_sta_ssid) - 1);

        if (s_cfg.on_state_change)
            s_cfg.on_state_change(ev.state, s_cfg.cb_ctx);

        if (ev.state == WIFI_MANAGER_STATE_STA_CONNECTED)
        {
            http_server_stop();
            app_server_start();
            if (s_cfg.on_connected)
                s_cfg.on_connected(ev.ssid, ev.ip, s_cfg.cb_ctx);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────────*/

esp_err_t wifi_manager_start(const wifi_manager_config_t *config)
{
    if (!config || !config->ap_ssid) return ESP_ERR_INVALID_ARG;
    memcpy(&s_cfg, config, sizeof(s_cfg));
    s_retry_count = 0;
    s_sta_failed  = false;
    parse_ap_ip();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition dirty, erasing");
        nvs_flash_erase();
        nvs_flash_init();
    }
    else if (nvs_err != ESP_OK && nvs_err != ESP_ERR_NVS_INVALID_STATE)
    {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(nvs_err));
        return nvs_err;
    }

    fs_mount();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_event_cb, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL);
    ESP_ERROR_CHECK(esp_wifi_start());

    s_sta_evt_grp = xEventGroupCreate();
    s_event_queue = xQueueCreate(10, sizeof(mgr_event_t));
    xTaskCreate(event_task, "wifi_mgr_evt", 4096, NULL, 4, &s_event_task);

    /* Try stored credentials first */
    char saved_ssid[33] = {0};
    char saved_pass[64] = {0};

    if (nvs_load_credentials(saved_ssid, sizeof(saved_ssid),
                             saved_pass, sizeof(saved_pass)))
    {
        strncpy(s_sta_ssid, saved_ssid, sizeof(s_sta_ssid) - 1);
        strncpy(s_sta_pass, saved_pass, sizeof(s_sta_pass) - 1);

        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid,     saved_ssid, sizeof(sta_cfg.sta.ssid)     - 1);
        strncpy((char *)sta_cfg.sta.password, saved_pass, sizeof(sta_cfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_connect();

        mgr_event_t ev = {.state = WIFI_MANAGER_STATE_CONNECTING_SAVED};
        strncpy(ev.ssid, saved_ssid, sizeof(ev.ssid) - 1);
        xQueueSend(s_event_queue, &ev, 0);

        EventBits_t bits = xEventGroupWaitBits(
            s_sta_evt_grp,
            EVT_STA_CONNECTED | EVT_STA_FAILED,
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(10000));

        if (bits & EVT_STA_CONNECTED) return ESP_OK;

        /* Saved creds failed — reset counter before starting portal */
        s_retry_count = 0;
        esp_wifi_disconnect();
        esp_wifi_set_mode(WIFI_MODE_NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    portal_start();
    return ESP_OK;
}

esp_err_t wifi_manager_reset(void)
{
    wifi_manager_erase_credentials();
    s_connected = s_sta_failed = false;
    s_retry_count = 0;
    http_server_stop();
    portal_start();
    return ESP_OK;
}

esp_err_t wifi_manager_erase_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK)
    {
        nvs_erase_key(nvs, NVS_KEY_SSID);
        nvs_erase_key(nvs, NVS_KEY_PASS);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Credentials erased");
    }
    return err;
}

bool        wifi_manager_is_connected(void) { return s_connected; }
const char *wifi_manager_get_ip(void)       { return s_connected ? s_ip : NULL; }
