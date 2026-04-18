# robot-provisioning

WiFi provisioning and RC control firmware for the **Heltec WiFi LoRa 32 V3** (ESP32-S3).

Pure ESP-IDF 5.x — no Arduino, no Espressif provisioning app, no BLE.
Connect via browser on any phone or PC.

---

## How it works

1. On boot the device tries stored WiFi credentials from NVS.
2. If none exist (or connection fails) it starts a **SoftAP + DNS hijack + captive portal**.
3. Your phone sees the AP. The OLED shows two QR codes:
   - **WIFI QR** — scan with camera to auto-join the setup AP.
   - **URL QR** — `http://192.168.4.1` — scan after joining if the captive redirect didn't fire.
4. The portal lists nearby networks. Pick yours, enter your password, submit.
5. Credentials saved to NVS. Device connects as STA and shows the control page QR + IP on the OLED.
6. On future boots the device connects straight to your network — no portal.

Once connected, the OLED shows a QR code for the robot control page alongside the IP address. The QR disappears once a WebSocket client connects, replaced by a plain IP display. When the browser disconnects the QR returns automatically.

---

## Project vision

Inspired by Meshtastic — but built for robotics, not messaging. Goal: a self-contained ESP-IDF foundation that handles the hard infrastructure (WiFi provisioning, OTA, captive portal, health monitoring, display, WebSocket control) so new use cases can be bootstrapped quickly by swapping a frontend template and implementing a thin application task.

No cloud required. No subscriptions. Optional cloud integrations are fine as add-ons.

**Target use cases** (all sharing the same core):

- Realtime RC control (tank, rover, arm)
- Device-to-device control (direct or through a gateway)
- Sequenced command playback (pre-arranged move lists)
- Autonomous with periodic status push (e.g. chicken coop door → Home Assistant)
- Smart home platform integration (Home Assistant, Google, Amazon)
- Custom Unity / game-engine app as frontend

---

## Hardware

| Board   | Heltec WiFi LoRa 32 V3 (HTIT-WB32LA) |
| ------- | ------------------------------------ |
| MCU     | ESP32-S3FN8 @ 240 MHz dual-core LX7  |
| Display | 0.96″ SSD1306 OLED 128×64 I2C        |
| Flash   | 8 MB                                 |
| PSRAM   | None (V3) / 2 MB (V4)                |

### Pin assignments

| Function    | GPIO |
| ----------- | ---- |
| OLED SDA    | 17   |
| OLED SCL    | 18   |
| OLED RST    | 21   |
| Vext power  | 36   |
| User button | 0    |

Motor pins and all other assignments are in `main/config.h`.

---

## Setup

### Prerequisites

- [VS Code](https://code.visualstudio.com/) + [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- ESP-IDF 5.x (PlatformIO downloads this automatically)
- Git

### Clone and get components

```bash
git clone <this-repo> robot-provisioning
cd robot-provisioning

# QR code generator (C port, optimised for embedded)
git submodule add https://github.com/nayuki/QR-Code-generator.git components/QR-Code-generator

# u8g2 main library
git submodule add https://github.com/olikraus/u8g2.git components/u8g2

# u8g2 HAL — download manually, do NOT add as submodule.
# Needs edits to work with ESP32-S3: delete idf_component.yml, patch I2C host names.
# Source: https://github.com/mdvorak/esp-u8g2-hal → components/u8g2_hal

# LittleFS
git submodule add https://github.com/joltwallet/esp_littlefs.git components/esp_littlefs
cd components/esp_littlefs && git submodule update --init --recursive && cd ../..
```

### Configure

Edit `main/config.h`:

```c
#define AP_SSID     "RobotSetup"   // Name of the device's own setup AP
#define AP_PASSWORD "robot1234"    // Min 8 chars. "" for open (not recommended)
```

All other tunables — pin assignments, timing constants, motor driver mode, debug flags — are also in `config.h`.

### Build and flash

```
PlatformIO: Build    (Ctrl+Alt+B)
PlatformIO: Upload   (Ctrl+Alt+U)
PlatformIO: Monitor  (Ctrl+Alt+S)
```

Or from the terminal:

```bash
pio run -t upload -t monitor
```

Upload the web files to LittleFS separately:

```bash
pio run -t uploadfs
```

### Re-provisioning

Hold the **USER button (GPIO0)** for 3 seconds at boot → credentials erased → portal starts.

### First-time setup: sdkconfig.defaults (ESP-IDF targets only)

The following keys must be present in `sdkconfig.defaults` before your first build. PlatformIO processes this file when it generates the IDF configuration — do not set these in `platformio.ini` `build_flags`.

```
# WebSocket support (required for the control page)
CONFIG_HTTPD_WS_SUPPORT=y

# OTA rollback — new firmware boots in "pending verify" state;
# rolls back automatically if the app crashes before marking itself valid.
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

# Allow OTA over plain HTTP (required unless you add TLS certificate handling)
CONFIG_OTA_ALLOW_HTTP=y
```

These settings have no equivalent on the D1 Mini (Arduino framework) — the ESP8266 `Updater` library handles OTA without menuconfig.

---

## OTA updates

Both firmware and web UI (LittleFS filesystem) can be updated over WiFi while the device is running. No USB cable, no serial programmer.

### Endpoints (all targets)

| Endpoint          | Method | Body                | Effect                              |
| ----------------- | ------ | ------------------- | ----------------------------------- |
| `/ota/firmware`   | POST   | raw `.bin`          | Flash new firmware, reboot          |
| `/ota/filesystem` | POST   | raw LittleFS `.bin` | Reflash web UI, remount — no reboot |

### Authentication

Set `OTA_AUTH_TOKEN` in `platformio.ini` `build_flags` to require a token header on every OTA request. Leave it commented out for open access on a trusted LAN.

```ini
; Inside [env:robot_esp32], [env:ttgo_lora32_v1], or [env:d1_mini]:
-DOTA_AUTH_TOKEN="your-secret-token"
```

Include the header in every OTA request:

```
X-OTA-Token: your-secret-token
```

### Rollback (ESP-IDF targets only)

After flashing, the bootloader boots the new image in a _pending verify_ state. `app_server_start()` calls `ota_server_mark_valid()` once the HTTP server is up — that call cancels the rollback. If the new firmware panics or fails to reach that point before the watchdog fires, the next power cycle automatically restores the previous image.

The D1 Mini has no rollback. If bad firmware is flashed over OTA, hold the reset button to re-enter provisioning and reflash over USB.

### How to flash OTA

Build first, then POST the binary with `curl`. The `.bin` paths are always under `.pio/build/<env>/`.

```bash
# 1. Build
pio run -e robot_esp32          # or ttgo_lora32_v1 / d1_mini

# 2. Flash firmware (replace IP with your device's address)
curl -X POST http://192.168.1.42/ota/firmware      -H "Content-Type: application/octet-stream"      --data-binary @.pio/build/robot_esp32/firmware.bin

# 3. Flash web UI separately (no reboot needed)
curl -X POST http://192.168.1.42/ota/filesystem      -H "Content-Type: application/octet-stream"      --data-binary @.pio/build/robot_esp32/littlefs.bin
```

With auth token:

```bash
curl -X POST http://192.168.1.42/ota/firmware      -H "X-OTA-Token: your-secret-token"      -H "Content-Type: application/octet-stream"      --data-binary @.pio/build/robot_esp32/firmware.bin
```

The D1 Mini advertises as `rc-tank.local` via mDNS — you can use the hostname instead of the IP after first provisioning:

```bash
curl -X POST http://rc-tank.local/ota/firmware      --data-binary @.pio/build/d1_mini/firmware.bin
```

### Partition layout

Each ESP32 target uses a dedicated partition table sized to fill its flash exactly and provide two equal firmware slots for rollback.

**Heltec V3 — `partitions_8mb.csv` (8 MB flash):**

| Partition | Size    | Purpose                           |
| --------- | ------- | --------------------------------- |
| nvs       | 20 kB   | NVS credential + settings storage |
| otadata   | 8 kB    | Active OTA slot selector          |
| ota_0     | 3.19 MB | Firmware slot A                   |
| ota_1     | 3.19 MB | Firmware slot B                   |
| storage   | 1.56 MB | LittleFS web UI                   |

**TTGO V1 — `partitions_4mb.csv` (4 MB flash):**

| Partition | Size    | Purpose                           |
| --------- | ------- | --------------------------------- |
| nvs       | 20 kB   | NVS credential + settings storage |
| otadata   | 8 kB    | Active OTA slot selector          |
| ota_0     | 1.75 MB | Firmware slot A                   |
| ota_1     | 1.75 MB | Firmware slot B                   |
| storage   | 448 kB  | LittleFS web UI                   |

The D1 Mini uses the ESP8266 Arduino core's built-in OTA partition scheme. The `Updater` library splits the 4 MB flash in half automatically — no custom partition table is needed or supported.

---

## Robot control page

Served by `app_server` on port 80 after STA connects. The setup page includes a **"Test robot now →"** skip link that jumps straight to the control page without completing WiFi setup, useful during local development.

### Controls

| Input               | How                                                  |
| ------------------- | ---------------------------------------------------- |
| Touch joystick      | Drag the puck — works in any browser                 |
| WASD keyboard       | Hold keys; axes ramp 0→1 over 180 ms                 |
| Arm / Disarm button | Must be armed before any movement command is sent    |
| PC ↔ Touch toggle   | Switches joystick visibility; keyboard always active |

Input mode switches automatically on first touch or mouse event rather than relying on a media query, which is unreliable on hybrid devices.

Both joystick and keyboard pass through the same processing pipeline: deadzone removal with rescaling (no output step at the threshold edge), followed by an exponential response curve for finer low-speed control.

### Commands sent to firmware (WebSocket text)

| Message   | Meaning                                         |
| --------- | ----------------------------------------------- |
| `x:F,y:F` | Drive axes −1.0 to +1.0, sent at 20 Hz          |
| `stop`    | Halt — sent when disarmed or both axes are zero |
| `led:F`   | LED brightness 0.00–1.00, sent on change only   |
| `ping`    | Latency probe; firmware replies `pong`          |

### Telemetry received from firmware (WebSocket JSON)

| Field     | Type          | Description                       |
| --------- | ------------- | --------------------------------- |
| `rssi`    | int           | WiFi signal dBm                   |
| `battery` | int           | Battery %                         |
| `temp`    | float \| null | Temperature °C; null if no sensor |
| `uptime`  | int           | Seconds since boot                |
| `heap`    | int           | Free heap bytes                   |
| `errors`  | int           | Cumulative error count            |

Track card speed bars are driven locally from the commanded axes (arcade mix mirrored from `ctrl_drive.c`) rather than from firmware telemetry, giving immediate visual feedback. Heap, uptime, and errors are shown in a collapsible diagnostics drawer rather than the primary UI.

---

## Firmware architecture

### Module map

```
main/
  main.c              Boot sequence only. No logic beyond init ordering and main loop.
  config.h            All compile-time constants: pins, timing, thresholds, debug flags.

  ── WiFi / Network ──────────────────────────────────────────────────────────
  wifi_manager.c/.h   STA/AP state machine + NVS credential storage.
  portal.c/.h         SoftAP captive portal: DNS hijack task, OS probe handlers
                      (Windows/iOS/Android), network scan endpoint, credential POST.
                      Owns one httpd instance.
  app_server.c/.h     Robot control HTTP file server + WebSocket dispatch (port 80).
                      Owns a second httpd instance.

  ── Display ─────────────────────────────────────────────────────────────────
  display.c/.h        u8g2 wrapper: mutex-guarded draw calls, dirty-flag flush.
                      Graceful no-op when OLED absent.
  prov_ui.c/.h        OLED state machine driven by wifi_manager callbacks.
                      Connected screen: QR centred above full-width IP row —
                      no side column, so the IP never clips on any address format.
  qr_gen.c/.h         QR encode helpers (WiFi URI, URL). Auto scale 3→2→1.

  ── Inputs (i_) ─────────────────────────────────────────────────────────────
  i_battery.c/.h      ADC oneshot read of VBAT via voltage divider. Rolling average.
                      IDF 5.x adc_cali_* scheme; falls back gracefully without eFuse data.
  i_sensors.c/.h      I2C bus owner. Re-inits bus after WiFi startup (WiFi resets the
                      peripheral on ESP32-S3). Scans bus, maps peripherals, exposes a
                      snapshot struct. Sensor driver tick added here as hardware is added.

  ── Outputs (o_) ────────────────────────────────────────────────────────────
  o_led.c/.h          Onboard white LED on GPIO35 via LEDC PWM. Non-blocking blink
                      patterns driven by a 50 ms FreeRTOS software timer.
  o_motors.c/.h       Two brushed DC motors. Two topologies selected by config.h:
                        MODE_DIR_PWM_EN  — DIR + PWM + EN per channel.
                        MODE_BTN8982     — BTN8982 H-bridge (IN1/IN2 PWM per side).

  ── Controllers (ctrl_) ─────────────────────────────────────────────────────
  ctrl_drive.c/.h     Differential drive: converts joystick (x, y) → per-track duties
                      via arcade mix (left = y+x, right = y−x), slew-rate ramp, and
                      o_motors_drive(). Owns arming state. Command watchdog: auto-disarms
                      and stops motors if no command received within DRIVE_WATCHDOG_MS
                      (default 400 ms), catching browser crash and WiFi drop silently.

  ── Health ──────────────────────────────────────────────────────────────────
  health_monitor.c/.h Periodic RSSI check. Reads i_battery and i_sensors snapshots.
                      Builds the flat telemetry JSON blob pushed by app_server.
```

### Boot sequence

```
display_init()          Power OLED via Vext, send SSD1306 init sequence.
o_led_init()            Configure LEDC PWM on GPIO35.
i_battery_init()        Configure ADC1 with curve-fitting calibration.
prov_ui_show_boot()     Immediate splash screen.
prov_ui_init()          Pre-generate WiFi + setup URL QR codes (~50 ms).
reprovision_check()     Non-blocking GPIO0 poll over REPROV_HOLD_MS.
wifi_manager_start()    Try NVS credentials → fall back to captive portal.
i_sensors_init()        Re-init I2C post-WiFi, scan bus, call display_reinit_i2c().
ctrl_drive_init()       Configure motor driver hardware.
health_monitor_init()   Cache peripheral map, baseline telemetry.
app_server_start()      HTTP file server + WebSocket on port 80.
[main loop 10 Hz]       prov_ui_tick(), health_monitor_tick().
[on STA connect]        Spawn app_task → ctrl_drive_tick(), i_sensors_tick(),
                        app_server_push_telemetry() at 5 Hz.
```

### I2C timing constraint

`esp_wifi_start()` resets the I2C peripheral on ESP32-S3 as a side-effect of the radio clock tree bringup. `display_init()` runs before WiFi and owns the bus for OLED init. After `wifi_manager_start()` returns, `i_sensors_init()` calls `display_reinit_i2c()` (which tears down and recreates the bus/device handles via the new `driver/i2c_master.h` API) before scanning. No delay hacks — handle lifetimes are managed explicitly.

### Motor driver modes

Set `MOTOR_DRIVER_MODE` in `config.h`:

| Mode                    | Description                                                                     |
| ----------------------- | ------------------------------------------------------------------------------- |
| `MOTOR_MODE_DIR_PWM_EN` | DIR pin sets direction, PWM sets speed, EN enables. One LEDC channel per motor. |
| `MOTOR_MODE_BTN8982`    | BTN8982 H-bridge: IN1/IN2 PWM per half-bridge. Four LEDC channels total.        |

### Design decisions

**File system:** LittleFS only. SPIFFS is deprecated.

**Framework:** ESP-IDF 5.x + PlatformIO as developer shell only. PlatformIO library manager is not used for components — all vendored as git submodules or manual downloads in `components/`. This avoids duplicate symbol linker errors, allows direct patching for S3 compatibility, and locks component versions.

**Config split:** `sdkconfig.defaults` → silicon/driver settings (including `CONFIG_HTTPD_WS_SUPPORT`). `platformio.ini` → build environment metadata. `config.h` → application constants baked in at compile time. LittleFS JSON → planned runtime user settings that survive OTA.

**UI independence:** All actions work regardless of display or browser state. Motors run if the OLED is dead. Motors run if the browser disconnects (modulo the watchdog timeout). UI is telemetry + convenience, not a dependency.

**Code style:** Allman braces. Comments explain constraints, tradeoffs, and platform quirks — not what the function name already says.

**WebSocket file serving:** URI→path concatenation is deliberately avoided in both `app_server` and `portal`. httpd URIs can be up to `CONFIG_HTTPD_MAX_URI_LEN` (512 B); prepending `/littlefs` would overflow any fixed stack buffer. Named routes only; wildcard catch-all returns 404.

**WS client tracking:** `s_ws_fds[]` is a fixed array protected by a mutex. `app_server_push_telemetry()` snapshots the live fd list under the mutex, releases it, then sends — `httpd_ws_send_frame_async` posts to the httpd send queue and must not be called while holding a mutex that httpd handlers also acquire. Dead fds pruned from async sends call `prov_ui_set_client_count()` and `ctrl_drive_emergency_stop()` just as a clean WebSocket close does.

---

## File map

| File                       | Purpose                                                            |
| -------------------------- | ------------------------------------------------------------------ |
| `main/config.h`            | All compile-time constants — pins, timing, motor mode, debug flags |
| `main/main.c`              | Boot sequence, main loop, boot-loop guard, safe mode               |
| `main/wifi_manager.c/.h`   | STA/AP state machine, NVS credential storage                       |
| `main/portal.c/.h`         | SoftAP captive portal, DNS hijack, scan endpoint                   |
| `main/app_server.c/.h`     | HTTP file server, WebSocket dispatch, telemetry push               |
| `main/prov_ui.c/.h`        | OLED state machine driven by wifi_manager callbacks                |
| `main/display.c/.h`        | u8g2 wrapper — mutex-guarded draw calls, dirty-flag flush          |
| `main/qr_gen.c/.h`         | QR encode helpers                                                  |
| `main/health_monitor.c/.h` | Periodic RSSI check, telemetry JSON builder                        |
| `main/i_battery.c/.h`      | ADC battery voltage + percent                                      |
| `main/i_sensors.c/.h`      | I2C bus owner, post-WiFi reinit, peripheral scan                   |
| `main/o_led.c/.h`          | Onboard LED LEDC driver, blink patterns                            |
| `main/o_motors.c/.h`       | Brushed DC motor output, two topology modes                        |
| `main/ctrl_drive.c/.h`     | Differential drive mixing, arming, command watchdog                |
| `main/ota_server.c/.h`     | OTA firmware + filesystem update endpoints (ESP-IDF targets)       |
| `partitions_8mb.csv`       | Flash layout for Heltec V3 (8 MB — two OTA slots + LittleFS)       |
| `partitions_4mb.csv`       | Flash layout for TTGO V1 (4 MB — two OTA slots + LittleFS)         |
| `data/index.html`          | Robot control page                                                 |
| `data/style.css`           | Robot control styles                                               |
| `data/script.js`           | Robot control logic (joystick, keyboard, WebSocket, telemetry)     |

---

## Extending for a new robot application

After provisioning, `wifi_manager_is_connected()` becomes true and `app_task` is spawned in `main.c`. Add application logic there:

```c
static void app_task(void *arg)
{
    while (1)
    {
        ctrl_drive_tick();        // advances motor ramp, checks command watchdog
        i_sensors_tick();         // polls registered sensor drivers
        app_server_push_telemetry(); // WebSocket JSON at 5 Hz
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

The WebSocket dispatch in `app_server.c` routes text commands to `ctrl_drive` and `o_led`. Add new command handlers in `dispatch()`. The display is available after the provisioning success screen — call `display_draw_*` + `display_flush()` freely from any task that holds the display mutex path.

---

## Troubleshooting

| Symptom                                  | Likely cause                                                                  |
| ---------------------------------------- | ----------------------------------------------------------------------------- |
| Web files 404                            | Run `pio run -t uploadfs`                                                     |
| WebSocket not connecting                 | `CONFIG_HTTPD_WS_SUPPORT` not set in `sdkconfig.defaults`                     |
| OLED blank                               | Check Vext GPIO (36), I2C address, ribbon cable                               |
| QR not scannable                         | SSID or password too long; reduce length or lower ECC                         |
| Motors don't move                        | Check `MOTOR_DRIVER_MODE` in `config.h`; confirm `ARMED` state in UI          |
| Boot loops                               | Hold USER button (GPIO0) 3 s to erase credentials                             |
| I2C errors after WiFi                    | `i_sensors_init()` must run after `wifi_manager_start()` — see boot sequence  |
| OTA returns 500                          | Image too large for partition, or corrupted transfer — check partition sizes  |
| OTA returns 401                          | `X-OTA-Token` header missing or wrong — check `OTA_AUTH_TOKEN` in build_flags |
| No rollback after OTA                    | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` missing from `sdkconfig.defaults`   |
| Device doesn't reboot after firmware OTA | Normal — wait 2 s then reconnect                                              |
| LittleFS corrupt after filesystem OTA    | Transfer interrupted — reflash `littlefs.bin` again                           |

---

## Open work

### Firmware / backend

- [ ] **Dynamic user settings** — JSON file in LittleFS for runtime config (calibration, UI template, LoRa region). Survives OTA. Simple GET/POST API from app_server.
- [ ] **UI template switching** — store selected template name in user settings; app_server serves the matching HTML/CSS/JS. Current RC tank UI is template #1.
- [ ] **Peripheral config system** — read hardware profile from user settings at boot (which sensors, which GPIOs). `i_sensors` already scans I2C; extend to act on findings.
- [ ] **Portal UX improvements** (from WiFiManager lessons): configurable portal timeout, info page with build info + reset button, per-AP credential history, static IP option.
- [ ] **JS error reporting** — catch `window.onerror` and POST to a firmware endpoint so JS errors appear in serial log. Currently invisible once deployed.
- [ ] **Favicon** — serve a real icon from LittleFS. Currently returns 204; browsers re-request on every load.

### Communications

- [ ] **WebSocket vs UDP evaluation** — WebSocket: reliable, browser-native, ~2–5 ms overhead on LAN. UDP: lower latency, no retransmit, needs native app or browser shim. Prototype UDP and compare measured latency + jitter under load before committing.
- [ ] **BLE provisioning path** — alternative to SoftAP for phones that handle BLE pairing more gracefully than captive portals.
- [ ] **LoRa integration** — join existing Meshtastic network or run custom point-to-point protocol. Must enforce regional duty-cycle limits (EU 1%, US unrestricted ISM). Requires separate HAL per radio module.
- [ ] **Home Assistant / MQTT** — publish sensor data and accept command topics. Store broker address in user settings.
- [ ] **Device-to-device control** — one ESP32 as controller, another as actuator, over WiFi (UDP/WS) or LoRa.

### Hardware support

- [ ] **Board abstraction layer** — GPIO pin tables per board so the same firmware targets multiple variants. Candidate boards:
  - Heltec WiFi LoRa 32 V3 (current — ESP32-S3, SSD1306 I2C)
  - TTGO LoRa32 V1 (ESP32, SSD1306 I2C, AXP192 PMIC, NEO-6M GPS, onboard temp)
  - Heltec HTIT-Tracker (ESP32-S3, confirm I2C pinout)
    Each board gets a header in `components/boards/<name>.h` selected by a build flag in `platformio.ini`.
- [ ] **TTGO V1 peripherals** — AXP192 PMIC (battery voltage, charge state), NEO-6M GPS, onboard temp. Add address-to-peripheral mapping entries in `i_sensors`.
- [ ] **SPI display support** — ST7735 / ILI9341 for boards without I2C OLED. u8g2 already supports these; needs HAL config.
- [ ] **E-paper display** — useful for low-power / solar use cases.

### Quality / process

- [ ] **Unit tests** — ESP-IDF Unity framework. Highest-value targets: `qr_gen` output size vs panel, `url_decode` edge cases, `best_qr_scale` boundary values, NVS round-trip.
- [ ] **CI / automated build** — GitHub Actions with ESP-IDF Docker image. Build for ESP32-S3 on every push; fail on compiler warnings.

---

## License

MIT. See individual component licenses:

- nayuki/QR-Code-generator: MIT
- ESP-IDF components: Apache 2.0
- u8g2: 2-clause BSD
