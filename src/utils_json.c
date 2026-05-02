#include "utils_json.h"
#include <stdbool.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"


static const char *TAG = "utils_json";
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

        /* cJSON_AddStringToObject handles any characters safely — no manual
         * JSON escaping needed. */
        cJSON_AddStringToObject(ap, "ssid",    (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(ap, "quality", vfs_rssi_to_quality(recs[i].rssi));
        cJSON_AddBoolToObject  (ap, "secure",  recs[i].authmode != WIFI_AUTH_OPEN);

        cJSON_AddItemToArray(arr, ap);
    }
    return arr;
}

cJSON *vfs_build_status_json(void)
{
    wifi_ap_record_t     ap      = {0};
    esp_netif_ip_info_t  ip_info = {0};
    char ip_str[16]              = {0};
    char ssid_str[33]            = {0};
    bool connected               = false;
    int  rssi                    = 0;

    esp_netif_t *sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_if &&
        esp_netif_get_ip_info(sta_if, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0 &&
        esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        snprintf(ip_str,   sizeof(ip_str),   IPSTR, IP2STR(&ip_info.ip));
        snprintf(ssid_str, sizeof(ssid_str), "%s",  (char *)ap.ssid);
        connected = true;
        rssi      = ap.rssi;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddBoolToObject  (obj, "connected", connected);
    cJSON_AddStringToObject(obj, "ssid",      ssid_str);
    cJSON_AddStringToObject(obj, "ip",        ip_str);
    cJSON_AddNumberToObject(obj, "rssi",      rssi);

    return obj;
}
