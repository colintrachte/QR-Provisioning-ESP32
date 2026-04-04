/**
 * wifi_prov.c — SoftAP captive portal WiFi provisioning.
 *
 * Components:
 *   - esp_wifi in APSTA mode (AP for portal, STA for target network)
 *   - lwip DNS server (all queries → 192.168.4.1 → triggers captive pop-up)
 *   - esp_http_server serving:
 *       GET  /          — portal HTML page
 *       GET  /scan      — JSON list of nearby APs
 *       POST /connect   — submit SSID + password
 *       GET  /status    — connection result polling
 *       GET  *          — catch-all redirect to / (captive portal magic)
 *   - NVS for credential persistence
 *   - Internal event queue so callbacks run on a safe task, not in ISRs
 */

#include "wifi_prov.h"

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

static const char *TAG = "wifi_prov";

/* ── NVS keys ────────────────────────────────────────────────────────────*/
#define NVS_NS      "prov"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

/* ── Event bits ──────────────────────────────────────────────────────────*/
#define STA_CONNECTED_BIT BIT0
#define STA_FAILED_BIT    BIT1

/* ── Internal event type for the callback dispatcher task ────────────────*/
typedef struct {
    wifi_prov_state_t state;
    char ssid[33];
    char ip[16];
} prov_event_t;

/* ── Module state ────────────────────────────────────────────────────────*/
static wifi_prov_config_t  s_cfg;
static httpd_handle_t      s_httpd       = NULL;
static EventGroupHandle_t  s_sta_events  = NULL;
static QueueHandle_t       s_event_queue = NULL;
static TaskHandle_t        s_dns_task    = NULL;
static TaskHandle_t        s_cb_task     = NULL;
static bool                s_connected   = false;
static char                s_ip[16]      = {0};
static int                 s_retry_count = 0;
static char                s_target_ssid[33] = {0};
static char                s_target_pass[64] = {0};

/* ── DNS server ──────────────────────────────────────────────────────────*/
#define DNS_PORT 53

/* Minimal DNS response: redirect everything to 192.168.4.1 */
static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "DNS socket failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed"); close(sock); vTaskDelete(NULL); return;
    }

    ESP_LOGI(TAG, "DNS server listening on port 53");

    static uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;  /* too short to be a valid DNS query */

        /* Build a minimal DNS A-record response pointing to 192.168.4.1 */
        uint8_t resp[512];
        memcpy(resp, buf, len);           /* copy question section */
        resp[2] = 0x81; resp[3] = 0x80;  /* QR=1, AA=1, RCODE=0  */
        resp[6] = 0x00; resp[7] = 0x01;  /* ANCOUNT = 1           */
        resp[8] = 0x00; resp[9] = 0x00;  /* NSCOUNT = 0           */
        resp[10]= 0x00; resp[11]= 0x00;  /* ARCOUNT = 0           */

        int resp_len = len;
        /* Append answer: pointer to question name, type A, class IN, TTL, RDLEN, RDATA */
        resp[resp_len++] = 0xC0; resp[resp_len++] = 0x0C; /* name ptr */
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x01; /* type A   */
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x01; /* class IN */
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x00;
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x3C; /* TTL 60s  */
        resp[resp_len++] = 0x00; resp[resp_len++] = 0x04; /* RDLEN 4  */
        /* 192.168.4.1 */
        resp[resp_len++] = 192; resp[resp_len++] = 168;
        resp[resp_len++] = 4;   resp[resp_len++] = 1;

        sendto(sock, resp, resp_len, 0, (struct sockaddr *)&client, client_len);
    }
}

/* ── Portal HTML (stored in flash) ──────────────────────────────────────*/
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Robot WiFi Setup</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;"
"display:flex;align-items:center;justify-content:center;padding:16px}"
".card{background:#16213e;border-radius:12px;padding:24px;width:100%;max-width:400px;"
"box-shadow:0 4px 24px rgba(0,0,0,.4)}"
"h1{font-size:1.4rem;margin-bottom:4px;color:#e94560}"
"p.sub{font-size:.85rem;color:#888;margin-bottom:20px}"
"label{display:block;font-size:.8rem;color:#aaa;margin-bottom:4px;margin-top:12px}"
"select,input{width:100%;padding:10px 12px;border-radius:8px;border:1px solid #333;"
"background:#0f3460;color:#eee;font-size:1rem}"
"button{margin-top:20px;width:100%;padding:12px;border:none;border-radius:8px;"
"background:#e94560;color:#fff;font-size:1rem;font-weight:600;cursor:pointer}"
"button:disabled{background:#555;cursor:not-allowed}"
"#status{margin-top:16px;padding:10px;border-radius:8px;font-size:.9rem;display:none}"
".ok{background:#1a4731;color:#4ade80}.err{background:#4c1a1a;color:#f87171}"
".info{background:#1a3a4c;color:#93c5fd}"
"#spinner{display:none;text-align:center;margin-top:16px;color:#888}"
"</style></head><body>"
"<div class='card'>"
"<h1>&#129302; Robot Setup</h1>"
"<p class='sub'>Connect this device to your WiFi network.</p>"
"<label>Select network</label>"
"<select id='ssid'><option value=''>Scanning...</option></select>"
"<label>Password</label>"
"<input type='password' id='pass' placeholder='WiFi password'>"
"<button id='btn' onclick='connect()'>Connect</button>"
"<div id='spinner'>Connecting&hellip;</div>"
"<div id='status'></div>"
"</div>"
"<script>"
"async function scan(){"
"  try{"
"    const r=await fetch('/scan');"
"    const nets=await r.json();"
"    const sel=document.getElementById('ssid');"
"    sel.innerHTML=nets.map(n=>"
"      `<option value='${n.ssid}'>${n.ssid} (${n.rssi} dBm)${n.auth?' \xf0\x9f\x94\x92':''}</option>`"
"    ).join('');"
"  }catch(e){console.error('scan failed',e);}"
"}"
"async function connect(){"
"  const ssid=document.getElementById('ssid').value;"
"  const pass=document.getElementById('pass').value;"
"  if(!ssid){showStatus('Please select a network.','err');return;}"
"  document.getElementById('btn').disabled=true;"
"  document.getElementById('spinner').style.display='block';"
"  showStatus('Sending credentials...','info');"
"  try{"
"    const r=await fetch('/connect',{method:'POST',"
"      headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"      body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)});"
"    if(r.ok){poll();}"
"    else{showStatus('Error sending credentials.','err');reset();}"
"  }catch(e){showStatus('Network error.','err');reset();}"
"}"
"async function poll(){"
"  showStatus('Connecting to network, please wait...','info');"
"  for(let i=0;i<20;i++){"
"    await new Promise(r=>setTimeout(r,1500));"
"    try{"
"      const r=await fetch('/status');"
"      const d=await r.json();"
"      if(d.connected){showStatus('Connected! IP: '+d.ip,'ok');return;}"
"      if(d.failed){showStatus('Connection failed. Check password and try again.','err');reset();return;}"
"    }catch(e){}"
"  }"
"  showStatus('Timed out. The device may have connected. Check your router.','info');"
"  reset();"
"}"
"function showStatus(msg,cls){"
"  const el=document.getElementById('status');"
"  el.textContent=msg;el.className=cls;el.style.display='block';"
"}"
"function reset(){"
"  document.getElementById('btn').disabled=false;"
"  document.getElementById('spinner').style.display='none';"
"}"
"scan();"
"</script></html>";

/* ── URL decode helper ───────────────────────────────────────────────────*/

static void url_decode(const char *src, char *dst, int maxlen)
{
    int di = 0;
    for (int i = 0; src[i] && di < maxlen - 1; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

/* ── HTTP handlers ───────────────────────────────────────────────────────*/

/* GET / — serve portal page */
static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, strlen(PORTAL_HTML));
    return ESP_OK;
}

/* Redirect handler for captive portal detection URLs */
static esp_err_t handler_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* GET /scan — return JSON array of nearby APs */
static esp_err_t handler_scan(httpd_req_t *req)
{
    /* Kick a scan and wait for results */
    esp_wifi_scan_start(NULL, true);  /* blocking scan */
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!records) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&ap_count, records);

    /* Build JSON manually to avoid cJSON dependency */
    char *json = malloc(ap_count * 80 + 8);
    if (!json) { free(records); httpd_resp_send_500(req); return ESP_OK; }

    int pos = 0;
    pos += sprintf(json + pos, "[");
    for (int i = 0; i < ap_count; i++) {
        /* Escape SSID quotes (basic) */
        char safe_ssid[33*2+1];
        int si = 0;
        for (int j = 0; records[i].ssid[j] && j < 32; j++) {
            char c = (char)records[i].ssid[j];
            if (c == '"' || c == '\\') safe_ssid[si++] = '\\';
            safe_ssid[si++] = c;
        }
        safe_ssid[si] = '\0';

        pos += sprintf(json + pos,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%s}",
            i ? "," : "",
            safe_ssid,
            records[i].rssi,
            records[i].authmode != WIFI_AUTH_OPEN ? "true" : "false"
        );
    }
    pos += sprintf(json + pos, "]");

    free(records);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, pos);
    free(json);
    return ESP_OK;
}

/* POST /connect — receive SSID + password, attempt connection */
static esp_err_t handler_connect(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) { httpd_resp_send_500(req); return ESP_OK; }
    body[received] = '\0';

    /* Parse URL-encoded body: ssid=...&pass=... */
    char ssid[33] = {0};
    char pass[64] = {0};

    /* Extract ssid= */
    char *p = strstr(body, "ssid=");
    if (p) {
        char raw[64] = {0};
        p += 5;
        char *end = strchr(p, '&');
        int len = end ? (int)(end - p) : (int)strlen(p);
        if (len > 63) len = 63;
        memcpy(raw, p, len);
        url_decode(raw, ssid, sizeof(ssid));
    }

    /* Extract pass= */
    p = strstr(body, "pass=");
    if (p) {
        char raw[128] = {0};
        p += 5;
        char *end = strchr(p, '&');
        int len = end ? (int)(end - p) : (int)strlen(p);
        if (len > 127) len = 127;
        memcpy(raw, p, len);
        url_decode(raw, pass, sizeof(pass));
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Received credentials for SSID: %s", ssid);

    /* Save to NVS */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_SSID, ssid);
        nvs_set_str(nvs, NVS_KEY_PASS, pass);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    /* Copy to module state for STA connection attempt */
    strncpy(s_target_ssid, ssid, sizeof(s_target_ssid) - 1);
    strncpy(s_target_pass, pass, sizeof(s_target_pass) - 1);
    s_retry_count = 0;

    /* Notify via state event */
    prov_event_t ev = { .state = WIFI_PROV_STATE_CREDS_RECEIVED };
    strncpy(ev.ssid, ssid, sizeof(ev.ssid) - 1);
    xQueueSend(s_event_queue, &ev, 0);

    /* Start STA connection attempt */
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    /* Signal connecting state */
    prov_event_t ev2 = { .state = WIFI_PROV_STATE_STA_CONNECTING };
    strncpy(ev2.ssid, ssid, sizeof(ev2.ssid) - 1);
    xQueueSend(s_event_queue, &ev2, 0);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 11);
    return ESP_OK;
}

/* GET /status — poll connection result */
static bool s_sta_failed = false;

static esp_err_t handler_status(httpd_req_t *req)
{
    char resp[64];
    if (s_connected) {
        snprintf(resp, sizeof(resp), "{\"connected\":true,\"ip\":\"%s\"}", s_ip);
    } else if (s_sta_failed) {
        snprintf(resp, sizeof(resp), "{\"connected\":false,\"failed\":true}");
    } else {
        snprintf(resp, sizeof(resp), "{\"connected\":false,\"failed\":false}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* ── HTTP server start/stop ──────────────────────────────────────────────*/

static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root = { .uri="/",        .method=HTTP_GET,  .handler=handler_root     };
    httpd_uri_t scan = { .uri="/scan",    .method=HTTP_GET,  .handler=handler_scan     };
    httpd_uri_t conn = { .uri="/connect", .method=HTTP_POST, .handler=handler_connect  };
    httpd_uri_t stat = { .uri="/status",  .method=HTTP_GET,  .handler=handler_status   };
    /* Catch-all for captive portal detection (msftconnecttest, connectivitycheck, etc.) */
    httpd_uri_t wild = { .uri="/*",       .method=HTTP_GET,  .handler=handler_redirect };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &conn);
    httpd_register_uri_handler(s_httpd, &stat);
    httpd_register_uri_handler(s_httpd, &wild);

    ESP_LOGI(TAG, "HTTP server started on port 80");
}

static void stop_http_server(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

/* ── WiFi event handler ──────────────────────────────────────────────────*/

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            if (s_retry_count < s_cfg.sta_max_retries) {
                s_retry_count++;
                ESP_LOGI(TAG, "STA retry %d/%d", s_retry_count, s_cfg.sta_max_retries);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "STA connection failed after %d retries", s_retry_count);
                s_sta_failed = true;
                xEventGroupSetBits(s_sta_events, STA_FAILED_BIT);
                prov_event_t ev = { .state = WIFI_PROV_STATE_STA_FAILED };
                xQueueSend(s_event_queue, &ev, 0);
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            prov_event_t ev = { .state = WIFI_PROV_STATE_CLIENT_CONNECTED };
            xQueueSend(s_event_queue, &ev, 0);
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            prov_event_t ev = { .state = WIFI_PROV_STATE_CLIENT_GONE };
            xQueueSend(s_event_queue, &ev, 0);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected = true;
        s_sta_failed = false;
        xEventGroupSetBits(s_sta_events, STA_CONNECTED_BIT);

        /* Save confirmed credentials (already saved on submit, this is belt-and-suspenders) */
        ESP_LOGI(TAG, "STA connected, IP: %s", s_ip);

        prov_event_t ev = { .state = WIFI_PROV_STATE_STA_CONNECTED };
        strncpy(ev.ssid, s_target_ssid, sizeof(ev.ssid) - 1);
        strncpy(ev.ip,   s_ip,          sizeof(ev.ip)   - 1);
        xQueueSend(s_event_queue, &ev, 0);
    }
}

/* ── Callback dispatcher task ────────────────────────────────────────────*/

static void cb_dispatcher_task(void *arg)
{
    prov_event_t ev;
    while (1) {
        if (xQueueReceive(s_event_queue, &ev, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "State: %d", ev.state);
            if (s_cfg.on_state_change) {
                s_cfg.on_state_change(ev.state, s_cfg.cb_ctx);
            }
            if (ev.state == WIFI_PROV_STATE_STA_CONNECTED && s_cfg.on_connected) {
                s_cfg.on_connected(ev.ssid, ev.ip, s_cfg.cb_ctx);
            }
        }
    }
}

/* ── SoftAP start ────────────────────────────────────────────────────────*/

static void start_softap(void)
{
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, s_cfg.ap_ssid,
            sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(s_cfg.ap_ssid);
    ap_cfg.ap.channel  = s_cfg.ap_channel ? s_cfg.ap_channel : 1;
    ap_cfg.ap.max_connection = 4;

    if (s_cfg.ap_password && strlen(s_cfg.ap_password) >= 8) {
        strncpy((char *)ap_cfg.ap.password, s_cfg.ap_password,
                sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    ESP_LOGI(TAG, "SoftAP started: SSID=%s", s_cfg.ap_ssid);

    /* Start DNS and HTTP */
    xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, &s_dns_task);
    start_http_server();

    prov_event_t ev = { .state = WIFI_PROV_STATE_AP_STARTED };
    xQueueSend(s_event_queue, &ev, 0);
}

/* ── NVS credential load ─────────────────────────────────────────────────*/

static bool load_credentials(char *ssid, size_t ssid_len,
                              char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return false;

    esp_err_t e1 = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
    nvs_close(nvs);

    bool ok = (e1 == ESP_OK && ssid[0] != '\0');
    ESP_LOGI(TAG, "NVS credentials %s (ssid='%s')", ok ? "found" : "not found", ok ? ssid : "");
    (void)e2;
    return ok;
}

/* ── Public API ──────────────────────────────────────────────────────────*/

esp_err_t wifi_prov_start(const wifi_prov_config_t *config)
{
    if (!config || !config->ap_ssid) return ESP_ERR_INVALID_ARG;
    memcpy(&s_cfg, config, sizeof(s_cfg));

    /* NVS init (idempotent — app_main may have called it already) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Network / event infrastructure */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,  &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Create event queue and dispatcher task */
    s_sta_events  = xEventGroupCreate();
    s_event_queue = xQueueCreate(10, sizeof(prov_event_t));
    xTaskCreate(cb_dispatcher_task, "prov_cb", 4096, NULL, 4, &s_cb_task);

    /* Attempt connection with saved credentials */
    char saved_ssid[33] = {0};
    char saved_pass[64] = {0};

    if (load_credentials(saved_ssid, sizeof(saved_ssid),
                         saved_pass, sizeof(saved_pass))) {
        strncpy(s_target_ssid, saved_ssid, sizeof(s_target_ssid) - 1);
        strncpy(s_target_pass, saved_pass, sizeof(s_target_pass) - 1);

        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, saved_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, saved_pass, sizeof(sta_cfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_connect();

        prov_event_t ev = { .state = WIFI_PROV_STATE_CONNECTING_SAVED };
        strncpy(ev.ssid, saved_ssid, sizeof(ev.ssid) - 1);
        xQueueSend(s_event_queue, &ev, 0);

        /* Wait up to 10s for connection before falling through to portal */
        EventBits_t bits = xEventGroupWaitBits(s_sta_events,
            STA_CONNECTED_BIT | STA_FAILED_BIT, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(10000));

        if (bits & STA_CONNECTED_BIT) {
            /* Already connected — no portal needed */
            return ESP_OK;
        }
        /* Fall through: connection failed, start portal */
        s_retry_count = 0;
        s_sta_failed  = false;
    }

    /* No credentials or connection failed — start portal */
    start_softap();
    return ESP_OK;
}

esp_err_t wifi_prov_reset_and_restart(void)
{
    wifi_prov_erase_credentials();
    esp_wifi_disconnect();
    s_connected   = false;
    s_sta_failed  = false;
    s_retry_count = 0;
    start_softap();
    return ESP_OK;
}

esp_err_t wifi_prov_erase_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_SSID);
        nvs_erase_key(nvs, NVS_KEY_PASS);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Credentials erased");
    }
    return ret;
}

bool wifi_prov_is_connected(void)  { return s_connected; }
const char *wifi_prov_get_ip(void) { return s_connected ? s_ip : NULL; }
