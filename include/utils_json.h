#pragma once
#include "esp_err.h" // Add this to define esp_err_t
#include "cJSON.h" // Add this to define the cJSON type
/* ── WiFi scan helpers ──────────────────────────────────────────────────── */

/**
 * Convert an RSSI value (dBm) to a 0–100 quality percentage.
 * -40 dBm → 100,  -100 dBm → 0,  linear between.
 */
int vfs_rssi_to_quality(int rssi);

/**
 * Build a cJSON array of scan results from a raw wifi_ap_record_t list.
 *
 * Each element: { "ssid": "...", "quality": 0-100, "secure": true/false }
 *
 * Duplicate SSIDs and hidden networks (ssid[0]=='\0') are skipped.
 * The caller owns the returned cJSON object and must call cJSON_Delete().
 *
 * @param recs   Pointer to array of wifi_ap_record_t from esp_wifi_scan_get_ap_records()
 * @param count  Number of records
 * @returns      cJSON array, or NULL on allocation failure
 */
cJSON *vfs_build_scan_json(const void *recs, int count);

/**
 * Build a cJSON object for the current WiFi STA status.
 *
 *   { "connected": true/false, "ssid": "...", "ip": "x.x.x.x", "rssi": -65 }
 *
 * The caller owns the returned cJSON object and must call cJSON_Delete().
 */
cJSON *vfs_build_status_json(void);
