/**
 * main_d1mini.cpp — RC Tank firmware for Wemos D1 Mini (ESP8266).
 *
 * Refactored to share the full feature set with the ESP32-S3 target:
 *   - Runtime settings via /api/settings (LittleFS + ArduinoJson)
 *   - UDP control socket on port 4210 (same packet format as S3)
 *   - Health telemetry with battery, RSSI, heap (double-buffered JSON)
 *   - Non-blocking WiFi manager with portal fallback
 *   - All existing: WebSocket, OTA, motor ramp, LED patterns, drive_mixer
 *
 * Build:    pio run -e d1_mini -t upload
 * Web UI:   pio run -e d1_mini -t uploadfs
 * Monitor:  pio device monitor -e d1_mini
 *
 * Pinout (Wemos D1 Mini) — OLED-safe (motors moved off D1/D2)
 * ────────────────────────────────────────────────────────────────────────────
 *   Left  forward   D7  GPIO 13
 *   Left  reverse   D8  GPIO 15  (must be LOW at boot — check boot mode)
 *   Right forward   D5  GPIO 14
 *   Right reverse   D6  GPIO 12
 *   Reset / reprov  D3  GPIO 0   FLASH button -- hold 3 s on boot
 *   Onboard LED         GPIO 2   Active LOW
 *
 * Optional OLED (SSD1306 128x64 I2C):
 *   SDA = D2 (GPIO 4)
 *   SCL = D1 (GPIO 5)
 *   Motor pins above are clear of D1/D2 so OLED can coexist.
 *   Uncomment #define HAS_OLED below to enable.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <Updater.h>

/* ── Shared drive logic ────────────────────────────────────────────────────*/
#define DRIVE_LEFT_SIGN  -1
#define DRIVE_RIGHT_SIGN  1
#include "drive_mixer.h"

/* ── Settings & managers ───────────────────────────────────────────────────*/
#include "settings_mgr.h"
#include "settings_server.h"
#include "udp_ctrl.h"
#include "health_monitor.h"
#include "i_battery.h"
#include "wifi_manager.h"

/* ── Optional OLED ─────────────────────────────────────────────────────────
 * Uncomment to enable SSD1306 support. Requires u8g2 library.
 * Motor pins (D5-D8) are already clear of the hardware I2C pins (D1/D2).
 */
// #define HAS_OLED
#ifdef HAS_OLED
  #include <U8g2lib.h>
  #include <Wire.h>
  #define OLED_SDA  D2   /* GPIO 4 — hardware SDA */
  #define OLED_SCL  D1   /* GPIO 5 — hardware SCL */
  static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_u8g2(U8G2_R0, U8X8_PIN_NONE);
  static bool s_oled_ok = false;
#endif

/* ── Identity ──────────────────────────────────────────────────────────────*/
static constexpr char HOSTNAME[] = "rc-tank";
static AsyncWebHandler *s_static_handler = nullptr;
/* ── Pinout ─────────────────────────────────────────────────────────────────*/
static constexpr uint8_t LEFT_FWD  = D7;   /* GPIO 13  */
static constexpr uint8_t LEFT_REV  = D8;   /* GPIO 15  — must be LOW at boot */
static constexpr uint8_t RIGHT_FWD = D5;   /* GPIO 14  */
static constexpr uint8_t RIGHT_REV = D6;   /* GPIO 12  */
static constexpr uint8_t RESET_BTN = D3;   /* GPIO 0   — FLASH button, active LOW (has onboard pull-up) */
static constexpr uint8_t LED_PIN   = 2;    /* GPIO 2   — active LOW, onboard LED */

/* ── Tuning ─────────────────────────────────────────────────────────────────*/
static constexpr int          MAX_PWM       = 1023;
static constexpr int          DIR_LOCK_PWM  = 80;

static constexpr unsigned long MOTOR_TICK_MS = 20;
static constexpr unsigned long CLEANUP_MS    = 5000;
static constexpr unsigned long MDNS_TICK_MS  = 100;
static constexpr unsigned long RESET_HOLD_MS = 3000;

/* ═════════════════════════════════════════════════════════════════════════════
 *  LED -- pattern engine
 * ═════════════════════════════════════════════════════════════════════════════*/

enum class LedPattern : uint8_t {
    OFF,
    ON,
    SLOW_BLINK,    /*  1 Hz  -- booting / disarmed          */
    FAST_BLINK,    /*  5 Hz  -- connecting                   */
    DOUBLE_BLINK,  /* two pulses then dark -- error / e-stop */
    HEARTBEAT,     /* long on + dim echo -- running / armed  */
};

#define LED_FULL  1023
#define LED_DIM   341
#define LED_INVERT(duty)  (LED_FULL - (duty))

static volatile LedPattern s_led_pattern = LedPattern::OFF;
static volatile int        s_led_step    = 0;
static volatile int        s_led_duty    = 0;
static volatile bool       s_led_dirty   = false;

static Ticker s_led_ticker;

static void IRAM_ATTR ledTick()
{
    LedPattern pat  = s_led_pattern;
    int        step = s_led_step;
    int        duty = 0;

    switch (pat) {
    case LedPattern::OFF:
        duty = 0; break;
    case LedPattern::ON:
        duty = LED_FULL; break;
    case LedPattern::SLOW_BLINK:
        duty = (step < 10) ? LED_FULL : 0;
        s_led_step = (step + 1) % 20;
        break;
    case LedPattern::FAST_BLINK:
        duty = (step < 2) ? LED_FULL : 0;
        s_led_step = (step + 1) % 4;
        break;
    case LedPattern::DOUBLE_BLINK:
        if      (step < 2)  duty = LED_FULL;
        else if (step < 4)  duty = 0;
        else if (step < 6)  duty = LED_FULL;
        else                duty = 0;
        s_led_step = (step + 1) % 20;
        break;
    case LedPattern::HEARTBEAT:
        if      (step < 14) duty = LED_FULL;
        else if (step < 16) duty = 0;
        else if (step < 18) duty = LED_DIM;
        else                duty = 0;
        s_led_step = (step + 1) % 20;
        break;
    }

    s_led_duty  = duty;
    s_led_dirty = true;
}

static void ledApply()
{
    if (!s_led_dirty) return;
    s_led_dirty = false;
    analogWrite(LED_PIN, LED_INVERT(s_led_duty));
}

void o_led_set(float brightness)
{
    brightness = constrain(brightness, 0.0f, 1.0f);
    s_led_step    = 0;
    s_led_pattern = LedPattern::OFF;
    analogWrite(LED_PIN, LED_INVERT((int)(brightness * LED_FULL)));
}

void o_led_blink(int pattern)
{
    s_led_step = 0;
    switch (pattern) {
        case 0: s_led_pattern = LedPattern::OFF;          break;
        case 1: s_led_pattern = LedPattern::ON;           break;
        case 2: s_led_pattern = LedPattern::SLOW_BLINK;   break;
        case 3: s_led_pattern = LedPattern::FAST_BLINK;   break;
        case 4: s_led_pattern = LedPattern::DOUBLE_BLINK; break;
        case 5: s_led_pattern = LedPattern::HEARTBEAT;    break;
        default: s_led_pattern = LedPattern::OFF;         break;
    }
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  MOTOR
 * ═════════════════════════════════════════════════════════════════════════════*/

enum class DriveState : uint8_t { STOPPED, RAMP_UP, RUNNING };

struct Motor {
    const uint8_t pinFwd;
    const uint8_t pinRev;
    int        pwm     = 0;
    int        target  = 0;
    bool       forward = true;
    DriveState state   = DriveState::STOPPED;
};

static Motor s_left  = { LEFT_FWD,  LEFT_REV  };
static Motor s_right = { RIGHT_FWD, RIGHT_REV };

static void motorWrite(const Motor &m)
{
    if (m.state == DriveState::STOPPED) {
        analogWrite(m.pinFwd, 0);
        analogWrite(m.pinRev, 0);
    } else if (m.forward) {
        analogWrite(m.pinFwd, m.pwm);
        analogWrite(m.pinRev, 0);
    } else {
        analogWrite(m.pinFwd, 0);
        analogWrite(m.pinRev, m.pwm);
    }
}

/* FIX: snapshot deadband/ramp once per tick rather than re-reading settings
 * on every motor call. Prevents a mid-ramp settings change from flickering
 * the state machine between STOPPED and RAMP_UP. */
static void motorTick(Motor &m, int deadband, int ramp)
{
    if (m.target < deadband) {
        m.pwm   = 0;
        m.state = DriveState::STOPPED;
    } else if (m.state == DriveState::STOPPED) {
        m.state = DriveState::RAMP_UP;
        m.pwm   = 0;
    } else if (m.state == DriveState::RAMP_UP) {
        m.pwm = min(m.pwm + ramp, m.target);
        if (m.pwm >= m.target) m.state = DriveState::RUNNING;
    } else {
        m.pwm = m.target;
    }
    motorWrite(m);
}

/* Direction reversal blocked above DIR_LOCK_PWM to protect H-bridge. */
static void motorSetDuty(Motor &m, float duty, int deadband)
{
    const bool want_fwd = (duty >= 0.0f);
    const int  mag      = min((int)(fabsf(duty) * MAX_PWM), MAX_PWM);

    if (want_fwd != m.forward && m.pwm > DIR_LOCK_PWM) return;

    m.forward = want_fwd;
    m.target  = mag;

    if (mag < deadband)
        m.state = DriveState::STOPPED;
    else if (m.state == DriveState::STOPPED)
        m.state = DriveState::RAMP_UP;
}

static void hardStop()
{
    s_left.target  = s_right.target = 0;
    s_left.pwm     = s_right.pwm    = 0;
    s_left.state   = s_right.state  = DriveState::STOPPED;
    motorWrite(s_left);
    motorWrite(s_right);
}

static void applyAxes(float x, float y, int deadband)
{
    drive_out_t out = drive_mix(x, y);
    motorSetDuty(s_left,  out.left,  deadband);
    motorSetDuty(s_right, out.right, deadband);
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  CTRL_DRIVE API -- exposed for udp_ctrl.cpp and WS handlers
 * ═════════════════════════════════════════════════════════════════════════════*/

static bool          s_armed          = false;
static bool          s_watchdog_fired = false;
static unsigned long s_last_command   = 0;

void ctrl_drive_feed_watchdog(void) { s_last_command = millis(); s_watchdog_fired = false; }
void ctrl_drive_set_axes(float x, float y)
{
    if (!s_armed) return;
    int deadband = (int)(settings_get()->drive_deadband * MAX_PWM);
    applyAxes(x, y, deadband);
}
void ctrl_drive_emergency_stop(void) { hardStop(); }
void ctrl_drive_set_armed(bool armed) { s_armed = armed; if (!armed) hardStop(); }
bool ctrl_drive_is_armed(void) { return s_armed; }

/* ═════════════════════════════════════════════════════════════════════════════
 *  GLOBAL STATE
 * ═════════════════════════════════════════════════════════════════════════════*/

static bool          s_provisioning   = false;
static bool          s_app_ready      = false;
static unsigned long s_reboot_at      = 0;

static unsigned long s_last_motor   = 0;
static unsigned long s_last_telem   = 0;
static unsigned long s_last_cleanup = 0;
static unsigned long s_last_mdns    = 0;

static AsyncWebServer s_http(80);
static AsyncWebSocket s_ws("/ws");
static DNSServer      s_dns;

/* ═════════════════════════════════════════════════════════════════════════════
 *  WEBSOCKET DISPATCH
 * ═════════════════════════════════════════════════════════════════════════════*/

static void broadcastTelemetry()
{
    if (s_ws.count() == 0) return;
    const char *json = health_monitor_get_telemetry_json();
    s_ws.textAll(json);
}

static void handleMessage(AsyncWebSocketClient *client, const char *msg)
{
    drive_cmd_t cmd;
    if (!drive_parse(msg, &cmd)) {
        Serial.printf("WS unknown: %s\n", msg);
        return;
    }

    s_last_command   = millis();
    s_watchdog_fired = false;

    int deadband = (int)(settings_get()->drive_deadband * MAX_PWM);

    switch (cmd.type) {
        case DRIVE_CMD_PING:
            client->text("pong");
            break;
        case DRIVE_CMD_ARM:
            s_armed = true;
            o_led_blink(5);   /* HEARTBEAT */
            Serial.println("ARMED");
            break;
        case DRIVE_CMD_DISARM:
            s_armed = false;
            hardStop();
            o_led_blink(2);   /* SLOW_BLINK */
            Serial.println("DISARMED");
            break;
        case DRIVE_CMD_STOP:
            hardStop();
            break;
        case DRIVE_CMD_AXES:
            if (s_armed) applyAxes(cmd.x, cmd.y, deadband);
            break;
        case DRIVE_CMD_LED:
            o_led_set(cmd.led);
            break;
        default:
            break;
    }
}

static void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("WS client #%u connected\n", client->id());
            s_last_command   = millis();
            s_watchdog_fired = false;
            o_led_blink(2);
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("WS client #%u disconnected\n", client->id());
            s_armed = false;
            hardStop();
            if (s_ws.count() == 0)
                o_led_blink(2);
            break;
        case WS_EVT_DATA: {
            const auto *info = reinterpret_cast<AwsFrameInfo *>(arg);
            if (info->opcode != WS_TEXT || len == 0) break;

            /* FIX: ESPAsyncWebServer allocates exactly `len` bytes — writing
             * data[len]='\0' is a one-byte out-of-bounds write that corrupts
             * the heap.  Use a stack copy with room for the terminator. */
            char buf[256];
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, data, len);
            buf[len] = '\0';
            handleMessage(client, buf);
            break;
        }
        default:
            break;
    }
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  WIFI MANAGER CALLBACKS
 * ═════════════════════════════════════════════════════════════════════════════*/

static void onWifiStateChange(wifi_mgr_state_t state, void *ctx)
{
    (void)ctx;
    switch (state) {
        case WIFI_MGR_STATE_AP_STARTED:
            o_led_blink(3);   /* FAST_BLINK -- setup mode */
            break;
        case WIFI_MGR_STATE_STA_CONNECTED:
            o_led_blink(2);   /* SLOW_BLINK -- connected, waiting for client */
            break;
        case WIFI_MGR_STATE_STA_FAILED:
            o_led_blink(4);   /* DOUBLE_BLINK -- error */
            break;
        default:
            break;
    }
}
static void setupOTAEndpoints(void);
static void onWifiConnected(const char *ssid, const char *ip, void *ctx)
{
    (void)ctx;
    Serial.printf("WiFi connected: %s @ %s\n", ssid, ip);

    robot_settings_t *s = settings_get();
    if (s->mdns_enable) {
        if (MDNS.begin(s->mdns_hostname[0] ? s->mdns_hostname : HOSTNAME)) {
            MDNS.addService("http", "tcp", 80);
            MDNS.addService("ws",   "tcp", 80);
            Serial.printf("mDNS: http://%s.local\n", s->mdns_hostname);
        }
    }

    s_ws.onEvent(onWsEvent);
    s_http.addHandler(&s_ws);

    if (s_static_handler) {
        s_http.removeHandler(s_static_handler);
        s_static_handler = &s_http.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    }

    settings_server_register(&s_http);
    setupOTAEndpoints();

    s_http.begin();

    udp_ctrl_begin();

    s_app_ready    = true;
    s_last_command = millis();
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  OTA ENDPOINTS
 * ═════════════════════════════════════════════════════════════════════════════*/

/* FIX: OTA token validation moved to chunk-index 0 only.
 * Previously the token check ran on every chunk and called req->send() mid-
 * upload — that hangs the async handler and leaves the Update object in a
 * partially-written state.  Rejecting at chunk 0 is clean and sufficient. */
static bool ota_check_token(AsyncWebServerRequest *req)
{
    robot_settings_t *s = settings_get();
    if (s->ota_token[0] == '\0') return true;   /* no token configured */

    String tok = req->header("X-OTA-Token");
    size_t cfg_len = strlen(s->ota_token);
    size_t tok_len = tok.length();
    if (cfg_len != tok_len) return false;

    uint8_t diff = 0;
    for (size_t i = 0; i < cfg_len; i++)
        diff |= (uint8_t)s->ota_token[i] ^ (uint8_t)tok[i];
    return diff == 0;
}

static void setupOTAEndpoints(void)
{
    s_http.on("/ota/firmware", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            bool ok = !Update.hasError();
            AsyncWebServerResponse *resp = req->beginResponse(
                ok ? 200 : 500, "text/plain",
                ok ? "OK -- rebooting" : "Update failed");
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) {
                req->onDisconnect([]() { delay(500); ESP.restart(); });
            }
        },
        nullptr,  // ← ArUploadHandlerFunction (not used)
        [](AsyncWebServerRequest *req, uint8_t *data,
        size_t len, size_t index, size_t total) {
            if (index == 0) {
                if (!ota_check_token(req)) {
                    req->send(401, "text/plain", "Bad or missing X-OTA-Token");
                    return;
                }
                Serial.printf("Firmware OTA start, size=%u\n", (unsigned)total);
                if (!Update.begin(total, U_FLASH))
                    Update.printError(Serial);
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len)
                    Update.printError(Serial);
            }
            if (index + len == total) {
                if (!Update.end(true))
                    Update.printError(Serial);
                else
                    Serial.printf("Firmware OTA complete (%u bytes)\n", (unsigned)total);
            }
        }
    );

    s_http.on("/ota/filesystem", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            bool ok = !Update.hasError();
            req->send(ok ? 200 : 500, "text/plain",
                      ok ? "OK -- filesystem updated" : "Filesystem update failed");
        },
        nullptr,
        [](AsyncWebServerRequest *req, uint8_t *data,
           size_t len, size_t index, size_t total) {
            if (index == 0) {
                if (!ota_check_token(req)) {
                    req->send(401, "text/plain", "Bad or missing X-OTA-Token");
                    return;
                }
                Serial.printf("Filesystem OTA start, size=%u\n", (unsigned)total);
                LittleFS.end();
                if (!Update.begin(total, U_FS))
                    Update.printError(Serial);
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len)
                    Update.printError(Serial);
            }
            if (index + len == total) {
                if (!Update.end(true))
                    Update.printError(Serial);
                else {
                    LittleFS.begin();
                    Serial.printf("Filesystem OTA complete (%u bytes)\n", (unsigned)total);
                }
            }
        });
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  URL DECODE HELPER
 * ═════════════════════════════════════════════════════════════════════════════*/

/* FIX: Full percent-decode + plus-as-space (application/x-www-form-urlencoded).
 * Original code only handled 4 sequences — SSIDs/passwords with any other
 * special character would silently pass garbage to wifi_mgr_set_credentials.
 */
static String urlDecode(const String &in)
{
    String out;
    out.reserve(in.length());
    for (int i = 0; i < (int)in.length(); i++) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < (int)in.length()) {
            char h = in[i+1], l = in[i+2];
            auto hexval = [](char x) -> int {
                if (x >= '0' && x <= '9') return x - '0';
                if (x >= 'A' && x <= 'F') return x - 'A' + 10;
                if (x >= 'a' && x <= 'f') return x - 'a' + 10;
                return -1;
            };
            int hi = hexval(h), lo = hexval(l);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  PROVISIONING MODE (captive portal)
 * ═════════════════════════════════════════════════════════════════════════════*/

static void startProvisioningMode()
{
    /* FIX: wifi_mgr_start() already called ap_configure() / WiFi.softAP()
     * before this function is reached.  Calling it again here was causing a
     * redundant AP restart that dropped clients who joined during the scan
     * window.  We only need to start DNS and register HTTP routes. */
    s_provisioning = true;

    robot_settings_t *s = settings_get();
    Serial.printf("AP: %s  IP: %s\n", s->ap_ssid,
                  WiFi.softAPIP().toString().c_str());
    o_led_blink(3);   /* FAST_BLINK */

    WiFi.scanNetworksAsync([](int n) {
        Serial.printf("Initial scan: %d networks\n", n);
    }, false);

    s_dns.start(53, "*", WiFi.softAPIP());

    /* OS captive-portal probes */
    s_http.on("/ncsi.txt", HTTP_GET,
        [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "Microsoft Connect Test"); });
    s_http.on("/connecttest.txt", HTTP_GET,
        [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "Microsoft Connect Test"); });
    s_http.on("/redirect", HTTP_GET,
        [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "Microsoft Connect Test"); });
    s_http.on("/hotspot-detect.html", HTTP_GET,
        [](AsyncWebServerRequest *r){ r->send(200, "text/html", "<HTML><BODY>Success</BODY></HTML>"); });
    s_http.on("/library/test/success.html", HTTP_GET,
        [](AsyncWebServerRequest *r){ r->send(200, "text/html", "<HTML><BODY>Success</BODY></HTML>"); });
    s_http.on("/generate_204", HTTP_GET,
        [](AsyncWebServerRequest *r){ r->send(204); });
    s_http.on("/gen_204", HTTP_GET,
        [](AsyncWebServerRequest *r){ r->send(204); });
    s_http.on("/success.txt", HTTP_GET,
        [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "success"); });

    /* Scan endpoint */
    s_http.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            req->send(202, "application/json", "{\"status\":\"scanning\"}");
            return;
        }
        if (n <= 0) {
            WiFi.scanNetworksAsync([](int n){ Serial.printf("Rescan: %d\n", n); }, false);
            req->send(202, "application/json", "{\"status\":\"scanning\"}");
            return;
        }
        String json = "[";
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            String ssid = WiFi.SSID(i);
            ssid.replace("\\", "\\\\");
            ssid.replace("\"", "\\\"");
            int q = constrain(2 * (WiFi.RSSI(i) + 100), 0, 100);
            json += "{\"ssid\":\"" + ssid + "\""
                  + ",\"quality\":"  + String(q)
                  + ",\"secure\":"   + (WiFi.encryptionType(i) != ENC_TYPE_NONE ? "true" : "false")
                  + "}";
        }
        json += "]";
        WiFi.scanDelete();
        WiFi.scanNetworksAsync([](int){}, false);
        req->send(200, "application/json", json);
    });

    /* Save credentials endpoint */
    s_http.on("/api/connect", HTTP_POST,
        [](AsyncWebServerRequest *req) {},
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            static char s_body[320];
            static size_t s_body_len = 0;
            if (index + len < sizeof(s_body)) {
                memcpy(s_body + index, data, len);
                s_body_len = index + len;
                s_body[s_body_len] = '\0';
            }
            if (index + len == total) {
                String body(s_body);
                int si = body.indexOf("ssid=");
                int pi = body.indexOf("pass=");
                String ssid, pass;
                if (si >= 0) {
                    si += 5;
                    int se = body.indexOf('&', si);
                    ssid = (se >= 0) ? body.substring(si, se) : body.substring(si);
                }
                if (pi >= 0) {
                    pi += 5;
                    int pe = body.indexOf('&', pi);
                    pass = (pe >= 0) ? body.substring(pi, pe) : body.substring(pi);
                }

                /* FIX: full URL decode (handles all % escapes + plus-as-space) */
                ssid = urlDecode(ssid);
                pass = urlDecode(pass);
                ssid.trim();
                pass.trim();

                if (ssid.length() == 0) {
                    req->send(400, "text/plain", "SSID required");
                    return;
                }

                wifi_mgr_set_credentials(ssid.c_str(), pass.c_str());
                req->send(200, "text/plain", "Saved. Connecting...");
            }
        });

    s_http.onNotFound([](AsyncWebServerRequest *req) {
        req->redirect("http://" + WiFi.softAPIP().toString() + "/setup.html");
    });

    s_static_handler = s_http.serveStatic("/", LittleFS, "/").setDefaultFile("setup.html");
    s_http.begin();
    Serial.println("Provisioning server ready");
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  RESET BUTTON
 * ═════════════════════════════════════════════════════════════════════════════*/

static void checkResetButton()
{
    /* FIX: GPIO0 (D3) has a 10k pull-up on the D1 Mini board, so INPUT is
     * functionally safe, but INPUT_PULLUP enables the internal pull-up too
     * for robustness on board variants without the external resistor. */
    if (digitalRead(RESET_BTN) == HIGH) return;

    Serial.print("Reset held -- hold for 3 s to clear credentials...");
    const unsigned long start = millis();
    while (digitalRead(RESET_BTN) == LOW) {
        unsigned long held = millis() - start;
        if (held >= RESET_HOLD_MS) {
            Serial.println(" done.");
            wifi_mgr_erase_credentials();
            o_led_set(1.0f);
            return;
        }
        digitalWrite(LED_PIN, (held / 100) % 2 == 0 ? LOW : HIGH);
        delay(50);
    }
    digitalWrite(LED_PIN, HIGH);
    Serial.println(" released early -- cancelled");
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  OPTIONAL OLED
 * ═════════════════════════════════════════════════════════════════════════════*/

#ifdef HAS_OLED
static void oled_init(void)
{
    Wire.begin(OLED_SDA, OLED_SCL);
    s_u8g2.setBusClock(400000);
    if (s_u8g2.begin()) {
        s_oled_ok = true;
        s_u8g2.clearBuffer();
        s_u8g2.setFont(u8g2_font_6x10_tf);
        s_u8g2.drawStr(0, 10, "RC-Tank booting...");
        s_u8g2.sendBuffer();
        Serial.println("OLED ready");
    } else {
        Serial.println("OLED not detected");
    }
}

static void oled_show_status(const char *line1, const char *line2, const char *line3)
{
    if (!s_oled_ok) return;
    s_u8g2.clearBuffer();
    s_u8g2.setFont(u8g2_font_6x10_tf);
    if (line1) s_u8g2.drawStr(0, 10, line1);
    if (line2) s_u8g2.drawStr(0, 22, line2);
    if (line3) s_u8g2.drawStr(0, 34, line3);
    s_u8g2.sendBuffer();
}
#else
static void oled_init(void) {}
static void oled_show_status(const char *, const char *, const char *) {}
#endif

/* ═════════════════════════════════════════════════════════════════════════════
 *  ARDUINO ENTRY POINTS
 * ═════════════════════════════════════════════════════════════════════════════*/

void setup()
{
    Serial.begin(74880);
    Serial.println("\n\nRC Tank (D1 Mini) booting...");

    /* Motor pins */
    for (uint8_t pin : { LEFT_FWD, LEFT_REV, RIGHT_FWD, RIGHT_REV })
        pinMode(pin, OUTPUT);
    hardStop();

    /* LED */
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);   /* off -- active LOW */

    /* FIX: INPUT_PULLUP for robustness on all D1 Mini board variants */
    pinMode(RESET_BTN, INPUT_PULLUP);

    /* LED ticker */
    s_led_ticker.attach_ms(50, ledTick);
    o_led_blink(2);   /* SLOW_BLINK -- booting */

    /* Filesystem */
    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed -- halting");
        o_led_blink(4);   /* DOUBLE_BLINK -- error */
        return;
    }

    /* Settings */
    settings_load();

    /* Battery ADC */
    i_battery_init();

    /* Optional OLED */
    oled_init();
    oled_show_status("RC-Tank", "Starting...", nullptr);

    /* Reset button check */
    checkResetButton();

    /* WiFi manager */
    robot_settings_t *st = settings_get();
    wifi_mgr_config_t cfg = {
        .ap_ssid         = st->ap_ssid,
        .ap_password     = st->ap_password,
        .ap_channel      = st->ap_channel,
        .sta_max_retries = 3,
        .on_state_change = onWifiStateChange,
        .on_connected    = onWifiConnected,
        .cb_ctx          = nullptr,
    };
    wifi_mgr_start(&cfg);

    /* If wifi_mgr_start() fell through to portal immediately, set it up */
    if (wifi_mgr_get_state() == WIFI_MGR_STATE_AP_STARTED) {
        startProvisioningMode();
    }

    /* Health monitor baseline */
    health_monitor_init();

    Serial.println("Setup complete -- entering loop");
}

void loop()
{
    ledApply();
    wifi_mgr_poll();

    if (s_provisioning) {
        s_dns.processNextRequest();
        if (s_reboot_at && millis() >= s_reboot_at) ESP.restart();
        return;
    }

    if (!s_app_ready) return;

    const unsigned long now = millis();

    /* FIX: snapshot settings once per loop iteration — avoids repeated
     * settings_get() calls across multiple conditionals, and prevents a
     * mid-loop settings update from producing inconsistent interval values
     * (e.g. watchdog evaluated with old value, motor tick with new). */
    const uint32_t watchdog_ms = settings_get()->drive_watchdog_ms;
    const uint32_t telem_ms    = settings_get()->telemetry_interval_ms;
    const int      deadband    = (int)(settings_get()->drive_deadband * MAX_PWM);
    const int      ramp        = max(1, (int)(settings_get()->drive_ramp_rate * MAX_PWM));

    /* mDNS */
    if (now - s_last_mdns >= MDNS_TICK_MS) {
        s_last_mdns = now;
        MDNS.update();
    }

    /* UDP control */
    udp_ctrl_poll();

    /* Command watchdog */
    if (!s_watchdog_fired && (now - s_last_command > watchdog_ms)) {
        s_armed          = false;
        s_watchdog_fired = true;
        hardStop();
        o_led_blink(4);   /* DOUBLE_BLINK -- error */
        Serial.printf("Watchdog: no command (%u ms) -- disarmed and stopped\n", watchdog_ms);
    }

    /* Motor ramp tick — pass snapshotted values */
    if (now - s_last_motor >= MOTOR_TICK_MS) {
        s_last_motor = now;
        motorTick(s_left,  deadband, ramp);
        motorTick(s_right, deadband, ramp);
    }

    /* Telemetry */
    if (now - s_last_telem >= telem_ms) {
        s_last_telem = now;
        health_monitor_tick();
        broadcastTelemetry();
    }

    /* WS cleanup */
    if (now - s_last_cleanup >= CLEANUP_MS) {
        s_last_cleanup = now;
        s_ws.cleanupClients();
    }
}
