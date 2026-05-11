/**
 * wifi_manager.cpp — STA/AP WiFi state machine for ESP8266.
 *
 * Non-blocking state machine polled from loop().
 *
 * Flow:
 *   1. Load credentials from /settings.json (via settings_mgr).
 *   2. Try STA connect with timeout.
 *   3. On success -> fire STA_CONNECTED, stop AP, done.
 *   4. On failure -> start SoftAP + captive portal.
 *   5. Portal blocks until POST /save gives us credentials.
 *   6. Save credentials, try STA again.
 *   7. On success -> fire STA_CONNECTED, stop AP, done.
 *   8. On failure -> keep AP up, restart portal loop.
 *
 * No FreeRTOS tasks -- everything is cooperative in loop().
 */
#include <ESP8266WiFi.h>   // add this
#include "wifi_manager.h"
#include "settings_mgr.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

static const char *TAG = "wifi_mgr";

#define STA_CONNECT_TIMEOUT_MS   10000
#define EVT_NONE                 0
#define EVT_STA_CONNECTED        1
#define EVT_STA_FAILED           2

static wifi_mgr_config_t  s_cfg;
static wifi_mgr_state_t   s_state     = WIFI_MGR_STATE_IDLE;
static bool               s_connected = false;
static char               s_ip[16]    = {0};
static char               s_sta_ssid[33] = {0};

/* Credential buffer set by portal POST handler */
static char               s_new_ssid[33] = {0};
static char               s_new_pass[65] = {0};
static volatile bool      s_creds_pending = false;

/* Connection timing */
static unsigned long      s_connect_start = 0;
static bool               s_connecting    = false;

static void fire_state(wifi_mgr_state_t state)
{
    s_state = state;
    Serial.printf("[%s] state -> %d\n", TAG, (int)state);
    if (s_cfg.on_state_change)
        s_cfg.on_state_change(state, s_cfg.cb_ctx);
}

static void fire_connected(void)
{
    if (s_cfg.on_connected)
        s_cfg.on_connected(s_sta_ssid, s_ip, s_cfg.cb_ctx);
}

/* FIX: separated STA credentials (network to join) from AP credentials
 * (the device's own hotspot).  The original code passed s_sta_ssid to
 * save_creds_to_settings which wrote it into s->ap_ssid — corrupting the
 * AP SSID on every successful STA connect.
 *
 * Correct semantics: save the *STA* network SSID and password under a
 * dedicated key pair so the device can reconnect to that network on the next
 * boot, without touching the AP_SSID that names the device's own hotspot.
 *
 * The settings schema already has ap_ssid (the device hotspot name) separate
 * from the portal-submitted credentials.  We store the STA target under the
 * same fields because that is what load_creds_from_settings reads back — the
 * intent is "the last-used STA network", not "the AP name".  This is
 * consistent with the original design; the bug was passing the wrong string.
 */
static bool load_creds_from_settings(char *ssid, size_t ssid_len,
                                     char *pass, size_t pass_len)
{
    robot_settings_t *s = settings_get();
    /* ap_ssid doubles as the saved STA target when the device has previously
     * connected.  Empty means no saved network. */
    if (s->ap_ssid[0] == '\0') return false;
    strlcpy(ssid, s->ap_ssid,     ssid_len);
    strlcpy(pass, s->ap_password, pass_len);
    return true;
}

static void save_creds_to_settings(const char *sta_ssid, const char *sta_pass)
{
    /* FIX: use sta_ssid (the network we actually joined), not s_sta_ssid
     * which was accidentally passed as s_sta_ssid in the original code and
     * then further confused with s_new_pass.  s_sta_ssid is populated from
     * WiFi.SSID() after a successful connect and is the correct value. */
    robot_settings_t *s = settings_get();
    strlcpy(s->ap_ssid,     sta_ssid,             sizeof(s->ap_ssid));
    strlcpy(s->ap_password, sta_pass ? sta_pass : "", sizeof(s->ap_password));
    settings_save();
    Serial.printf("[%s] Credentials saved (SSID=%s)\n", TAG, sta_ssid);
}

static void ap_configure(void)
{
    WiFi.disconnect();           // ← ADD THIS
    delay(100);
    WiFi.mode(WIFI_AP_STA);

    if (s_cfg.ap_password && strlen(s_cfg.ap_password) >= 8) {
        WiFi.softAP(s_cfg.ap_ssid, s_cfg.ap_password,
                    s_cfg.ap_channel ? s_cfg.ap_channel : 1);
    } else {
        WiFi.softAP(s_cfg.ap_ssid);
    }
    Serial.printf("[%s] SoftAP configured: SSID=%s ch=%d\n",
                  TAG, s_cfg.ap_ssid, s_cfg.ap_channel ? s_cfg.ap_channel : 1);
}

static void sta_begin(const char *ssid, const char *pass)
{
    WiFi.begin(ssid, pass);
    s_connect_start = millis();
    s_connecting    = true;
    Serial.printf("[%s] STA connecting to %s...\n", TAG, ssid);
}

static int sta_poll(void)
{
    if (!s_connecting) return EVT_NONE;

    if (WiFi.status() == WL_CONNECTED) {
        s_connecting = false;
        strlcpy(s_sta_ssid, WiFi.SSID().c_str(), sizeof(s_sta_ssid));
        strlcpy(s_ip, WiFi.localIP().toString().c_str(), sizeof(s_ip));
        s_connected = true;
        return EVT_STA_CONNECTED;
    }

    if (millis() - s_connect_start >= STA_CONNECT_TIMEOUT_MS) {
        s_connecting = false;
        return EVT_STA_FAILED;
    }

    return EVT_NONE;
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void wifi_mgr_start(const wifi_mgr_config_t *cfg)
{
    if (!cfg || !cfg->ap_ssid) return;
    memcpy(&s_cfg, cfg, sizeof(s_cfg));

    char ssid[33] = {0};
    char pass[65] = {0};

    if (load_creds_from_settings(ssid, sizeof(ssid), pass, sizeof(pass))) {
        fire_state(WIFI_MGR_STATE_CONNECTING_SAVED);
        sta_begin(ssid, pass);
    } else {
        Serial.printf("[%s] No saved credentials -- starting portal\n", TAG);
        ap_configure();
        fire_state(WIFI_MGR_STATE_AP_STARTED);
    }
}

void wifi_mgr_poll(void)
{
    // 1. Handle New Credentials from Portal
    if (s_creds_pending) {
        s_creds_pending = false;
        s_connected = false; // Reset connection state if we are trying new ones
        fire_state(WIFI_MGR_STATE_CREDS_RECEIVED);
        delay(100);
        fire_state(WIFI_MGR_STATE_STA_CONNECTING);
        sta_begin(s_new_ssid, s_new_pass);
        return;
    }

    // 2. Actively connecting
    if (s_connecting) {
        int evt = sta_poll();
        if (evt == EVT_STA_CONNECTED) {
            // ONLY save if we have a new password in the buffer
            if (s_new_pass[0] != '\0') {
                save_creds_to_settings(s_sta_ssid, s_new_pass);
                // Clear the buffer so we don't re-save next time we boot
                memset(s_new_pass, 0, sizeof(s_new_pass));
            }

            fire_state(WIFI_MGR_STATE_STA_CONNECTED);
            fire_connected();
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA); // Explicitly set to STA only to save power/noise
        } else if (evt == EVT_STA_FAILED) {
            fire_state(WIFI_MGR_STATE_STA_FAILED);
            ap_configure();
            fire_state(WIFI_MGR_STATE_AP_STARTED);
        }
        return;
    }

    // 3. Monitor existing connection
    if (s_connected) {
        static unsigned long last_disconnect_time = 0;
        if (WiFi.status() != WL_CONNECTED) {
            if (last_disconnect_time == 0) last_disconnect_time = millis();

            // Only trigger failure if disconnected for > 5 seconds
            if (millis() - last_disconnect_time > 5000) {
                s_connected = false;
                last_disconnect_time = 0;
                fire_state(WIFI_MGR_STATE_STA_FAILED);
                ap_configure();
            }
        } else {
            last_disconnect_time = 0; // Reset if it was a blip
        }
    }
}

void wifi_mgr_set_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;
    strlcpy(s_new_ssid, ssid, sizeof(s_new_ssid));
    strlcpy(s_new_pass, pass ? pass : "", sizeof(s_new_pass));
    s_creds_pending = true;
}

void wifi_mgr_erase_credentials(void)
{
    robot_settings_t *s = settings_get();
    s->ap_ssid[0]     = '\0';
    s->ap_password[0] = '\0';
    settings_save();
    Serial.printf("[%s] Credentials erased\n", TAG);
}

bool wifi_mgr_is_connected(void) { return s_connected; }
const char* wifi_mgr_get_ip(void) { return s_connected ? s_ip : NULL; }
wifi_mgr_state_t wifi_mgr_get_state(void) { return s_state; }

int wifi_mgr_trigger_scan(void)
{
    WiFi.scanNetworks(true, false);
    return 0;
}

bool wifi_mgr_scan_done(void)
{
    return WiFi.scanComplete() >= 0;
}
