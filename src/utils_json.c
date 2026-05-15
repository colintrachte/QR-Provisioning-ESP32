#include "utils_json.h"
#include <stdbool.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

//static const char *TAG = "utils_json";
/* ── WiFi scan helpers ──────────────────────────────────────────────────── */

int vfs_rssi_to_quality(int rssi)
{
    if (rssi <= -100) return 0;
    if (rssi >= -40)  return 100;
    return 2 * (rssi + 100);
}

cJSON *vfs_build_scan_json(const void *recs_v, int count)
{
    const wifi_ap_record_t *recs = (const wifi_ap_record_t *)recs_v;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < count; i++) {
        /* Skip hidden networks */
        if (recs[i].ssid[0] == '\0') continue;

        cJSON *ap = cJSON_CreateObject();
        if (!ap) continue;

        cJSON_AddStringToObject(ap, "ssid",    (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(ap, "quality", vfs_rssi_to_quality(recs[i].rssi));
        cJSON_AddBoolToObject  (ap, "secure",  recs[i].authmode != WIFI_AUTH_OPEN);

        cJSON_AddItemToArray(arr, ap);
    }
    return arr;
}

/**
 * Build a WiFi status JSON object.
 *
 * Shape:
 *   {
 *     "connected":     bool,   // true if reachable by any means (STA or AP)
 *     "sta_connected": bool,   // true only if STA has a router IP
 *     "ap_active":     bool,   // true if SoftAP is up with a valid IP
 *     "ssid":          string, // router SSID if STA connected, else ""
 *     "ip":            string, // STA IP if connected, else AP gateway IP
 *     "rssi":          number  // STA RSSI if connected, else 0
 *   }
 *
 * The frontend should treat connected == true as "I can talk to this device"
 * and allow arming in either mode.  sta_connected distinguishes "on router"
 * from "on AP only" for display purposes.
 */
cJSON *vfs_build_status_json(void)
{
    wifi_ap_record_t    sta_ap   = {0};
    esp_netif_ip_info_t ip_info  = {0};
    char ip_str[16]              = {0};
    char ssid_str[33]            = {0};
    bool sta_connected           = false;
    bool ap_active               = false;
    int  rssi                    = 0;

    /* ── STA check ──────────────────────────────────────────────────────── */
    esp_netif_t *sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_if &&
        esp_netif_get_ip_info(sta_if, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0 &&
        esp_wifi_sta_get_ap_info(&sta_ap) == ESP_OK)
    {
        snprintf(ip_str,   sizeof(ip_str),   IPSTR, IP2STR(&ip_info.ip));
        snprintf(ssid_str, sizeof(ssid_str), "%s",  (char *)sta_ap.ssid);
        sta_connected = true;
        rssi          = sta_ap.rssi;
    }

    /* ── AP check ───────────────────────────────────────────────────────── */
    esp_netif_t *ap_if = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_if) {
        esp_netif_ip_info_t ap_info = {0};
        if (esp_netif_get_ip_info(ap_if, &ap_info) == ESP_OK &&
            ap_info.ip.addr != 0)
        {
            ap_active = true;
            /* Use AP gateway IP as the reported address when STA is not up,
             * so the frontend knows what address it is actually talking to. */
            if (!sta_connected)
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ap_info.ip));
        }
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    /* "connected" is true whenever the device is reachable by any means.
     * The frontend uses this to decide whether to allow arming. */
    cJSON_AddBoolToObject  (obj, "connected",     sta_connected || ap_active);
    cJSON_AddBoolToObject  (obj, "sta_connected", sta_connected);
    cJSON_AddBoolToObject  (obj, "ap_active",     ap_active);
    cJSON_AddStringToObject(obj, "ssid",          ssid_str);
    cJSON_AddStringToObject(obj, "ip",            ip_str);
    cJSON_AddNumberToObject(obj, "rssi",          rssi);

    return obj;
}
