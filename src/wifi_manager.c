/**
 * wifi_manager.c — STA/AP WiFi state machine and NVS credential storage.
 *
 * Manager task flow
 * ─────────────────
 *   wifi_manager_start() spawns mgr_task and returns immediately.
 *
 *   mgr_task:
 *     [NVS creds found] → fire CONNECTING_SAVED → attempt STA connect
 *                       → on success: fire STA_CONNECTED, exit prov mode.
 *                       → on failure: fall through to portal.
 *
 *     [no creds / STA failed] → configure SoftAP → fire AP_STARTED
 *                              → app_server_enter_provisioning_mode() via portal_start()
 *                              → portal_start() blocks until POST /api/connect
 *                              → credentials already saved to NVS by /api/connect handler
 *                              → portal_stop() → app_server_exit_provisioning_mode()
 *                              → fire CREDS_RECEIVED, STA_CONNECTING
 *                              → attempt STA connect
 *                              → on success: fire STA_CONNECTED.
 *                              → on failure: fire STA_FAILED, restart portal.
 *
 * Key changes from previous version
 * ──────────────────────────────────
 *   - app_server_start() is NOT called here.  app_server owns port 80 from
 *     boot (called in main.c after ctrl_drive_init).
 *   - on_sta_connected() calls app_server_exit_provisioning_mode() so that
 *     setup routes and DNS are torn down once STA is up.
 *   - Credentials are saved to NVS inside /api/connect (app_server) before
 *     the portal unblocks, so a transient RF failure never discards them.
 *   - sta_connect() distinguishes auth failure from RF/timeout failure via
 *     s_last_disconnect_reason, enabling a silent STA retry on RF issues.
 */

#include "wifi_manager.h"
#include "portal.h"
#include "app_server.h"
#include "nvs_keys.h"
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

#define EVT_STA_CONNECTED BIT0
#define EVT_STA_FAILED    BIT1

#define STA_CONNECT_TIMEOUT_MS 10000

/* Reason codes from the last WIFI_EVENT_STA_DISCONNECTED event.
 * WIFI_REASON_AUTH_FAIL (201) and WIFI_REASON_NO_AP_FOUND (201/202) indicate
 * wrong credentials; everything else is likely an RF / association issue. */
#define REASON_AUTH_FAIL      15    /* 802.11 auth reject — bad password */
#define REASON_ASSOC_FAIL     17    /* association refused */
#define REASON_HANDSHAKE_FAIL 204   /* 4-way handshake timeout — bad password */

typedef enum {
    MGR_AP_IDLE,
    MGR_AP_ACTIVE,
    MGR_AP_SHUTTING_DOWN,
} mgr_ap_state_t;

/* Return value for sta_connect() */
typedef enum {
    STA_CONN_OK,
    STA_CONN_WRONG_CREDS,   /* auth / handshake failure — re-enter portal */
    STA_CONN_RF_FAIL,       /* timeout / RF issue — creds are probably fine */
} sta_conn_result_t;

static volatile mgr_ap_state_t s_ap_state      = MGR_AP_IDLE;
static wifi_manager_config_t   s_cfg;
static EventGroupHandle_t      s_sta_evt_grp   = NULL;
static bool                    s_connected     = false;
static char                    s_ip[16]        = {0};
static char                    s_sta_ssid[33]  = {0};
static volatile int            s_retry_count   = 0;
static volatile uint8_t        s_last_disc_reason = 0;

/* ── NVS helpers ────────────────────────────────────────────────────────── */

static bool nvs_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &nvs) != ESP_OK) return false;

    bool ok = (nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK)
              && (ssid[0] != '\0');
    nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
    nvs_close(nvs);

    ESP_LOGI(TAG, "NVS credentials: %s", ok ? "found" : "not found");
    return ok;
}

/* ── State-change callbacks ─────────────────────────────────────────────── */

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

/* ── WiFi event handler ─────────────────────────────────────────────────── */

static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_connected = false;
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)data;
            s_last_disc_reason = disc->reason;
            ESP_LOGW(TAG, "STA disconnected reason=%d retry=%d/%d",
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
            if (s_ap_state == MGR_AP_SHUTTING_DOWN) break;
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

/* ── AP configuration ───────────────────────────────────────────────────── */

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

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    s_ap_state = MGR_AP_ACTIVE;
    ESP_LOGI(TAG, "SoftAP: SSID=%s ch=%d", s_cfg.ap_ssid, ap_cfg.ap.channel);
}

/* ── STA connect ────────────────────────────────────────────────────────── */

static sta_conn_result_t sta_connect(const char *ssid, const char *pass)
{
    xEventGroupClearBits(s_sta_evt_grp, EVT_STA_CONNECTED | EVT_STA_FAILED);
    s_retry_count      = 0;
    s_last_disc_reason = 0;

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
        return STA_CONN_OK;
    }

    esp_wifi_disconnect();

    /* Classify the failure so mgr_task can decide whether to show the portal */
    uint8_t reason = s_last_disc_reason;
    if (reason == REASON_AUTH_FAIL      ||
        reason == REASON_ASSOC_FAIL     ||
        reason == REASON_HANDSHAKE_FAIL) {
        ESP_LOGW(TAG, "STA connect: auth/handshake failure (reason=%d)", reason);
        return STA_CONN_WRONG_CREDS;
    }

    ESP_LOGW(TAG, "STA connect: RF/timeout failure (reason=%d) — creds kept", reason);
    return STA_CONN_RF_FAIL;
}

/* ── Radio reset for AP→APSTA transition ────────────────────────────────── */

static void reset_wifi_for_ap(void)
{
    ESP_LOGI(TAG, "Resetting WiFi for AP mode");
    s_ap_state = MGR_AP_SHUTTING_DOWN;
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    s_ap_state = MGR_AP_IDLE;

    if (s_cfg.on_radio_reset)
        s_cfg.on_radio_reset(s_cfg.cb_ctx);
}

/* ── Post-connect actions ───────────────────────────────────────────────── */

static void on_sta_connected(void)
{
    /* Tear down provisioning routes and DNS.  No-op if we never entered
     * provisioning mode (fast path with stored credentials). */
    app_server_exit_provisioning_mode();

    fire_state(WIFI_MANAGER_STATE_STA_CONNECTED);
    fire_connected();

    /* Keep AP up intentionally — a client already on the SoftAP should
     * not lose the control page just because STA connected.  The AP will
     * be idle (no DNS hijack, no setup routes) but harmless.  Power cost
     * is negligible on ESP32-S3 in APSTA mode with no AP clients. */
    ESP_LOGI(TAG, "STA connected — remaining in APSTA mode for local control");

#if MDNS_ENABLE
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set("MuleBot Robot");
        ESP_LOGI(TAG, "mDNS: %s.local", MDNS_HOSTNAME);
    }
#endif
}

/* ── Manager task ───────────────────────────────────────────────────────── */

static void mgr_task(void *arg)
{
    (void)arg;
    char ssid[33] = {0};
    char pass[65] = {0};

    /* ── Fast path: stored credentials ─────────────────────────────────── */
    if (nvs_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        fire_state(WIFI_MANAGER_STATE_CONNECTING_SAVED);
        sta_conn_result_t result = sta_connect(ssid, pass);

        if (result == STA_CONN_OK) {
            on_sta_connected();
            vTaskDelete(NULL);
            return;
        }

        if (result == STA_CONN_RF_FAIL) {
            /* Credentials are likely fine — silent retry before portal.
             * RF conditions may improve within a few seconds. */
            ESP_LOGW(TAG, "RF failure on saved creds — retrying in 3 s");
            vTaskDelay(pdMS_TO_TICKS(3000));
            result = sta_connect(ssid, pass);
            if (result == STA_CONN_OK) {
                on_sta_connected();
                vTaskDelete(NULL);
                return;
            }
        }

        ESP_LOGW(TAG, "Saved credentials failed (result=%d) — falling back to portal",
                 (int)result);
    }

    /* ── Portal path ────────────────────────────────────────────────────── */
    reset_wifi_for_ap();
    ap_configure();
    fire_state(WIFI_MANAGER_STATE_AP_STARTED);

    bool have_creds = false;
    uint32_t rf_retry_delay_ms = 5000;   /* starts at 5 s, caps at 30 s */

    while (1) {
        /* ── Credential acquisition ───────────────────────────────────── */
        if (!have_creds) {
            /* portal_start() calls app_server_enter_provisioning_mode()
             * and blocks until /api/connect saves creds to NVS and fires
             * the callback.  Credentials are in NVS before we unblock. */
            portal_start(s_cfg.ap_ssid);

            s_ap_state = MGR_AP_SHUTTING_DOWN;

            /* portal_stop() calls app_server_exit_provisioning_mode():
             * DNS stops, setup routes unregister, / is now the control page.
             * The AP radio stays up — clients keep their connection. */
            portal_stop();

            portal_get_credentials(ssid, pass);
            fire_state(WIFI_MANAGER_STATE_CREDS_RECEIVED);
            have_creds = true;
            rf_retry_delay_ms = 5000;   /* reset backoff for fresh creds */
        }

        /* ── STA connection attempt ───────────────────────────────────── */
        vTaskDelay(pdMS_TO_TICKS(300));
        fire_state(WIFI_MANAGER_STATE_STA_CONNECTING);

        sta_conn_result_t result = sta_connect(ssid, pass);

        if (result == STA_CONN_OK) {
            on_sta_connected();   /* keeps AP alive, exits prov mode */
            vTaskDelete(NULL);
            return;
        }

        if (result == STA_CONN_RF_FAIL) {
            /* Creds are already in NVS.  Keep AP up so the user can reach
             * the control page at /index.html and drive the robot while
             * we retry in the background with exponential backoff. */
            fire_state(WIFI_MANAGER_STATE_STA_RETRYING);
            ESP_LOGW(TAG, "RF fail — AP up, control page live at /index.html, "
                          "retrying STA in %lu ms", (unsigned long)rf_retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(rf_retry_delay_ms));

            /* Exponential backoff: 5 s → 10 s → 20 s → 30 s (cap) */
            rf_retry_delay_ms = rf_retry_delay_ms * 2;
            if (rf_retry_delay_ms > 30000) rf_retry_delay_ms = 30000;

            continue;   /* retry STA — do NOT re-enter portal */
        }

        /* ── Auth failure: credentials are wrong, ask user again ─────── */
        fire_state(WIFI_MANAGER_STATE_STA_FAILED);
        ESP_LOGW(TAG, "Auth failure — restarting portal for new credentials");
        have_creds = false;   /* re-enter portal on next iteration */
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

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

    /* esp_netif_init(), esp_event_loop_create_default(), and
     * esp_netif_create_default_wifi_{ap,sta}() are called in main.c before
     * app_server_start() so the TCP/IP stack is up before httpd_start().
     * Guard against double-init in case wifi_manager_start() is ever called
     * standalone in a test harness. */
    {
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
    esp_err_t err = nvs_open(NVS_NS_WIFI, NVS_READWRITE, &nvs);
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
