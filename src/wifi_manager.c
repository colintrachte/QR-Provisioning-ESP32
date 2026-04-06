/**
 * wifi_manager.c -- SoftAP captive-portal WiFi provisioning for ESP-IDF.
 *
 * See wifi_manager.h for the public API and behaviour overview.
 *
 * Internal architecture
 * ---------------------
 *  dns_task          UDP/53 server -- answers every A-record query with the
 *                    AP gateway IP, hijacking all DNS so that every hostname
 *                    the connecting device probes resolves to us.
 *
 *  event_task        Dequeues internal prov_event_t messages produced by
 *                    the WiFi event handler and fires the user's callbacks.
 *                    Runs on its own task so callbacks never execute from
 *                    the WiFi/lwIP context -- display_* calls are safe.
 *
 *  wifi_event_cb     Registered for WIFI_EVENT (any) and IP_EVENT_STA_GOT_IP.
 *                    Translates ESP-IDF events into prov_event_t and enqueues
 *                    them.  Never calls user code directly.
 *
 *  HTTP handlers     Served by esp_http_server.  Each OS that probes for
 *                    internet connectivity gets a dedicated handler -- see the
 *                    table in wifi_manager.h and the handler comments below.
 *
 * Scan flow
 * -----------
 *  Scanning runs on a dedicated scan_task, not from the httpd handler.
 *  Calling esp_wifi_scan_start(block=true) from an httpd handler starves
 *  the TCP stack -- the HTTP response never arrives.  scan_task blocks
 *  itself while the radio scans, stores results in a mutex-protected cache,
 *  and re-scans every SCAN_INTERVAL_MS.  handle_scan() only reads that
 *  cache -- it never touches the WiFi driver.
 */

#include "wifi_manager.h"

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

static const char *TAG = "wifi_mgr";

/* =======================================================================
 *  LITTLEFS / FILE SERVING
 * =======================================================================*/
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include <dirent.h>

#define FS_BASE_PATH  "/littlefs"
#define FS_PARTITION  "storage"
#define FS_CHUNK_SIZE 1024

static esp_err_t serve_file(httpd_req_t *req,
                             const char  *path,
                             const char  *mime_type)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "serve_file: not found: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }
    httpd_resp_set_type(req, mime_type);
    char *buf = malloc(FS_CHUNK_SIZE);
    if (!buf) { fclose(f); httpd_resp_send_500(req); return ESP_OK; }
    size_t n;
    bool send_ok = true;
    while (send_ok && (n = fread(buf, 1, FS_CHUNK_SIZE, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            ESP_LOGW(TAG, "serve_file: chunk send failed: %s", path);
            send_ok = false;
        }
    }
    /* Always terminate chunked transfer so the browser does not hang,
     * even when a mid-stream send error occurred. */
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    fclose(f);
    return ESP_OK;
}

/* -- NVS namespace / keys -----------------------------------------------*/
#define NVS_NAMESPACE  "wifi_mgr"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "pass"

/* -- Event-group bits used to synchronise the initial credential check -*/
#define EVT_STA_CONNECTED BIT0
#define EVT_STA_FAILED    BIT1

/* -- DNS ----------------------------------------------------------------*/
#define DNS_PORT 53

/* -- AP gateway (must match the netif IP assigned to the AP interface) -*/
#define AP_GW_IP  WIFI_MANAGER_AP_IP  /* "192.168.4.1" */

/* -- Internal event passed through the callback queue ------------------*/
typedef struct {
    wifi_manager_state_t state;
    char ssid[33];
    char ip[16];
} mgr_event_t;

/* -- Scan result cache --------------------------------------------------
 * Populated by scan_task, read by handle_scan.
 * Protected by s_scan_mutex so the HTTP handler never sees a half-written
 * buffer.  s_scan_ready flips to true after the first scan completes.
 */
#define SCAN_MAX_APS  20

typedef struct {
    char ssid[33];
    int  quality;   /* 0-100 mapped from RSSI */
    bool secure;
} ap_info_t;

static SemaphoreHandle_t s_scan_mutex  = NULL;
static ap_info_t         s_scan_aps[SCAN_MAX_APS];
static int               s_scan_count  = 0;
static bool              s_scan_ready  = false;
static TaskHandle_t      s_scan_task   = NULL;

/* -- Module-level state -------------------------------------------------*/
static wifi_manager_config_t s_cfg;
static httpd_handle_t        s_httpd        = NULL;
static EventGroupHandle_t    s_sta_evt_grp  = NULL;
static QueueHandle_t         s_event_queue  = NULL;
static TaskHandle_t          s_dns_task     = NULL;
static TaskHandle_t          s_event_task   = NULL;
static bool                  s_connected    = false;
static bool                  s_sta_failed   = false;
static char                  s_ip[16]       = {0};
static int                   s_retry_count  = 0;
static char                  s_sta_ssid[33] = {0};  /* SSID we are trying to join */
static char                  s_sta_pass[64] = {0};  /* password for above         */

/* =======================================================================
 *  SCAN TASK
 *
 *  Runs a blocking WiFi scan every SCAN_INTERVAL_MS on its own FreeRTOS
 *  task, then stores results in the shared cache (s_scan_aps / s_scan_count).
 *
 *  Why a dedicated task?
 *    esp_wifi_scan_start(block=true) is safe to call from a FreeRTOS task
 *    but NOT from the httpd handler task -- blocking there starves the TCP
 *    stack and the HTTP response never arrives.  A dedicated task has no
 *    such constraint; it just blocks itself while the radio scans.
 *
 *  Why not the WIFI_EVENT_SCAN_DONE event approach?
 *    It requires a volatile state machine shared between the WiFi event
 *    handler and the HTTP handler, across two different execution contexts,
 *    with an inherent race: the event can fire between the handler checking
 *    state and the next HTTP poll arriving.  A dedicated task with a mutex-
 *    protected cache is simpler, more robust, and easier to reason about.
 * =======================================================================*/

#define SCAN_INTERVAL_MS  12000   /* re-scan every 12 s while portal is up */

static void scan_task(void *arg)
{
    while (1) {
        /* Block while STA is actively connecting -- scanning during a
         * connection attempt can disrupt the handshake on some APs. */
        while (s_retry_count > 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        ESP_LOGI(TAG, "Starting WiFi scan");
        wifi_scan_config_t scan_cfg = {
            .ssid        = NULL,
            .bssid       = NULL,
            .channel     = 0,        /* all channels */
            .show_hidden = false,
            .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        };

        /* block=true: this task sleeps until the scan finishes (~2-4 s).
         * Safe here because we are not the httpd task. */
        esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scan_start failed: %s -- retrying in 3 s",
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        if (count > SCAN_MAX_APS) count = SCAN_MAX_APS;

        wifi_ap_record_t *recs = calloc(count, sizeof(wifi_ap_record_t));
        if (recs) {
            esp_wifi_scan_get_ap_records(&count, recs);

            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            s_scan_count = 0;
            for (int i = 0; i < count; i++) {
                /* Skip entries with empty or non-printable SSIDs. */
                if (recs[i].ssid[0] == '\0') continue;

                int rssi    = recs[i].rssi;
                int quality = rssi <= -100 ? 0
                            : rssi >= -40  ? 100
                            : 2 * (rssi + 100);

                /* JSON-escape the SSID: only '"'  and '\\' need escaping.
                 * Guard si < 31 so an escape+char pair at the last slot
                 * (bytes 31+32) still leaves room for the null at byte 32,
                 * which is within the 33-byte ssid field. */
                int si = 0;
                for (int j = 0; recs[i].ssid[j] && j < 32 && si < 31; j++) {
                    char c = (char)recs[i].ssid[j];
                    if (c == '"' || c == '\\') s_scan_aps[s_scan_count].ssid[si++] = '\\';
                    if (si < 32) s_scan_aps[s_scan_count].ssid[si++] = c;
                }
                s_scan_aps[s_scan_count].ssid[si] = '\0';
                s_scan_aps[s_scan_count].quality   = quality;
                s_scan_aps[s_scan_count].secure    = (recs[i].authmode != WIFI_AUTH_OPEN);
                s_scan_count++;
            }
            s_scan_ready = true;
            xSemaphoreGive(s_scan_mutex);

            ESP_LOGI(TAG, "Scan complete: %d APs found", s_scan_count);
            free(recs);
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

/* =======================================================================
 *  DNS TASK
 *  Answers every UDP DNS query with an A record pointing to AP_GW_IP.
 *  This forces all hostname lookups (captive-portal probes, browsers, etc.)
 *  to resolve to the ESP32 HTTP server.
 * =======================================================================*/
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns_task: socket() failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "dns_task: bind() failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS hijack listening on :%d -> %s", DNS_PORT, AP_GW_IP);

    /* Hard-code the response IP octets once. */
    uint8_t gw[4] = {192, 168, 4, 1};

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;  /* too short to be a valid DNS header */

        /*
         * Build a minimal DNS response in-place.
         * We copy the query, flip the QR bit, set ANCOUNT=1, then append
         * a single A record answer via a name pointer back to the question.
         *
         * DNS header flags:
         *   byte 2: 0x81 = QR(1) OPCODE(0000) AA(0) TC(0) RD(1)
         *   byte 3: 0x80 = RA(1) Z(000) RCODE(0000)
         */
        uint8_t resp[512];
        memcpy(resp, buf, len);
        resp[2] = 0x81; resp[3] = 0x80;           /* Response, no error          */
        resp[6] = 0x00; resp[7] = 0x01;           /* ANCOUNT = 1                 */
        resp[8] = resp[9] = resp[10] = resp[11] = 0x00; /* NSCOUNT/ARCOUNT = 0   */

        /* A-record suffix is exactly 16 bytes.  Verify it fits before
         * appending so we cannot write past the end of resp[512]. */
        int rlen = len;
        if (rlen + 16 > (int)sizeof(resp)) {
            ESP_LOGW(TAG, "dns_task: query too large (%d), dropping", len);
            continue;
        }
        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C; /* Name: pointer to offset 12 */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; /* Type A                      */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; /* Class IN                    */
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x3C; /* TTL = 60 s                  */
        resp[rlen++] = 0x00; resp[rlen++] = 0x04; /* RDLENGTH = 4                */
        resp[rlen++] = gw[0]; resp[rlen++] = gw[1];
        resp[rlen++] = gw[2]; resp[rlen++] = gw[3];

        sendto(sock, resp, rlen, 0, (struct sockaddr *)&client, client_len);
    }
}

/* =======================================================================
 *  PORTAL HTML / CSS / JS
 *
 *  Inlined as a C string literal -- no filesystem required.
 *  The JS uses a poll-based scan (fetch /scan until array arrives) rather
 *  than a single blocking request, matching the non-blocking scan design
 *  in handler_scan() below.
 * =======================================================================*/
/* ── Portal HTTP handlers ────────────────────────────────────────────────────
 * setup.html / setup.css / setup.js are served from LittleFS.
 * Edit the files in data/ and run `pio run -t uploadfs` to update them
 * without reflashing the firmware.
 */

/** GET /  — provisioning portal page. */
static esp_err_t handle_root(httpd_req_t *req)
{
    return serve_file(req, FS_BASE_PATH "/setup.html", "text/html");
}

/** GET /setup.css */
static esp_err_t handle_setup_css(httpd_req_t *req)
{
    return serve_file(req, FS_BASE_PATH "/setup.css", "text/css");
}

/** GET /setup.js */
static esp_err_t handle_setup_js(httpd_req_t *req)
{
    return serve_file(req, FS_BASE_PATH "/setup.js", "application/javascript");
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (!s_scan_ready) {
        httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
        return ESP_OK;
    }

    /* Build JSON with cJSON -- handles allocation and escaping safely.
     * cJSON is bundled with ESP-IDF; no extra dependency needed. */
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        xSemaphoreGive(s_scan_mutex);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    for (int i = 0; i < s_scan_count; i++) {
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

    if (!json) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/**
 * POST /save -- persist credentials to NVS and schedule a device reboot.
 *
 * Borrowed from the 8266 project: save-and-reboot is simpler and more
 * reliable than trying to connect from within APSTA mode.
 *
 *   1. Credentials written to NVS.
 *   2. HTTP 200 response sent to the browser.
 *   3. Reboot scheduled 1.2 s later (gives the TCP stack time to flush).
 *   4. Device reboots into STA-only mode and connects to the target network.
 *
 * The browser's fetch() will either get the 200 (fast) or a network error
 * (if the AP shuts down before the response arrives) -- both cases are
 * handled by the JS, which shows a success message either way.
 *
 * Body: application/x-www-form-urlencoded  ssid=<val>&pass=<val>
 * Response: HTTP 200 plain text, then device reboots.
 */
/* ======================================================================
 *  FORM FIELD HELPERS
 * ======================================================================*/

static void url_decode(const char *src, char *dst, int dst_size)
{
    int out = 0;
    for (int i = 0; src[i] && out < dst_size - 1; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], '\0'};
            dst[out++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[out++] = ' ';
        } else {
            dst[out++] = src[i];
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
    if (len >= out_size) len = out_size - 1;
    char tmp[256] = {0};
    if (len > (int)sizeof(tmp) - 1) len = (int)sizeof(tmp) - 1;
    memcpy(tmp, p, len);
    url_decode(tmp, out, out_size);
}

/* One-shot task: delay briefly then reboot.
 * Spawned by handle_save() so the HTTP handler can return and lwIP
 * can complete the TCP ACK/FIN before the radio shuts down. */
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

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Saving credentials for SSID: %s", ssid);

    /* Notify UI state machine. */
    mgr_event_t ev = {.state = WIFI_MANAGER_STATE_CREDS_RECEIVED};
    strncpy(ev.ssid, ssid, sizeof(ev.ssid) - 1);
    xQueueSend(s_event_queue, &ev, 0);

    /* Persist to NVS. */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_SSID, ssid);
        nvs_set_str(nvs, NVS_KEY_PASS, pass);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    /* Respond before rebooting so the browser receives the reply. */
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Saved. Rebooting...");

    /* Reboot from a one-shot task so this handler returns immediately
     * and lwIP can finish the TCP ACK + FIN handshake before the radio
     * disappears.  Spawning a task lets the RTOS flush naturally instead
     * of relying on a fixed-duration guess delay. */
    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t handle_probe_win_modern(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Microsoft Connect Test");
    return ESP_OK;
}

/**
 * Windows 7/8 (legacy NCSI) -- probes /ncsi.txt on msftncsi.com.
 * Requires HTTP 200 + exact body "Microsoft NCSI".
 */
static esp_err_t handle_probe_win_legacy(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Microsoft NCSI");
    return ESP_OK;
}

/**
 * iOS / macOS -- probes /hotspot-detect.html on captive.apple.com and
 * /library/test/success.html on www.apple.com.
 * Requires HTTP 200 + body containing the literal string "<Success>".
 * We also add a JS redirect so the CNA mini-browser navigates straight to
 * the portal without an extra tap.
 */
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

/**
 * Android -- probes /generate_204 on various Google hostnames.
 * Expects HTTP 204 when internet is present; any other status triggers
 * the "Sign in to network" notification.  We return 302 so Android also
 * navigates the user to the portal directly.
 */
static esp_err_t handle_probe_android(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * Catch-all -- redirect anything else to the portal.
 * By the time a real browser makes an arbitrary request, the OS has already
 * shown the sign-in notification via one of the dedicated probe handlers above.
 */
static esp_err_t handle_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_GW_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* =======================================================================
 *  HTTP SERVER LIFECYCLE
 * =======================================================================*/

/* =======================================================================
 *  FILESYSTEM MOUNT
 *
 *  Called once at the top of wifi_manager_start() so LittleFS is ready
 *  regardless of which path (saved-credentials or portal) is taken.
 * =======================================================================*/
static void fs_mount(void)
{
    static bool s_mounted = false;
    if (s_mounted) return;

    esp_vfs_spiffs_conf_t cfg = {
        .base_path              = FS_BASE_PATH,
        .partition_label        = FS_PARTITION,
        .max_files              = 8,
        /* format_if_mount_failed = true: if the partition contains
         * garbage (never been formatted, or flashed with wrong image)
         * the driver formats it automatically.  The web files will still
         * be missing until uploadfs runs, but the mount itself succeeds
         * so serve_file() returns clean 404s rather than crashing. */
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Partition 'storage' at 0x520000 may be missing or corrupt.");
    } else {
        ESP_LOGI(TAG, "LittleFS mounted at " FS_BASE_PATH);
        s_mounted = true;

        /* List every file in the filesystem on boot so the serial log
         * confirms exactly what was flashed and at what VFS path.      */
        DIR *dp = opendir(FS_BASE_PATH);
        if (dp) {
            struct dirent *de;
            ESP_LOGI(TAG, "--- LittleFS contents ---");
            while ((de = readdir(dp)) != NULL) {
                ESP_LOGI(TAG, "  %s/%s", FS_BASE_PATH, de->d_name);
            }
            closedir(dp);
            ESP_LOGI(TAG, "--- end ---");
        } else {
            ESP_LOGW(TAG, "opendir(" FS_BASE_PATH ") failed");
        }
    }
}

static void http_server_start(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    /* -- Application endpoints ----------------------------------------
     * All registered before captive-probe paths to guarantee priority. */
    static const httpd_uri_t routes[] = {
        {.uri="/",               .method=HTTP_GET,  .handler=handle_root       },
        {.uri="/setup.css",      .method=HTTP_GET,  .handler=handle_setup_css  },
        {.uri="/setup.js",       .method=HTTP_GET,  .handler=handle_setup_js   },
        {.uri="/scan",           .method=HTTP_GET,  .handler=handle_scan       },
        {.uri="/save",           .method=HTTP_POST, .handler=handle_save       },

        /* Windows 10/11 -- GET and HEAD both required */
        {.uri="/connecttest.txt", .method=HTTP_GET,  .handler=handle_probe_win_modern},
        {.uri="/connecttest.txt", .method=HTTP_HEAD, .handler=handle_probe_win_modern},

        /* Windows legacy */
        {.uri="/ncsi.txt", .method=HTTP_GET,  .handler=handle_probe_win_legacy},
        {.uri="/ncsi.txt", .method=HTTP_HEAD, .handler=handle_probe_win_legacy},

        /* iOS / macOS */
        {.uri="/hotspot-detect.html",      .method=HTTP_GET, .handler=handle_probe_apple},
        {.uri="/library/test/success.html",.method=HTTP_GET, .handler=handle_probe_apple},

        /* Android */
        {.uri="/generate_204", .method=HTTP_GET, .handler=handle_probe_android},

        /* Catch-all -- MUST be last */
        {.uri="/*", .method=HTTP_GET, .handler=handle_captive_redirect},
    };

    for (int i = 0; i < (int)(sizeof(routes)/sizeof(routes[0])); i++) {
        httpd_register_uri_handler(s_httpd, &routes[i]);
    }

    ESP_LOGI(TAG, "HTTP server started");
}

/* ── App HTTP handlers (STA mode) ────────────────────────────────────────────
 * Served after the device connects to the user's network.
 * index.html / style.css / script.js live in data/ on LittleFS.
 *
 * WebSocket (/ws) is handled here as a plain HTTP upgrade; the actual
 * telemetry/command logic lives in your robot application layer — wire it
 * up by calling httpd_ws_send_frame() from wherever you produce telemetry.
 */

/** GET /favicon.ico -- return 204 No Content so browsers stop asking. */
static esp_err_t handle_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_app_root(httpd_req_t *req)
{
    return serve_file(req, FS_BASE_PATH "/index.html", "text/html");
}

static esp_err_t handle_app_css(httpd_req_t *req)
{
    return serve_file(req, FS_BASE_PATH "/style.css", "text/css");
}

static esp_err_t handle_app_js(httpd_req_t *req)
{
    return serve_file(req, FS_BASE_PATH "/script.js", "application/javascript");
}

static httpd_handle_t s_app_httpd = NULL;  /* app server (STA mode) */

/**
 * Start the robot-control HTTP server on port 80.
 * Called from both connection paths in wifi_manager_start() and from
 * event_task when WIFI_MANAGER_STATE_STA_CONNECTED fires (portal path).
 * Guarded so it is safe to call more than once.
 */
void app_server_start(void)
{
    if (s_app_httpd) return;

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 8;  /* /, css, js, ws, favicon + headroom */

    if (httpd_start(&s_app_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "app_server_start: httpd_start failed");
        return;
    }

    /* /ws is registered as a normal GET handler.
     * ESP-IDF upgrades it to WebSocket automatically when
     * CONFIG_HTTPD_WS_SUPPORT=y is set in sdkconfig.
     * Without that Kconfig option the route still accepts the HTTP
     * handshake; the browser's WebSocket constructor will then throw,
     * which is the correct failure mode — set the Kconfig option to
     * enable the full WebSocket path. */
    static const httpd_uri_t app_routes[] = {
        { .uri = "/",            .method = HTTP_GET, .handler = handle_app_root },
        { .uri = "/style.css",   .method = HTTP_GET, .handler = handle_app_css  },
        { .uri = "/script.js",   .method = HTTP_GET, .handler = handle_app_js   },
        { .uri = "/ws",          .method = HTTP_GET, .handler = handle_app_root },
        /* Silently discard favicon requests rather than logging a 404. */
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = handle_favicon  },
    };
    cfg.uri_match_fn = NULL;  /* exact match only for app server */

    for (int i = 0; i < (int)(sizeof(app_routes)/sizeof(app_routes[0])); i++) {
        httpd_register_uri_handler(s_app_httpd, &app_routes[i]);
    }

    ESP_LOGI(TAG, "App HTTP server started on port 80");
}

void app_server_stop(void)
{
    if (s_app_httpd) {
        httpd_stop(s_app_httpd);
        s_app_httpd = NULL;
    }
}

static void http_server_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

/* =======================================================================
 *  SOFTAP + PORTAL START
 * =======================================================================*/

static void portal_start(void)
{
    /* Make sure STA is fully idle before we allow scans.
     * If we just failed a saved-credential attempt, STA may still be in
     * CONNECTING state; esp_wifi_scan_start() will return ESP_ERR_WIFI_STATE
     * until this call completes. */
    esp_wifi_disconnect();

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, s_cfg.ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(s_cfg.ap_ssid);
    ap_cfg.ap.channel        = s_cfg.ap_channel ? s_cfg.ap_channel : 1;
    ap_cfg.ap.max_connection = WIFI_MANAGER_AP_MAX_STA;

    if (s_cfg.ap_password && strlen(s_cfg.ap_password) >= 8) {
        strncpy((char *)ap_cfg.ap.password, s_cfg.ap_password,
                sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    ESP_LOGI(TAG, "SoftAP started: SSID=%s", s_cfg.ap_ssid);

    if (!s_scan_mutex) {
        s_scan_mutex = xSemaphoreCreateMutex();
    }
    if (!s_scan_task) {
        /* Stack 4 kB: scan_task calls esp_wifi_scan_start (blocking) and
         * does a small heap alloc for AP records -- 4 kB is sufficient. */
        xTaskCreate(scan_task, "wifi_mgr_scan", 4096, NULL, 3, &s_scan_task);
    }
    if (!s_dns_task) {
        xTaskCreate(dns_task, "wifi_mgr_dns", 4096, NULL, 5, &s_dns_task);
    }
    if (!s_httpd) {
        http_server_start();
    }

    mgr_event_t ev = {.state = WIFI_MANAGER_STATE_AP_STARTED};
    xQueueSend(s_event_queue, &ev, 0);
}

/* =======================================================================
 *  NVS HELPERS
 * =======================================================================*/

static bool nvs_load_credentials(char *ssid, size_t ssid_sz,
                                  char *pass, size_t pass_sz)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    bool ok = (nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_sz) == ESP_OK)
              && (ssid[0] != '\0');
    /* Password is optional -- open networks have no password stored. */
    nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_sz);

    nvs_close(nvs);
    ESP_LOGI(TAG, "NVS credentials: %s", ok ? "found" : "not found");
    return ok;
}

/* =======================================================================
 *  WIFI EVENT HANDLER
 *  Runs in the WiFi/lwIP system task -- must not call user code or block.
 *  All it does is push mgr_event_t messages onto the queue for event_task.
 * =======================================================================*/

static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {

        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            if (s_retry_count < s_cfg.sta_max_retries) {
                s_retry_count++;
                ESP_LOGI(TAG, "STA retry %d/%d", s_retry_count, s_cfg.sta_max_retries);
                esp_wifi_connect();
            } else {
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

        default:
            break;
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
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

/* =======================================================================
 *  EVENT DISPATCH TASK
 *  Runs user callbacks from a predictable task context, not from the
 *  WiFi event handler.  display_* calls and other driver APIs are safe here.
 * =======================================================================*/

static void event_task(void *arg)
{
    mgr_event_t ev;
    while (xQueueReceive(s_event_queue, &ev, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "state -> %d", ev.state);
        if (s_cfg.on_state_change) {
            s_cfg.on_state_change(ev.state, s_cfg.cb_ctx);
        }
        if (ev.state == WIFI_MANAGER_STATE_STA_CONNECTED) {
            app_server_start();  /* start robot UI on port 80 */
            if (s_cfg.on_connected) {
                s_cfg.on_connected(ev.ssid, ev.ip, s_cfg.cb_ctx);
            }
        }
    }
}

/* =======================================================================
 *  PUBLIC API
 * =======================================================================*/

esp_err_t wifi_manager_start(const wifi_manager_config_t *config)
{
    if (!config || !config->ap_ssid) return ESP_ERR_INVALID_ARG;
    memcpy(&s_cfg, config, sizeof(s_cfg));

    /* NVS -- init here; callers should not need to call nvs_flash_init() themselves. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition dirty, erasing");
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Mount LittleFS before any server starts, so both the portal path
     * and the saved-credentials fast path can serve files. */
    fs_mount();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL);

    ESP_ERROR_CHECK(esp_wifi_start());

    s_sta_evt_grp = xEventGroupCreate();
    s_event_queue = xQueueCreate(10, sizeof(mgr_event_t));
    xTaskCreate(event_task, "wifi_mgr_evt", 4096, NULL, 4, &s_event_task);

    /* -- Try stored credentials first -------------------------------- */
    char saved_ssid[33] = {0};
    char saved_pass[64] = {0};

    if (nvs_load_credentials(saved_ssid, sizeof(saved_ssid),
                             saved_pass, sizeof(saved_pass))) {
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

        /* Wait up to 10 s for the connection attempt to resolve. */
        EventBits_t bits = xEventGroupWaitBits(
            s_sta_evt_grp,
            EVT_STA_CONNECTED | EVT_STA_FAILED,
            pdTRUE,   /* clear bits on return */
            pdFALSE,  /* wait for either bit  */
            pdMS_TO_TICKS(10000));

        if (bits & EVT_STA_CONNECTED) {
            app_server_start();  /* start robot UI on port 80 */
            return ESP_OK;  /* connected -- skip portal entirely */
        }

        /* Credential attempt failed or timed out.  Reset retry counter
         * and fall through to the portal. */
        /* 1. Stop the failing connection attempt immediately */
        esp_wifi_disconnect();

        /* 2. Clear the retry count to unlock the scan_task */
        s_retry_count = 0;
        s_sta_failed  = false;

        /* Force a full radio state clear by toggling modes.
         * This kicks the driver out of the failed-connect loop. */
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

    /* Stop the current HTTP server and DNS task so portal_start() can
     * restart them cleanly.  (If they weren't running, these are no-ops.) */
    http_server_stop();

    portal_start();
    return ESP_OK;
}

esp_err_t wifi_manager_erase_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
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
