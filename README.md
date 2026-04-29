# Robot Provisioning

Inspired by [Meshtastic](https://meshtastic.org/) — but built for robotics, not messaging. A self-contained ESP-IDF foundation handling the hard infrastructure so new use cases boot quickly by swapping a frontend template and implementing a thin application task.

**No cloud required. No subscriptions.** Optional cloud integrations are fine as add-ons.

WiFi provisioning and RC control firmware for [**Heltec WiFi LoRa 32 V3**](https://heltec.org/project/wifi-lora-32-v3/) (ESP32-S3). Also targeting TTGO LoRa32 V1 and Wemos D1 Mini (ESP8266/Arduino).

> **No Espressif provisioning app required**
> Connect via browser on any phone or PC.

[![PlatformIO](https://img.shields.io/badge/PlatformIO-6.9+-orange.svg)](https://platformio.org/)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.x-blue.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32s3/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

---

### Target Use Cases

| Use Case                    | Notes                                                                           |
| --------------------------- | ------------------------------------------------------------------------------- |
| Realtime RC control         | Tank, rover, arm — current default                                              |
| Device-to-device control    | Direct or through a gateway                                                     |
| Sequenced command playback  | Pre-arranged move lists                                                         |
| Autonomous + status push    | e.g. chicken coop door → [Home Assistant](https://www.home-assistant.io/)       |
| Smart home integration      | Home Assistant, Google, Amazon                                                  |
| Custom game-engine frontend | [Unity](https://unity.com/) / [Unreal](https://www.unrealengine.com/) app as UI |

---

## Table of Contents

- [How It Works](#how-it-works)
- [Hardware](#hardware)
- [Quick Start](#quick-start)
- [Firmware Architecture](#firmware-architecture)
- [OTA Updates](#ota-updates)
- [Robot Control Page](#robot-control-page)
- [Troubleshooting](#troubleshooting)
- [Open Work / Roadmap](#open-work--roadmap)
- [License](#license)

---

## How It Works

1. **Boot** → tries stored WiFi credentials from [NVS](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_flash.html).
2. **No creds / fails** → starts **SoftAP + DNS hijack + captive portal**.
3. **Phone joins AP** → OLED shows two QR codes:
   - **WIFI QR** — camera scan auto-joins the setup AP
   - **URL QR** — `http://192.168.4.1` — opens portal if captive redirect didn't fire
4. **Portal** → pick network, enter password, submit.
5. **Save & connect** → credentials saved to NVS. Device connects as STA.
6. **Future boots** → connects straight to your network. No portal.

Once connected, the OLED shows a QR for the robot control page + IP address. The QR disappears once a WebSocket client connects, replaced by plain IP. Disconnect → QR returns automatically.

### Re-provisioning

Hold **USER button (GPIO0)** for 3 seconds at boot → credentials erased → portal starts fresh.

---

## Hardware

| Board   | [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/) (HTIT-WB32LA)        |
| ------- | ------------------------------------------------------------------------------------------ |
| MCU     | [ESP32-S3FN8](https://www.espressif.com/en/products/socs/esp32-s3) @ 240 MHz dual-core LX7 |
| Display | [0.96″ SSD1306](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf) OLED 128×64 I2C      |
| Flash   | 8 MB                                                                                       |
| PSRAM   | None (V3) / 2 MB (V4)                                                                      |

### Pin Assignments

| Function    | GPIO |
| ----------- | ---- |
| OLED SDA    | 17   |
| OLED SCL    | 18   |
| OLED RST    | 21   |
| Vext power  | 36   |
| User button | 0    |

Motor pins and all other assignments are in [`main/config.h`](main/config.h).

---

## Quick Start

### Prerequisites

- [Python](https://www.python.org/downloads/)
- [VS Code](https://code.visualstudio.com/) + [PlatformIO IDE](https://platformio.org/install/ide?install=vscode)
- [Git](https://git-scm.com/)
- ESP-IDF 5.x (PlatformIO downloads this automatically)

### Clone & Components

```bash
git clone <this-repo> robot-provisioning
cd robot-provisioning

# QR code generator (C port, optimised for embedded)
git submodule add https://github.com/nayuki/QR-Code-generator.git \
  components/QR-Code-generator

# u8g2 display library
git submodule add https://github.com/olikraus/u8g2.git components/u8g2

# u8g2 ESP32 HAL has been patched for ESP32-S3: I2C host names and update from i2c legacy to master
# Source: https://github.com/mdvorak/esp-u8g2-hal → components/u8g2_hal

# Managed via idf_component.yml:
#   - joltwallet/littlefs
#   - espressif/mdns
```

### Configure

Edit [`main/config.h`](main/config.h):

```c
#define AP_SSID     "RobotSetup"   // Device's own setup AP name
#define AP_PASSWORD "robot1234"    // Min 8 chars. "" for open (not recommended)
```

### Build, Flash, Monitor

```bash
# Build & flash firmware
pio run -e robot_esp32 -t upload

# Flash web UI to LittleFS
pio run -e robot_esp32 -t uploadfs

# Serial monitor
pio device monitor -e robot_esp32
```

Or use VS Code shortcuts: **Ctrl+Alt+B** (Build), **Ctrl+Alt+U** (Upload), **Ctrl+Alt+S** (Monitor).

---

## Firmware Architecture

### Module Map

```
main/
  main.c              Boot sequence only
  config.h            Compile-time constants

  ── WiFi / Network ──────────────────────────────────────────────────────────
  wifi_manager.c/.h   STA/AP state machine + NVS credentials
  portal.c/.h         SoftAP captive portal, DNS hijack, scan endpoint
  app_server.c/.h     HTTP file server + WebSocket dispatch (port 80)

  ── Display ─────────────────────────────────────────────────────────────────
  display.c/.h        u8g2 wrapper, mutex-guarded draw calls
  prov_ui.c/.h        OLED state machine driven by wifi_manager callbacks
  qr_gen.c/.h         QR encode helpers

  ── Inputs (i_) ─────────────────────────────────────────────────────────────
  i_battery.c/.h      ADC battery voltage + percent
  i_sensors.c/.h      I2C bus owner, post-WiFi reinit, peripheral scan

  ── Outputs (o_) ────────────────────────────────────────────────────────────
  o_led.c/.h          Onboard LED LEDC driver, blink patterns
  o_motors.c/.h       Brushed DC motor output, two topology modes

  ── Controllers (ctrl_) ─────────────────────────────────────────────────────
  ctrl_drive.c/.h     Differential drive, arming, command watchdog

  ── Health ──────────────────────────────────────────────────────────────────
  health_monitor.c/.h Telemetry JSON builder

  ── OTA ─────────────────────────────────────────────────────────────────────
  ota_server.c/.h     Firmware + filesystem update endpoints
```

### Boot Sequence

```
display_init()          → Power OLED
o_led_init()            → LEDC PWM on GPIO35
i_battery_init()        → ADC1 calibration
prov_ui_show_boot()     → Splash screen
prov_ui_init()          → Pre-generate QR codes (~50 ms)
reprovision_check()     → GPIO0 poll
wifi_manager_start()    → Try NVS → portal fallback
i_sensors_init()        → Re-init I2C post-WiFi, scan bus
ctrl_drive_init()       → Motor driver hardware
health_monitor_init()   → Baseline telemetry
app_server_start()      → HTTP + WebSocket (once STA connects)
```

### I2C Timing Constraint

[`esp_wifi_start()`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html#_CPPv415esp_wifi_startv) resets the I2C peripheral on ESP32-S3 as a side-effect of radio clock tree bringup. `display_init()` runs before WiFi. After `wifi_manager_start()`, `i_sensors_init()` calls `display_reinit_i2c()` to recreate bus/device handles via the new [`driver/i2c_master.h`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2c.html) API. No delay hacks — handle lifetimes are managed explicitly.

### Motor Driver Modes

Set `MOTOR_DRIVER_MODE` in `config.h`:

| Mode                    | Description                                                                                                                                                                                       |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `MOTOR_MODE_DIR_PWM_EN` | DIR + PWM + EN per channel. One LEDC channel per motor.                                                                                                                                           |
| `MOTOR_MODE_BTN8982`    | [BTN8982](https://www.infineon.com/cms/en/product/power/motor-control-ics/intelligent-motor-control-ics/multi-half-bridge-drivers/btn8982ta/) H-bridge. IN1/IN2 PWM per side. Four LEDC channels. |

---

## OTA Updates

Both firmware and web UI can be updated over WiFi while running.

### Endpoints

| Endpoint          | Method | Body                | Effect                              |
| ----------------- | ------ | ------------------- | ----------------------------------- |
| `/ota/firmware`   | POST   | raw `.bin`          | Flash new firmware, reboot          |
| `/ota/filesystem` | POST   | raw LittleFS `.bin` | Reflash web UI, remount — no reboot |

### Authentication (optional)

Set in `platformio.ini`:

```ini
build_flags = -DOTA_AUTH_TOKEN=\"your-secret-token\"
```

Then include header:

```bash
curl -X POST http://192.168.1.42/ota/firmware \
  -H "X-OTA-Token: your-secret-token" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @.pio/build/robot_esp32/firmware.bin
```

### Rollback (ESP-IDF only)

After flashing, the bootloader boots the new image in a _pending verify_ state. `ota_server_mark_valid()` cancels rollback once the HTTP server is up. If the firmware panics before that, the next boot restores the previous image.

### Flashing Commands

```bash
# Firmware
curl -X POST http://<device-ip>/ota/firmware \
  -H "Content-Type: application/octet-stream" \
  --data-binary @.pio/build/robot_esp32/firmware.bin

# Web UI (no reboot)
curl -X POST http://<device-ip>/ota/filesystem \
  -H "Content-Type: application/octet-stream" \
  --data-binary @.pio/build/robot_esp32/littlefs.bin
```

### Partition Layouts

**Heltec V3 — 8 MB:**

| Partition | Size    | Purpose                  |
| --------- | ------- | ------------------------ |
| nvs       | 20 kB   | Credentials + settings   |
| otadata   | 8 kB    | Active OTA slot selector |
| ota_0     | 3.19 MB | Firmware slot A          |
| ota_1     | 3.19 MB | Firmware slot B          |
| storage   | 1.56 MB | LittleFS web UI          |

**TTGO V1 — 4 MB:**

| Partition | Size    | Purpose                  |
| --------- | ------- | ------------------------ |
| nvs       | 20 kB   | Credentials + settings   |
| otadata   | 8 kB    | Active OTA slot selector |
| ota_0     | 1.75 MB | Firmware slot A          |
| ota_1     | 1.75 MB | Firmware slot B          |
| storage   | 448 kB  | LittleFS web UI          |

---

## Robot Control Page

Served on port 80 after STA connects.

### Controls

| Input             | How                                  |
| ----------------- | ------------------------------------ |
| Touch joystick    | Drag the puck — works in any browser |
| WASD keyboard     | Hold keys; axes ramp 0→1 over 180 ms |
| Arm / Disarm      | Must arm before movement commands    |
| PC ↔ Touch toggle | Switches joystick visibility         |

### WebSocket Protocol

**Client → Firmware:**

| Message   | Meaning                        |
| --------- | ------------------------------ |
| `x:F,y:F` | Drive axes −1.0 to +1.0, 20 Hz |
| `stop`    | Halt                           |
| `led:F`   | LED brightness 0.00–1.00       |
| `ping`    | Latency probe; replies `pong`  |

**Firmware → Client (JSON telemetry):**

| Field     | Type          | Description            |
| --------- | ------------- | ---------------------- |
| `rssi`    | int           | WiFi signal dBm        |
| `battery` | int           | Battery %              |
| `temp`    | float \| null | Temperature °C         |
| `uptime`  | int           | Seconds since boot     |
| `heap`    | int           | Free heap bytes        |
| `errors`  | int           | Cumulative error count |

---

## Troubleshooting

| Symptom                  | Likely Cause                                    | Fix                                       |
| ------------------------ | ----------------------------------------------- | ----------------------------------------- |
| Web files 404            | LittleFS not mounted                            | `pio run -t uploadfs`                     |
| WebSocket not connecting | `CONFIG_HTTPD_WS_SUPPORT` missing               | Add to `sdkconfig.defaults`               |
| OLED blank               | Vext GPIO (36), I2C address, cable              | Check hardware, `i_sensors_init()` log    |
| QR not scannable         | SSID/password too long                          | Reduce length or lower ECC in `qr_gen.c`  |
| Motors don't move        | Wrong `MOTOR_DRIVER_MODE` / not armed           | Check `config.h`, arm in UI               |
| Boot loops               | Corrupt NVS or WiFi init fail                   | Hold GPIO0 3s to erase credentials        |
| I2C errors after WiFi    | `i_sensors_init()` order wrong                  | Must run **after** `wifi_manager_start()` |
| OTA 500                  | Image too large for partition                   | Check partition sizes vs `.bin`           |
| OTA 401                  | Missing `X-OTA-Token`                           | Check `OTA_AUTH_TOKEN` in build flags     |
| No rollback              | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` missing | Add to `sdkconfig.defaults`               |

---

## Open Work / Roadmap

&gt; **Value:** 🔴 High — prevents bricks, reduces support, or unlocks new use cases
&gt; **Value:** 🔵 Medium — improves UX or expands capabilities
&gt; **Value:** 🟢 Low — nice to have, polish, or future-proofing
&gt; **Effort:** S = hours, M = days, L = weeks

---

### Safety & Reliability

_These prevent bricks and make field debugging possible. Do these first._

- [ ] **🔴S OTA checksum verification** — SHA-256 download + MD5 transfer integrity before `esp_ota_end()`. Prevents bricks from corrupted transfers.
- [ ] **🔴S Task + heap watchdogs** — Enable FreeRTOS task watchdog (5 s timeout) and heap corruption detection. Catches infinite loops and memory corruption without physical reset.
- [ ] **🔴S Embed frontend into firmware binary** — Gzip SPA at build time, convert to C byte array. Serve from ROM. One OTA updates everything; no more "forgot `uploadfs`" failures.
- [ ] **🔵M Persistent event logging** — Compressed JSONL ring buffer on flash (last 5 sessions, ~100 KB each). Browsable/downloadable from UI. Optional remote syslog. Debug field issues without a serial cable.
- [ ] **🔵S Factory reset from UI** — Settings page button + GPIO0 hold → erase WiFi creds, reset to defaults, reboot into portal. Currently only GPIO0 works.
- [ ] **🔵S Graceful WS shutdown before OTA reboot** — Close the HTTP server and send WS close frames before `esp_restart()` so the browser UI shows "updating" instead of a hung connection.
- [ ] **🔵S Battery low warning** — Surface `BATTERY_WARN_PCT` threshold in telemetry JSON and on the OLED status bar. Prevents deep-discharge damage in the field.
- [x] **🔴S Blocking WiFi scan** — Manual refresh only via `/api/rescan`. No background task = no missed-probe reboots.
- [x] **🔴S Command watchdog** — Auto-disarm on disconnect (400 ms timeout).
- [x] **🔴S Boot-loop guard** — Safe mode after `WIFI_MAX_RESTART_ATTEMPTS`.
- [x] **🔴S OTA firmware + filesystem** — Separate endpoints, rollback support via `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.
- [x] **🟢S Boot-loop diagnostics** — Log `esp_reset_reason()` to serial at boot and display the reset cause on the safe mode screen.
- [x] **🔵S I2C re-init after WiFi** — Handles ESP32-S3 peripheral reset via `display_reinit_i2c()`.

---

### User Experience

_These reduce friction for non-technical users and cut your support burden._

- [ ] **🔴M Runtime user settings (JSON in NVS/LittleFS)** — Hostname, motor deadband, UI template, timezone, notification topic. Survives OTA. Stop recompiling `config.h` for every change.
- [ ] **🔴M Standalone flash tool** — Python/Go CLI that auto-detects USB, queries GitHub Releases, downloads correct `.bin`, flashes with bundled `esptool.py`. One command for non-technical users.
- [ ] **🔵M Portal hibernation** — After N minutes of inactivity, reduce AP TX power or stop DNS task to save power. Wake on GPIO0 press or client reconnect.
- [ ] **🔵S UI template switching** — Store template name in settings; serve matching HTML/CSS/JS. Template #1 = RC tank (current). Template #2 = sensor dashboard. Template #3 = autonomous waypoint editor.
- [ ] **🔵S Per-AP credential history** — Remember last 3 SSIDs in NVS. Dropdown in portal instead of typing from scratch.
- [ ] **🟢S Portal info page** — Build version, git hash, uptime, free heap, reset button. Helps users report bugs accurately.
- [ ] **🟢S Static IP option** — In settings, allow optional static IP/gateway/DNS instead of DHCP. Useful for fixed installations.
- [x] **🟢S Favicon** — Served from `/littlefs/favicon.svg` by `app_server.c`.
- [x] **🔵M SoftAP captive portal** — DNS hijack, OS probe handlers (Windows/iOS/Android), credential POST.
- [x] **🔵M OLED state machine** — QR codes (WiFi join, portal URL, robot URL), boot splash, connected screen.

---

### Frontend & Web UI

_These make the browser experience richer and more debuggable._

- [x] **🔵S JS error reporting** — `window.onerror` → POST `/api/jserror` so frontend bugs appear in the serial log. Currently invisible once deployed.
- [ ] **🔵S Latency display** — Show round-trip ping time in the control UI. Helps diagnose WiFi congestion.
- [ ] **🟢S Connection quality indicator** — WiFi signal bars + packet loss estimate in the UI header.
- [ ] **🟢S Dark mode toggle** — Persist preference in `localStorage`. Match `prefers-color-scheme` on first load.
- [x] **🔴M WebSocket control** — Joystick, keyboard, arming, telemetry push at 5 Hz.

---

### Communications

_These turn the robot from a LAN toy into an internet-aware device._

- [ ] **🔵M Push notifications (ntfy.sh)** — Optional alerts to phone: "Battery low", "Motor stall", "WiFi disconnected". User configures topic in settings. No app build needed.
- [ ] **🔵M Home Assistant / MQTT** — Publish telemetry to configurable broker. Accept command topics (`robot/cmd/stop`, `robot/cmd/led`). Auto-discover via HA MQTT integration.
- [ ] **🔵S NTP time sync** — Sync RTC on boot and every 4 h. Enables timestamped logs, scheduled autonomous missions, accurate uptime reporting. (ESP-IDF `esp_sntp.h` makes this small effort.)
- [ ] **🔵M WebSocket vs UDP evaluation** — Prototype UDP control channel. Measure latency + jitter under load vs current WebSocket. Decide before committing to UDP for low-latency use cases.
- [ ] **🟢L BLE provisioning path** — Alternative to SoftAP for phones that handle BLE pairing better than captive portals. ESP32-S3 has BLE 5.0. Keep SoftAP as fallback.
- [ ] **🟢L LoRa integration** — Join [Meshtastic](https://meshtastic.org/) network or custom point-to-point protocol. Must enforce regional duty-cycle limits (EU 1%, US ISM unrestricted). Requires separate radio HAL.
- [ ] **🟢L Device-to-device control** — One ESP32 as controller, another as actuator over WiFi (UDP/WS) or LoRa. Pairing via QR code or NFC bump.

---

### Hardware Abstraction

_These let the same firmware run on multiple boards without `#ifdef` spaghetti._

- [ ] **🔵M Board abstraction layer** — GPIO pin tables per board in `components/boards/&lt;name&gt;.h`. Selected by build flag. Candidates:
  - [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/) (current — ESP32-S3, SSD1306 I2C)
  - [TTGO LoRa32 V1](https://github.com/LilyGO/TTGO-LORA32) (ESP32, SSD1306 I2C, AXP192 PMIC, NEO-6M GPS)
  - [Heltec HTIT-Tracker](https://heltec.org/project/htit-tracker/) (ESP32-S3, confirm I2C pinout)
- [ ] **🔵M Peripheral auto-discovery** — `i_sensors` already scans I2C. Extend to probe known addresses (BME280 @ 0x76, AXP192 @ 0x34, etc.) and auto-populate `i_peripheral_map_t`. Settings page shows detected hardware.
- [ ] **🔵M TTGO V1 peripherals** — AXP192 PMIC driver (battery voltage, charge state, power management), NEO-6M GPS parser (UART2), onboard temperature sensor.
- [ ] **🟢M SPI display support** — ST7735 / ILI9341 for boards without I2C OLED. u8g2 already supports these; needs HAL config in board header.
- [ ] **🟢L E-paper display** — Low-power / solar use cases. Different refresh model (partial vs full updates). u8g2 has limited support; may need dedicated driver.
- [x] **🔵M Motor driver abstraction** — `MOTOR_MODE_DIR_PWM_EN` and `MOTOR_MODE_BTN8982`.

---

### Quality & Process

_These make the project maintainable as it grows and help others contribute._

- [ ] **🔵S Deduplicate `serve_file()`** — Extract the identical gzip-transparent file serving logic from `portal.c` and `app_server.c` into a shared helper (e.g., `vfs_mount.c` or `vfs_serve.h`). Shrinks binary and eliminates a maintenance hazard.
- [ ] **🔵M GitHub Discussions + Issue conventions** — Require Discussions for questions/setup help. Issues only for confirmed bugs. Naming: `feat:`, `fix:`, `enhance:`, `chore:`, `docs:`, `build:`.
- [ ] **🔵M `/prerelease` bot workflow** — Comment `/prerelease` on PR → GitHub Actions builds all targets, publishes to Releases as `vX.Y.Z-prerelease-N`. Testers flash without compiling.
- [ ] **🟢S Unit tests** — ESP-IDF [Unity framework](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/unit-tests.html). Targets: `qr_gen` boundary values, `url_decode` edge cases, NVS round-trip, `best_qr_scale` clipping.
- [ ] **🟢S CI build** — [GitHub Actions](https://github.com/features/actions) with ESP-IDF Docker. Build all three targets on push; fail on compiler warnings.
- [ ] **🟢S CONTRIBUTING.md** — Build instructions, code style (Allman braces), module boundaries, how to add a WebSocket command, how to add a sensor.
- [ ] **🟢S Architecture decision records (ADRs)** — Short markdown files in `docs/adr/` explaining why: LittleFS over SPIFFS, blocking scan over background scan, WebSocket over UDP, ESP-IDF over Arduino.
- [x] **🔵S Shared LittleFS mount** — Idempotent `vfs_mount_littlefs()` prevents double-mount 404s.

---

## Resources & Links

### Documentation

- [ESP-IDF 5.x Programming Guide](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32s3/)
- [ESP-IDF WiFi API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html)
- [ESP-IDF HTTP Server](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html)
- [ESP-IDF I2C Master Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2c.html)
- [PlatformIO Project Configuration](https://docs.platformio.org/page/projectconf.html)
- [PlatformIO ESP-IDF](https://docs.platformio.org/en/latest/frameworks/espidf.html)

### Components

- [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator) — MIT
- [olikraus/u8g2](https://github.com/olikraus/u8g2) — 2-clause BSD
- [mdvorak/esp-u8g2-hal](https://github.com/mdvorak/esp-u8g2-hal) — ESP32 HAL for u8g2
- [joltwallet/esp_littlefs](https://github.com/joltwallet/esp_littlefs) — LittleFS for ESP-IDF
- [espressif/mdns](https://components.espressif.com/components/espressif/mdns) — mDNS component

### Related Projects

- [Meshtastic](https://meshtastic.org/) — Offline mesh messaging (inspiration)
- [tzapu/WiFiManager](https://github.com/tzapu/WiFiManager) — Arduino WiFi provisioning (lessons learned)
- [espressif/esp-idf](https://github.com/espressif/esp-idf) — Official ESP-IDF repo

### Hardware

- [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/)
- [ESP32-S3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- [SSD1306 OLED Datasheet](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)

---

## License

MIT. See individual component licenses:

- [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator): MIT
- [ESP-IDF](https://github.com/espressif/esp-idf): Apache 2.0
- [u8g2](https://github.com/olikraus/u8g2): 2-clause BSD
