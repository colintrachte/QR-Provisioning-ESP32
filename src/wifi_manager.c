/**
 * wifi_manager.c — STA/AP WiFi state machine and NVS credential storage.
 *
 * Manager task flow
 * ─────────────────
 *   wifi_manager_start() spawns mgr_task and returns immediately.
 *
 *   mgr_task:
 *     [NVS creds found] → fire CONNECTING_SAVED → attempt STA connect
 *                       → wait up to 10 s for EVT_STA_CONNECTED
 *                       → on success: fire STA_CONNECTED callback, done.
 *                       → on failure: fall through to portal.
 *
 *     [no creds / STA failed] → tear down STA → configure SoftAP → fire AP_STARTED
 *                              → portal_start() [blocks until POST /api/connect]
 *                              → portal_stop()
 *                              → save credentials to NVS
 *                              → fire CREDS_RECEIVED, STA_CONNECTING
 *                              → attempt STA connect
 *                              → wait for EVT_STA_CONNECTED
 *                              → on success: fire STA_CONNECTED callback.
 *                              → on failure: fire STA_FAILED, restart portal.
 */

#include "wifi_manager.h"
#include "portal.h"
#include "app_server.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#if MDNS_ENABLE
#include "mdns.h"
#endif

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE "wifi_mgr"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define EVT_STA_CONNECTED BIT0
#define EVT_STA_FAILED    BIT1

#define STA_CONNECT_TIMEOUT_MS 10000

static wifi_manager_config_t  s_cfg;
static EventGroupHandle_t     s_sta_evt_grp = NULL;
static bool                   s_connected   = false;
static volatile bool          s_ap_shutting_down = false;  /* suppress AP events during intentional shutdown */
static char                   s_ip[16]      = {0};
static char                   s_sta_ssid[33] = {0};
static volatile int           s_retry_count  = 0;

static bool nvs_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    bool ok = (nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK)
              && (ssid[0] != '\0');
    nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
    nvs_close(nvs);

    ESP_LOGI(TAG, "NVS credentials: %s", ok ? "found" : "not found");
    return ok;
}

static void nvs_save(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed — credentials not saved");
        return;
    }
    nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_PASS, pass);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Credentials saved to NVS (SSID=%s)", ssid);
}

static void fire_state(wifi_manager_state_t state)
{
    ESP_LOGI(TAG, "state → %d", (int)state);
    if (s_cfg.on_state_change)
        s_cfg.on_state_change(state, s_cfg.cb_ctx);
}

static void fire_connected(void)
{
    if (s_cfg.on_connected)
        s_cfg.on_connected(s_sta_ssid, s_ip, s_cfg.cb_ctx);
}

static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_connected = false;
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "STA disconnected (reason %d), retry %d/%d",
                     disc->reason, s_retry_count, s_cfg.sta_max_retries);

            if (s_retry_count < s_cfg.sta_max_retries) {
                s_retry_count++;
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_sta_evt_grp, EVT_STA_FAILED);
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "AP: client joined");
            fire_state(WIFI_MANAGER_STATE_CLIENT_CONNECTED);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_shutting_down) {
                /* AP is being shut down intentionally — ignore client disconnects */
                break;
            }
            ESP_LOGI(TAG, "AP: client left");
            fire_state(WIFI_MANAGER_STATE_CLIENT_DISCONNECTED);
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "STA got IP: %s", s_ip);
        xEventGroupSetBits(s_sta_evt_grp, EVT_STA_CONNECTED);
    }
}

static void ap_configure(void)
{
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, s_cfg.ap_ssid,
            sizeof(ap_cfg.ap.ssid) - 1);
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
    ESP_LOGI(TAG, "SoftAP configured: SSID=%s ch=%d",
             s_cfg.ap_ssid, ap_cfg.ap.channel);
}

static bool sta_connect(const char *ssid, const char *pass)
{
    xEventGroupClearBits(s_sta_evt_grp, EVT_STA_CONNECTED | EVT_STA_FAILED);
    s_retry_count = 0;

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid,
            sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass,
            sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.pmf_cfg.capable  = true;
    sta_cfg.sta.pmf_cfg.required = false;

    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_sta_evt_grp,
        EVT_STA_CONNECTED | EVT_STA_FAILED,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));

    if (bits & EVT_STA_CONNECTED) {
        strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
        return true;
    }

    esp_wifi_disconnect();
    return false;
}

static void reset_wifi_for_ap(void)
{
    ESP_LOGI(TAG, "Resetting WiFi for AP mode");
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
}

static void mgr_task(void *arg)
{
    (void)arg;

    char ssid[33] = {0};
    char pass[65] = {0};

    /* ── Try stored credentials ────────────────────────────────────────────*/
    if (nvs_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        fire_state(WIFI_MANAGER_STATE_CONNECTING_SAVED);

        if (sta_connect(ssid, pass)) {
            fire_state(WIFI_MANAGER_STATE_STA_CONNECTED);
            fire_connected();

            app_server_start();
#if MDNS_ENABLE
            if (mdns_init() == ESP_OK) {
                mdns_hostname_set(MDNS_HOSTNAME);
                mdns_instance_name_set("MuleBot Robot");
                ESP_LOGI(TAG, "mDNS: %s.local", MDNS_HOSTNAME);
            }
#endif
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGW(TAG, "Saved credentials failed — falling back to portal");
    }

    /* ── Portal loop: repeat until STA connects ────────────────────────────*/
    reset_wifi_for_ap();
    ap_configure();
    fire_state(WIFI_MANAGER_STATE_AP_STARTED);

    while (1) {
        portal_start(s_cfg.ap_ssid);

        portal_stop();
        portal_get_credentials(ssid, pass);
        fire_state(WIFI_MANAGER_STATE_CREDS_RECEIVED);

        vTaskDelay(pdMS_TO_TICKS(300));

        fire_state(WIFI_MANAGER_STATE_STA_CONNECTING);

        if (sta_connect(ssid, pass)) {
            nvs_save(ssid, pass);

            fire_state(WIFI_MANAGER_STATE_STA_CONNECTED);
            fire_connected();

            app_server_start();
#if MDNS_ENABLE
            if (mdns_init() == ESP_OK) {
                mdns_hostname_set(MDNS_HOSTNAME);
                mdns_instance_name_set("MuleBot Robot");
                ESP_LOGI(TAG, "mDNS: %s.local", MDNS_HOSTNAME);
            }
#endif
            vTaskDelete(NULL);
            return;
        }

        fire_state(WIFI_MANAGER_STATE_STA_FAILED);
        ESP_LOGW(TAG, "STA connection failed — restarting portal");

        reset_wifi_for_ap();
        ap_configure();
        fire_state(WIFI_MANAGER_STATE_AP_STARTED);
    }
}

/* ── Public API ────────────────────────────────────────────────────────────*/

esp_err_t wifi_manager_start(const wifi_manager_config_t *config)
{
    if (!config || !config->ap_ssid) return ESP_ERR_INVALID_ARG;
    memcpy(&s_cfg, config, sizeof(s_cfg));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupt — erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t ni_err = esp_netif_init();
    if (ni_err != ESP_OK && ni_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(ni_err));
        return ni_err;
    }
    esp_err_t el_err = esp_event_loop_create_default();
    if (el_err != ESP_OK && el_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(el_err));
        return el_err;
    }
    static bool s_netif_created = false;
    if (!s_netif_created) {
        esp_netif_create_default_wifi_ap();
        esp_netif_create_default_wifi_sta();
        s_netif_created = true;
    }

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t wifi_err = esp_wifi_init(&wcfg);
    if (wifi_err != ESP_OK && wifi_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(wifi_err));
        return wifi_err;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               wifi_event_cb, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               wifi_event_cb, NULL);

    ESP_ERROR_CHECK(esp_wifi_start());

    s_sta_evt_grp = xEventGroupCreate();
    if (!s_sta_evt_grp) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(mgr_task, "wifi_mgr", 6144, NULL, 4, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "wifi_manager_start OK — mgr_task running");
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

esp_err_t wifi_manager_reset(void)
{
    wifi_manager_erase_credentials();
    ESP_LOGW(TAG, "wifi_manager_reset — restarting");
    esp_restart();
    return ESP_OK;
}

bool        wifi_manager_is_connected(void) { return s_connected; }
const char *wifi_manager_get_ip(void)       { return s_connected ? s_ip : NULL; }
