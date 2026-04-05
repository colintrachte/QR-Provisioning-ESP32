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

static const char *TAG = "wifi_mgr";

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

                /* JSON-escape the SSID (only ' " ' and ' \ ' need escaping). */
                int si = 0;
                for (int j = 0; recs[i].ssid[j] && j < 32 && si < 63; j++) {
                    char c = (char)recs[i].ssid[j];
                    if (c == '"' || c == '\\') s_scan_aps[s_scan_count].ssid[si++] = '\\';
                    s_scan_aps[s_scan_count].ssid[si++] = c;
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

        int rlen = len;
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
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<title>WiFi Setup</title>"
"<style>"
"*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,sans-serif;background:#0f1117;color:#e4e6f0;"
"min-height:100dvh;display:flex;align-items:center;justify-content:center;padding:24px}"
".card{width:100%;max-width:380px;background:#1c1f2b;border:1px solid #2e3347;"
"border-radius:14px;padding:32px 28px}"
".logo{font-size:2rem;text-align:center;margin-bottom:6px}"
"h1{font-size:1.1rem;font-weight:700;text-align:center;letter-spacing:.04em;"
"text-transform:uppercase;margin-bottom:4px}"
".subtitle{font-size:.78rem;color:#4a4f6a;text-align:center;margin-bottom:24px}"
".section-label{font-size:.72rem;font-weight:600;text-transform:uppercase;"
"letter-spacing:.05em;color:#4a4f6a;margin-bottom:8px}"
"#network-list{display:flex;flex-direction:column;gap:6px;margin-bottom:20px;"
"max-height:220px;overflow-y:auto}"
".network-btn{display:flex;align-items:center;justify-content:space-between;"
"padding:10px 12px;background:#0f1117;border:1px solid #2e3347;border-radius:8px;"
"color:#e4e6f0;font-size:.9rem;cursor:pointer;text-align:left;"
"transition:border-color .15s,background .15s;width:100%;font-family:inherit}"
".network-btn:hover,.network-btn.selected{border-color:#3b82f6;background:#13172a}"
".network-name{font-weight:500}"
".network-meta{display:flex;align-items:center;gap:6px;flex-shrink:0}"
".signal{font-size:.8rem;color:#4a4f6a}"
".scan-status{font-size:.78rem;color:#4a4f6a;text-align:center;padding:12px 0}"
".rescan-btn{background:none;border:1px solid #2e3347;border-radius:6px;"
"color:#4a4f6a;font-size:.72rem;font-family:inherit;padding:4px 10px;"
"cursor:pointer;margin-left:8px;transition:border-color .15s,color .15s}"
".rescan-btn:hover{border-color:#3b82f6;color:#e4e6f0}"
"label{display:block;font-size:.72rem;font-weight:600;text-transform:uppercase;"
"letter-spacing:.05em;color:#4a4f6a;margin-bottom:6px}"
"input[type=text],input[type=password]{width:100%;padding:10px 12px;"
"background:#0f1117;border:1px solid #2e3347;border-radius:8px;color:#e4e6f0;"
"font-size:1rem;font-family:inherit;outline:none;transition:border-color .15s;"
"margin-bottom:16px}"
"input:focus{border-color:#3b82f6}"
".pass-wrap{position:relative;margin-bottom:24px}"
".pass-wrap input{margin-bottom:0;padding-right:44px}"
".toggle-pass{position:absolute;right:12px;top:50%;transform:translateY(-50%);"
"background:none;border:none;color:#4a4f6a;cursor:pointer;font-size:1rem;padding:4px}"
".toggle-pass:hover{color:#e4e6f0}"
"button[type=submit]{width:100%;padding:12px;background:#3b82f6;color:#fff;"
"border:none;border-radius:8px;font-size:1rem;font-weight:600;font-family:inherit;"
"cursor:pointer;transition:background .15s,opacity .15s}"
"button[type=submit]:hover{background:#2563eb}"
"button[type=submit]:disabled{opacity:.5;cursor:default}"
".status{margin-top:14px;font-size:.8rem;text-align:center;color:#4a4f6a;min-height:1.2em}"
".status.error{color:#ef4444}"
".status.ok{color:#4ade80}"
".hint{margin-top:22px;padding-top:18px;border-top:1px solid #2e3347;"
"font-size:.72rem;color:#4a4f6a;text-align:center;line-height:1.7}"
".hint strong{color:#e4e6f0}"
"</style></head><body><div class='card'>"
"<div class='logo'>&#128246;</div>"
"<h1>WiFi Setup</h1>"
"<p class='subtitle'>Connect this device to your network</p>"
"<p class='section-label'>Available networks"
"<button class='rescan-btn' onclick='startScan()'>&#8635; Rescan</button></p>"
"<div id='network-list'><p class='scan-status'>Scanning&hellip;</p></div>"
"<form id='setup-form'>"
"<label for='ssid'>Network name</label>"
"<input type='text' id='ssid' name='ssid' placeholder='Or type manually'"
" autocomplete='off' autocorrect='off' autocapitalize='none' spellcheck='false' required>"
"<label for='pass'>Password</label>"
"<div class='pass-wrap'>"
"<input type='password' id='pass' name='pass' placeholder='Leave blank if open network'>"
"<button type='button' class='toggle-pass' aria-label='Show/hide password'>&#128065;</button>"
"</div>"
"<button type='submit' id='submit-btn'>Save &amp; Connect</button>"
"<p class='status' id='status'></p>"
"</form>"
"<p class='hint'>After saving, the device will reboot and join your network.<br>"
"To reconfigure: hold BOOT 3&nbsp;s at power-up.</p>"
"</div><script>"

/* -- Helpers -- */
"function escHtml(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"
"function signalBars(q){"
"if(q>80)return'\u2582\u2584\u2586\u2588';"
"if(q>60)return'\u2582\u2584\u2586\xb7';"
"if(q>40)return'\u2582\u2584\xb7\xb7';"
"return'\u2582\xb7\xb7\xb7';}"

/* -- Network list -- */
"function renderNetworks(networks){"
"var listEl=document.getElementById('network-list');"
"listEl.innerHTML='';"
"if(!networks||!networks.length){"
"listEl.innerHTML='<p class=\"scan-status\">No networks found."
" <button class=\"rescan-btn\" onclick=\"startScan()\">Rescan</button></p>';return;}"
"networks.sort(function(a,b){return b.quality-a.quality;});"
"networks.forEach(function(net){"
"var btn=document.createElement('button');"
"btn.type='button';btn.className='network-btn';"
"btn.innerHTML="
"'<span class=\"network-name\">'+escHtml(net.ssid)+'</span>'"
"+'<span class=\"network-meta\">'"
"+'<span class=\"signal\">'+signalBars(net.quality)+' '+net.quality+'%</span>'"
"+(net.secure?'<span>&#128274;</span>':'')+'</span>';"
"btn.addEventListener('click',function(){"
"listEl.querySelectorAll('.network-btn').forEach(function(b){b.classList.remove('selected');});"
"btn.classList.add('selected');"
"document.getElementById('ssid').value=net.ssid;"
"if(net.secure)document.getElementById('pass').focus();"
"else document.getElementById('pass').value='';});"
"listEl.appendChild(btn);});}"

/* -- Scan: poll GET /scan until server returns an array -- */
"var scanPoll=null;"
"function pollScan(){"
"fetch('/scan')"
".then(function(r){return r.ok?r.json():Promise.reject(r.status);})"
".then(function(d){"
"if(Array.isArray(d)){renderNetworks(d);}"
"else{scanPoll=setTimeout(pollScan,1500);}})" // Changed ); to })
".catch(function(){scanPoll=setTimeout(pollScan,3000);});}"
"function startScan(){"
"document.getElementById('network-list').innerHTML='<p class=\"scan-status\">Scanning&hellip;</p>';"
"clearTimeout(scanPoll);"
"pollScan();}"

/* -- Password toggle -- */
"document.querySelector('.toggle-pass').addEventListener('click',function(){"
"var p=document.getElementById('pass');"
"p.type=p.type==='password'?'text':'password';});"

/* -- Save flow -- borrowed from the 8266 project:
 *   POST /save -> device saves to NVS and reboots.
 *   The AP disappears, the fetch() catch fires, and we redirect
 *   the user to the device's normal hostname. No /connect-status
 *   polling required -- the reboot is the connection attempt. -- */
"document.getElementById('setup-form').addEventListener('submit',function(e){"
"e.preventDefault();"
"var ssid=document.getElementById('ssid').value.trim();"
"if(!ssid)return;"
"var statusEl=document.getElementById('status');"
"var btn=document.getElementById('submit-btn');"
"btn.disabled=true;btn.textContent='Saving...';"
"statusEl.textContent='Sending credentials...';"
"statusEl.className='status';"
"var body='ssid='+encodeURIComponent(ssid)"
"+'&pass='+encodeURIComponent(document.getElementById('pass').value);"
"fetch('/save',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
".then(function(){"
"statusEl.textContent='Saved! Device is rebooting and joining your network...';"
"statusEl.className='status ok';})"
".catch(function(){"
/* fetch error = device rebooted and AP is gone = expected success path */
"statusEl.textContent='Device rebooting -- check your normal WiFi in a moment.';"
"statusEl.className='status ok';});});"

/* -- Boot -- */
"startScan();"
"</script></html>";

/* =======================================================================
 *  HELPERS
 * =======================================================================*/

/**
 * Decode a percent-encoded URL query value into dst.
 * '+' is decoded as space (application/x-www-form-urlencoded convention).
 */
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

/**
 * Extract the value of a URL-encoded form field from a raw POST body.
 * e.g. extract_field("ssid=Foo&pass=Bar", "ssid", out, sizeof(out))
 * writes "Foo" into out.
 */
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

    char raw[256] = {0};
    if (len > (int)sizeof(raw) - 1) len = (int)sizeof(raw) - 1;
    memcpy(raw, p, len);
    url_decode(raw, out, out_size);
}

/* =======================================================================
 *  HTTP HANDLERS -- application endpoints
 * =======================================================================*/

/** GET / -- serve the portal page. */
static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * GET /scan -- return cached WiFi scan results as a JSON array.
 *
 * Results are populated by scan_task(), which runs a blocking scan on its
 * own FreeRTOS task every SCAN_INTERVAL_MS milliseconds.  This handler
 * never touches the WiFi driver -- it only reads the shared cache under a
 * mutex, so it is safe to call from the httpd task at any time.
 *
 * Returns {"status":"scanning"} until the first scan completes.
 * Returns a JSON array of {ssid, quality, secure} objects thereafter.
 */
static esp_err_t handle_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (!s_scan_ready) {
        httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
        return ESP_OK;
    }

    /* Lock, serialise the cache to JSON, unlock. */
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    /* Upper bound: each entry is at most ~90 bytes + 2 for [] */
    char *json = malloc(s_scan_count * 90 + 4);
    if (!json) {
        xSemaphoreGive(s_scan_mutex);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int pos = 0;
    pos += sprintf(json + pos, "[");
    for (int i = 0; i < s_scan_count; i++) {
        pos += sprintf(json + pos,
            "%s{\"ssid\":\"%s\",\"quality\":%d,\"secure\":%s}",
            i ? "," : "",
            s_scan_aps[i].ssid,
            s_scan_aps[i].quality,
            s_scan_aps[i].secure ? "true" : "false");
    }
    pos += sprintf(json + pos, "]");

    xSemaphoreGive(s_scan_mutex);

    httpd_resp_send(req, json, pos);
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

    /* Short delay then reboot from a separate task so the HTTP response
     * has time to be flushed through the TCP stack. */
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();

    return ESP_OK;  /* unreachable, but satisfies the compiler */
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
        {.uri="/",               .method=HTTP_GET,  .handler=handle_root           },
        {.uri="/scan",           .method=HTTP_GET,  .handler=handle_scan           },
        {.uri="/save",           .method=HTTP_POST, .handler=handle_save           },

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
        if (ev.state == WIFI_MANAGER_STATE_STA_CONNECTED && s_cfg.on_connected) {
            s_cfg.on_connected(ev.ssid, ev.ip, s_cfg.cb_ctx);
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
