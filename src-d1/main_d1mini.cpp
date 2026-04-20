/**
 * src-d1/main.cpp — RC Tank firmware for Wemos D1 Mini (ESP8266).
 *
 * Build:    pio run -e d1_mini -t upload
 * Web UI:   pio run -e d1_mini -t uploadfs
 * Monitor:  pio device monitor -e d1_mini
 *
 * Shares data/ (index.html / style.css / script.js) with the S3 target.
 * Shared drive logic lives in shared/drive_mixer.h.
 *
 * Pinout (Wemos D1 Mini)
 * ─────────────────────────────────────────────────────────────────────────────
 *   Left  forward   D1  GPIO 5
 *   Left  reverse   D2  GPIO 4
 *   Right forward   D5  GPIO 14
 *   Right reverse   D6  GPIO 12
 *   Reset / reprov  D3  GPIO 0   FLASH button — hold 3 s on boot
 *   Onboard LED         GPIO 2   Active LOW. Shared with UART TX — expect
 *                                flicker on serial output. This is normal.
 *
 * Motor wiring note
 * ─────────────────────────────────────────────────────────────────────────────
 * The left motor on this board is physically wired so that "forward" on the
 * H-bridge drives the track backward relative to the chassis. DRIVE_LEFT_SIGN
 * compensates without touching the mixer math. If your wiring differs, flip
 * DRIVE_LEFT_SIGN or DRIVE_RIGHT_SIGN in the defines below.
 *
 * LED quirks vs S3 target
 * ─────────────────────────────────────────────────────────────────────────────
 * S3:  LEDC PWM, active HIGH, FreeRTOS software timer drives pattern tick.
 * D1:  analogWrite() PWM (10-bit), active LOW (duty must be inverted),
 *      Ticker drives pattern tick from an os_timer ISR.
 *
 *      CRITICAL: analogWrite() is not ISR-safe on ESP8266 — it calls into
 *      the PWM driver with interrupts enabled and will crash if called from
 *      the Ticker callback. The pattern tick therefore only sets a flag;
 *      ledApply() is called from loop() to do the actual analogWrite().
 *      This adds at most one loop() latency (~few ms) which is imperceptible
 *      for blink patterns.
 *
 * Differences from S3 target
 * ─────────────────────────────────────────────────────────────────────────────
 *   No OLED, no QR codes, no LEDC peripheral.
 *   Credentials stored in LittleFS JSON instead of NVS.
 *   ESPAsyncWebServer instead of esp_http_server.
 *   mDNS via ESP8266mDNS (requires MDNS.update() in loop).
 *   Telemetry: sends left/right track state + rssi. No battery/temp on bare
 *   D1 Mini — add an ADC voltage divider and the i_battery equivalent if needed.
 *
 * OTA update
 * ─────────────────────────────────────────────────────────────────────────────
 *   POST /ota/firmware   — upload a new .bin (Arduino sketch binary)
 *   POST /ota/filesystem — upload a new LittleFS image
 *   Both endpoints accept the binary as the raw POST body.
 *   Auth: include X-OTA-Token header matching OTA_AUTH_TOKEN (empty = open).
 *   No rollback on ESP8266 — the Update library writes and reboots directly.
 *   If the new firmware fails to boot, hold D3 for 3 s to reprovision, or
 *   reflash over USB.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <Updater.h>

/* Sign convention for this board's motor wiring. See header for explanation. */
#define DRIVE_LEFT_SIGN  -1
#define DRIVE_RIGHT_SIGN  1
#include "drive_mixer.h"

/* ── Identity ────────────────────────────────────────────────────────────────*/
static constexpr char HOSTNAME[]   = "rc-tank";
static constexpr char AP_SSID[]    = "RC-Tank-Setup";
static constexpr char CREDS_FILE[] = "/wifi.json";

/* ── Pinout ──────────────────────────────────────────────────────────────────*/
static constexpr uint8_t LEFT_FWD  = D1;   /* GPIO 5  */
static constexpr uint8_t LEFT_REV  = D2;   /* GPIO 4  */
static constexpr uint8_t RIGHT_FWD = D5;   /* GPIO 14 */
static constexpr uint8_t RIGHT_REV = D6;   /* GPIO 12 */
static constexpr uint8_t RESET_BTN = D3;   /* GPIO 0 — FLASH button, active LOW */
static constexpr uint8_t LED_PIN   = 2;    /* GPIO 2 — active LOW, shared with TX */

/* ── Tuning ──────────────────────────────────────────────────────────────────*/
static constexpr int          MAX_PWM       = 1023;
static constexpr int          PWM_DEADBAND  = 20;
static constexpr int          RAMP_STEP     = 40;
static constexpr int          DIR_LOCK_PWM  = 80;

static constexpr unsigned long MOTOR_TICK_MS = 20;
static constexpr unsigned long TELEMETRY_MS  = 200;   /* 5 Hz — matches S3 */
static constexpr unsigned long WATCHDOG_MS   = 400;   /* matches S3 DRIVE_WATCHDOG_MS */
static constexpr unsigned long CLEANUP_MS    = 5000;
static constexpr unsigned long MDNS_TICK_MS  = 100;
static constexpr unsigned long RESET_HOLD_MS = 3000;

/* ═════════════════════════════════════════════════════════════════════════════
 *  LED — pattern engine
 *
 *  Mirrors the S3's o_led.c patterns exactly. Ticker fires every 50 ms from
 *  an os_timer ISR to advance the step counter and compute the next duty
 *  value, but does NOT call analogWrite(). The pending duty is written to
 *  s_led_pending_duty and a flag is set. loop() calls ledApply() which does
 *  the actual analogWrite() in normal (non-ISR) context.
 *
 *  Active-LOW inversion: duty 0 = full brightness, 1023 = off.
 *  LED_DUTY(n) converts a 0–1023 "logical brightness" to the inverted value.
 * ═════════════════════════════════════════════════════════════════════════════*/

enum class LedPattern : uint8_t {
    OFF,
    ON,
    SLOW_BLINK,    /*  1 Hz  — booting / disarmed          */
    FAST_BLINK,    /*  5 Hz  — connecting                   */
    DOUBLE_BLINK,  /* two pulses then dark — error / e-stop */
    HEARTBEAT,     /* long on + dim echo — running / armed  */
};

#define LED_FULL  1023
#define LED_DIM   341    /* ~1/3 of full — matches S3's DUTY_MAX/3 */
#define LED_OFF_V 0      /* logical off → inverted to 1023 at write */

/* Active-LOW inversion applied at write time, not in the pattern table. */
#define LED_INVERT(duty)  (LED_FULL - (duty))

static volatile LedPattern s_led_pattern = LedPattern::OFF;
static volatile int        s_led_step    = 0;
static volatile int        s_led_duty    = 0;   /* logical 0–1023; inverted on write */
static volatile bool       s_led_dirty   = false;

static Ticker s_led_ticker;

/* Called from Ticker ISR — no analogWrite(), only arithmetic and flag. */
static void IRAM_ATTR ledTick()
{
    LedPattern pat  = s_led_pattern;
    int        step = s_led_step;
    int        duty = 0;

    switch (pat) {
    case LedPattern::OFF:
        duty = 0;
        break;

    case LedPattern::ON:
        duty = LED_FULL;
        break;

    case LedPattern::SLOW_BLINK:
        /* 1 Hz: 10 ticks on (500 ms), 10 ticks off */
        duty = (step < 10) ? LED_FULL : 0;
        s_led_step = (step + 1) % 20;
        break;

    case LedPattern::FAST_BLINK:
        /* 5 Hz: 2 ticks on (100 ms), 2 ticks off */
        duty = (step < 2) ? LED_FULL : 0;
        s_led_step = (step + 1) % 4;
        break;

    case LedPattern::DOUBLE_BLINK:
        /* Two short pulses then dark — 20-tick (1 s) cycle */
        if      (step < 2)  duty = LED_FULL;
        else if (step < 4)  duty = 0;
        else if (step < 6)  duty = LED_FULL;
        else                duty = 0;
        s_led_step = (step + 1) % 20;
        break;

    case LedPattern::HEARTBEAT:
        /* Long on, brief off, dim pulse, off — 20-tick (1 s) cycle.
         * Matches the S3 pattern: running / armed indicator. */
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

/* Call from loop() — safe to call analogWrite() here. */
static void ledApply()
{
    if (!s_led_dirty) return;
    s_led_dirty = false;
    analogWrite(LED_PIN, LED_INVERT(s_led_duty));
}

/* Public-facing LED API — mirrors o_led.h interface names. */

/* Set a continuous brightness (0.0 = off, 1.0 = full).
 * Stops any running pattern. Step reset before pattern change — same
 * write-order discipline as the S3 to avoid stale step with new pattern. */
static void ledSet(float brightness)
{
    brightness = constrain(brightness, 0.0f, 1.0f);
    s_led_step    = 0;
    s_led_pattern = LedPattern::OFF;
    /* Write duty directly; bypass ISR path for immediate effect. */
    analogWrite(LED_PIN, LED_INVERT((int)(brightness * LED_FULL)));
}

static void ledBlink(LedPattern pattern)
{
    s_led_step    = 0;   /* reset before pattern — same order as S3 */
    s_led_pattern = pattern;
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

static void motorTick(Motor &m)
{
    if (m.target < PWM_DEADBAND) {
        m.pwm   = 0;
        m.state = DriveState::STOPPED;
    } else if (m.state == DriveState::STOPPED) {
        m.state = DriveState::RAMP_UP;
        m.pwm   = 0;
    } else if (m.state == DriveState::RAMP_UP) {
        m.pwm = min(m.pwm + RAMP_STEP, m.target);
        if (m.pwm >= m.target) m.state = DriveState::RUNNING;
    } else {
        m.pwm = m.target;
    }
    motorWrite(m);
}

/* Direction reversal blocked above DIR_LOCK_PWM to protect H-bridge. */
static void motorSetDuty(Motor &m, float duty)
{
    const bool want_fwd = (duty >= 0.0f);
    const int  mag      = min((int)(fabsf(duty) * MAX_PWM), MAX_PWM);

    if (want_fwd != m.forward && m.pwm > DIR_LOCK_PWM) return;

    m.forward = want_fwd;
    m.target  = mag;

    if (mag < PWM_DEADBAND)
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

static void applyAxes(float x, float y)
{
    drive_out_t out = drive_mix(x, y);
    motorSetDuty(s_left,  out.left);
    motorSetDuty(s_right, out.right);
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  GLOBAL STATE
 * ═════════════════════════════════════════════════════════════════════════════*/

static bool          s_armed          = false;
static bool          s_provisioning   = false;
static bool          s_watchdog_fired = false;
static unsigned long s_last_command   = 0;
static unsigned long s_reboot_at      = 0;

static unsigned long s_last_motor   = 0;
static unsigned long s_last_telem   = 0;
static unsigned long s_last_cleanup = 0;
static unsigned long s_last_mdns    = 0;

static AsyncWebServer s_http(80);
static AsyncWebSocket s_ws("/ws");
static DNSServer      s_dns;

/* ═════════════════════════════════════════════════════════════════════════════
 *  CREDENTIALS  (LittleFS JSON: {"s":"ssid","p":"pass"})
 * ═════════════════════════════════════════════════════════════════════════════*/

static bool parseCreds(const String &json, String &ssid, String &pass)
{
    int si = json.indexOf("\"s\":\"");
    int pi = json.indexOf("\"p\":\"");
    if (si < 0 || pi < 0) return false;
    si += 5; int se = json.indexOf('"', si);
    pi += 5; int pe = json.indexOf('"', pi);
    if (se < 0 || pe < 0) return false;
    ssid = json.substring(si, se);
    pass = json.substring(pi, pe);
    return ssid.length() > 0;
}

static bool loadCreds(String &ssid, String &pass)
{
    if (!LittleFS.exists(CREDS_FILE)) return false;
    File f = LittleFS.open(CREDS_FILE, "r");
    if (!f) return false;
    String json = f.readString();
    f.close();
    return parseCreds(json, ssid, pass);
}

static void saveCreds(const String &ssid, const String &pass)
{
    File f = LittleFS.open(CREDS_FILE, "w");
    if (!f) return;
    f.print("{\"s\":\""); f.print(ssid);
    f.print("\",\"p\":\""); f.print(pass);
    f.print("\"}");
    f.close();
    Serial.printf("Credentials saved: %s\n", ssid.c_str());
}

static void clearCreds()
{
    LittleFS.remove(CREDS_FILE);
    Serial.println("Credentials cleared");
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  WEBSOCKET DISPATCH
 * ═════════════════════════════════════════════════════════════════════════════*/

static const char *driveStateName(DriveState s)
{
    switch (s) {
        case DriveState::STOPPED: return "STOPPED";
        case DriveState::RAMP_UP: return "RAMP_UP";
        case DriveState::RUNNING: return "RUNNING";
        default:                  return "UNKNOWN";
    }
}

static void broadcastTelemetry()
{
    if (s_ws.count() == 0) return;

    /* Flat JSON + left/right track objects. script.js uses track objects when
     * present (D1 Mini path) and falls back to local axis math when absent
     * (S3 path, unless S3 health_monitor is extended to emit them too). */
    char buf[220];
    snprintf(buf, sizeof(buf),
        "{"
        "\"rssi\":%d,"
        "\"left\":{\"speed\":%d,\"state\":\"%s\",\"forward\":%s},"
        "\"right\":{\"speed\":%d,\"state\":\"%s\",\"forward\":%s}"
        "}",
        (int)WiFi.RSSI(),
        s_left.pwm,  driveStateName(s_left.state),  s_left.forward  ? "true" : "false",
        s_right.pwm, driveStateName(s_right.state), s_right.forward ? "true" : "false");

    s_ws.textAll(buf);
}

static void handleMessage(AsyncWebSocketClient *client, const char *msg)
{
    drive_cmd_t cmd;
    if (!drive_parse(msg, &cmd)) {
        Serial.printf("WS unknown: %s\n", msg);
        return;
    }

    /* Every recognised message resets the watchdog — same policy as S3. */
    s_last_command   = millis();
    s_watchdog_fired = false;

    switch (cmd.type) {
        case DRIVE_CMD_PING:
            client->text("pong");
            break;

        case DRIVE_CMD_ARM:
            s_armed = true;
            ledBlink(LedPattern::HEARTBEAT);
            Serial.println("ARMED");
            break;

        case DRIVE_CMD_DISARM:
            s_armed = false;
            hardStop();
            ledBlink(LedPattern::SLOW_BLINK);
            Serial.println("DISARMED");
            break;

        case DRIVE_CMD_STOP:
            hardStop();
            break;

        case DRIVE_CMD_AXES:
            if (s_armed) applyAxes(cmd.x, cmd.y);
            break;

        case DRIVE_CMD_LED:
            /* UI sends joystick magnitude as LED brightness — use it as-is.
             * Active-LOW inversion is handled inside ledSet(). */
            ledSet(cmd.led);
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
            ledBlink(LedPattern::SLOW_BLINK);   /* client connected but not yet armed */
            break;

        case WS_EVT_DISCONNECT:
            Serial.printf("WS client #%u disconnected\n", client->id());
            s_armed = false;
            hardStop();
            /* If no clients remain, go back to slow blink (idle/connected state).
             * If clients remain (multi-browser), keep current pattern. */
            if (s_ws.count() == 0)
                ledBlink(LedPattern::SLOW_BLINK);
            break;

        case WS_EVT_DATA: {
            const auto *info = reinterpret_cast<AwsFrameInfo *>(arg);
            if (info->opcode != WS_TEXT || len == 0) break;
            data[len] = '\0';
            handleMessage(client, reinterpret_cast<char *>(data));
            break;
        }

        default:
            break;
    }
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  PROVISIONING MODE
 * ═════════════════════════════════════════════════════════════════════════════*/

static void startProvisioningMode()
{
    s_provisioning = true;
    Serial.printf("AP: %s (open)\n", AP_SSID);
    ledBlink(LedPattern::FAST_BLINK);   /* "connecting / setup" indicator */

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);
    delay(100);

    WiFi.scanNetworksAsync([](int n) {
        Serial.printf("Initial scan: %d networks\n", n);
    }, false);

    s_dns.start(53, "*", WiFi.softAPIP());
    Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());

    /* OS captive-portal probes */
    s_http.on("/ncsi.txt",                  HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "Microsoft Connect Test"); });
    s_http.on("/connecttest.txt",           HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "Microsoft Connect Test"); });
    s_http.on("/redirect",                  HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "Microsoft Connect Test"); });
    s_http.on("/hotspot-detect.html",       HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/html",  "<HTML><BODY>Success</BODY></HTML>"); });
    s_http.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/html",  "<HTML><BODY>Success</BODY></HTML>"); });
    s_http.on("/generate_204",              HTTP_GET, [](AsyncWebServerRequest *r){ r->send(204); });
    s_http.on("/gen_204",                   HTTP_GET, [](AsyncWebServerRequest *r){ r->send(204); });
    s_http.on("/success.txt",               HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "success"); });

    s_http.on("/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
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

    s_http.on("/save", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("ssid", true)) { req->send(400, "text/plain", "Missing SSID"); return; }
        String ssid = req->getParam("ssid", true)->value();
        String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
        ssid.trim();
        if (ssid.length() == 0) { req->send(400, "text/plain", "SSID empty"); return; }
        saveCreds(ssid, pass);
        s_reboot_at = millis() + 1200;
        req->send(200, "text/plain", "Saved. Rebooting...");
    });

    s_http.onNotFound([](AsyncWebServerRequest *req) {
        req->redirect("http://192.168.4.1/setup.html");
    });

    s_http.serveStatic("/", LittleFS, "/").setDefaultFile("setup.html");
    s_http.begin();
    Serial.println("Provisioning server ready");
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  NORMAL OPERATION
 * ═════════════════════════════════════════════════════════════════════════════*/

static void startNormalMode(const String &ssid, const String &pass)
{
    ledBlink(LedPattern::FAST_BLINK);   /* "connecting" — same as S3 */
    Serial.printf("Connecting to %s", ssid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.hostname(HOSTNAME);
    WiFi.begin(ssid, pass);

    const unsigned long deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        ledApply();   /* service LED while blocking — keeps pattern alive */
        delay(50);
        if (millis() % 250 < 50) Serial.print('.');
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi failed — clearing credentials");
        ledBlink(LedPattern::DOUBLE_BLINK);
        clearCreds();
        startProvisioningMode();
        return;
    }

    Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
    ledBlink(LedPattern::SLOW_BLINK);   /* connected, no client yet */

    if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("ws",   "tcp", 80);
        Serial.printf("mDNS: http://%s.local\n", HOSTNAME);
    } else {
        Serial.println("mDNS failed — still reachable by IP");
    }

    s_ws.onEvent(onWsEvent);
    s_http.addHandler(&s_ws);
    s_http.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    /* ── OTA firmware update endpoint ────────────────────────────────────
     * Accepts a raw .bin POST body. Streams directly into the flash write
     * buffer via the ESP8266 Update library — no RAM buffer needed.
     * If OTA_AUTH_TOKEN is defined and non-empty, the X-OTA-Token header
     * must match before any flash operation begins. */
    s_http.on("/ota/firmware", HTTP_POST,
        /* onRequest — sent after body is received; reboot here */
        [](AsyncWebServerRequest *req) {
            bool ok = !Update.hasError();
            AsyncWebServerResponse *resp = req->beginResponse(
                ok ? 200 : 500, "text/plain",
                ok ? "OK — rebooting" : "Update failed");
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) {
                delay(500);   /* give response time to reach browser */
                ESP.restart();
            }
        },
        /* onBody — called for each chunk of the POST body */
        [](AsyncWebServerRequest *req, uint8_t *data,
           size_t len, size_t index, size_t total) {

#ifdef OTA_AUTH_TOKEN
            if (sizeof(OTA_AUTH_TOKEN) > 1) {
                String tok = req->header("X-OTA-Token");
                if (tok != OTA_AUTH_TOKEN) {
                    req->send(401, "text/plain", "Bad or missing X-OTA-Token");
                    return;
                }
            }
#endif
            if (index == 0) {
                Serial.printf("Firmware OTA start, size=%u", (unsigned)total);
                /* U_FLASH = sketch partition. Free space must exceed total. */
                if (!Update.begin(total, U_FLASH)) {
                    Update.printError(Serial);
                }
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (index + len == total) {
                if (!Update.end(true)) {
                    Update.printError(Serial);
                }
                Serial.printf("Firmware OTA complete (%u bytes)", (unsigned)total);
            }
        });

    /* ── OTA filesystem update endpoint ──────────────────────────────────
     * Accepts a raw LittleFS image .bin as the POST body.
     * Writes to the filesystem partition (U_FS). No reboot needed —
     * LittleFS remounts automatically after the write completes. */
    s_http.on("/ota/filesystem", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            bool ok = !Update.hasError();
            req->send(ok ? 200 : 500, "text/plain",
                      ok ? "OK — filesystem updated" : "Filesystem update failed");
        },
        [](AsyncWebServerRequest *req, uint8_t *data,
           size_t len, size_t index, size_t total) {

#ifdef OTA_AUTH_TOKEN
            if (sizeof(OTA_AUTH_TOKEN) > 1) {
                String tok = req->header("X-OTA-Token");
                if (tok != OTA_AUTH_TOKEN) {
                    req->send(401, "text/plain", "Bad or missing X-OTA-Token");
                    return;
                }
            }
#endif
            if (index == 0) {
                Serial.printf("Filesystem OTA start, size=%u", (unsigned)total);
                LittleFS.end();   /* unmount before writing */
                if (!Update.begin(total, U_FS)) {
                    Update.printError(Serial);
                }
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (index + len == total) {
                if (!Update.end(true)) {
                    Update.printError(Serial);
                } else {
                    LittleFS.begin();   /* remount */
                    Serial.printf("Filesystem OTA complete (%u bytes)", (unsigned)total);
                }
            }
        });

    s_http.begin();

    s_last_command = millis();
    Serial.println("HTTP + WebSocket + OTA ready");
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  RESET BUTTON  (hold FLASH/D3 for RESET_HOLD_MS on boot)
 * ═════════════════════════════════════════════════════════════════════════════*/

static void checkResetButton()
{
    if (digitalRead(RESET_BTN) == HIGH) return;

    Serial.print("Reset held — hold for 3 s to clear credentials...");
    const unsigned long start = millis();
    while (digitalRead(RESET_BTN) == LOW) {
        unsigned long held = millis() - start;
        if (held >= RESET_HOLD_MS) {
            Serial.println(" done.");
            clearCreds();
            ledSet(1.0f);   /* solid on while erasing — matches S3 behaviour */
            return;
        }
        /* Fast blink during hold for visual feedback. Drive it directly here
         * since the Ticker hasn't started yet at this point in boot. */
        digitalWrite(LED_PIN, (held / 100) % 2 == 0 ? LOW : HIGH);
        delay(50);
    }
    digitalWrite(LED_PIN, HIGH);   /* off */
    Serial.println(" released early — cancelled");
}

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

    /* LED — configure before Ticker starts so the pin is in a known state.
     * pinMode OUTPUT is required; without it analogWrite() on GPIO2 has no
     * effect on some ESP8266 SDK versions. Start with LED off (HIGH). */
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    /* Reprovision button */
    pinMode(RESET_BTN, INPUT);

    /* Start LED pattern ticker now — patterns will be visible during WiFi
     * init. 50 ms period matches the S3's FreeRTOS timer interval. */
    s_led_ticker.attach_ms(50, ledTick);
    ledBlink(LedPattern::SLOW_BLINK);   /* "booting" — same as S3 */

    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed — halting");
        ledBlink(LedPattern::DOUBLE_BLINK);
        return;
    }

    checkResetButton();

    String ssid, pass;
    if (loadCreds(ssid, pass))
        startNormalMode(ssid, pass);
    else
        startProvisioningMode();
}

void loop()
{
    /* Apply any pending LED duty change from the Ticker ISR. Must be first
     * so patterns stay responsive even when other work takes time. */
    ledApply();

    if (s_provisioning) {
        s_dns.processNextRequest();
        if (s_reboot_at && millis() >= s_reboot_at) ESP.restart();
        return;
    }

    const unsigned long now = millis();

    /* mDNS requires regular update() calls on ESP8266. */
    if (now - s_last_mdns >= MDNS_TICK_MS) {
        s_last_mdns = now;
        MDNS.update();
    }

    /* Command watchdog — disarm and stop if no message within WATCHDOG_MS.
     * Matches the S3's ctrl_drive watchdog behaviour. */
    if (!s_watchdog_fired && (now - s_last_command > WATCHDOG_MS)) {
        s_armed          = false;
        s_watchdog_fired = true;
        hardStop();
        ledBlink(LedPattern::DOUBLE_BLINK);   /* distinctive error pattern */
        Serial.println("Watchdog: no command — disarmed and stopped");
    }

    /* Motor ramp tick. */
    if (now - s_last_motor >= MOTOR_TICK_MS) {
        s_last_motor = now;
        motorTick(s_left);
        motorTick(s_right);
    }

    /* Telemetry broadcast. */
    if (now - s_last_telem >= TELEMETRY_MS) {
        s_last_telem = now;
        broadcastTelemetry();
    }

    /* Periodic WS cleanup — frees memory from stale connections. */
    if (now - s_last_cleanup >= CLEANUP_MS) {
        s_last_cleanup = now;
        s_ws.cleanupClients();
    }
}
