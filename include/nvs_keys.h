/**
 * nvs_keys.h — Shared NVS namespace and key string literals.
 *
 * Both wifi_manager.c and app_server.c previously defined these independently,
 * creating a silent failure risk: if one file changed its string and the other
 * did not, they would read from different NVS slots without any compile-time
 * warning.
 *
 * Include this header in any file that reads or writes WiFi credentials or
 * device settings via NVS. Do not redefine these strings locally.
 *
 * Namespace layout
 * ────────────────
 * NVS_NS_WIFI     — WiFi credential storage (wifi_manager.c reads/writes)
 * NVS_NS_ROBOT    — Device settings storage (app_server.c / settings_mgr.c)
 *
 * Key names are intentionally short (NVS key max is 15 chars + NUL).
 */

#pragma once

/* ── Namespaces ─────────────────────────────────────────────────────────────*/
#define NVS_NS_WIFI      "wifi_mgr"    /**< WiFi credential namespace        */
#define NVS_NS_ROBOT     "robot_cfg"   /**< Device settings namespace         */

/* ── WiFi credential keys (namespace: NVS_NS_WIFI) ─────────────────────────*/
#define NVS_KEY_SSID     "ssid"        /**< Stored STA SSID                  */
#define NVS_KEY_PASS     "pass"        /**< Stored STA password               */

/* ── Settings key (namespace: NVS_NS_ROBOT) ────────────────────────────────*/
#define NVS_KEY_SETTINGS "settings"    /**< JSON blob for robot_settings_t    */
