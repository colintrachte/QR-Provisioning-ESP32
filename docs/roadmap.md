# Roadmap

> **Value:** 🔴 High — prevents bricks, reduces support, or unlocks new use cases
> **Value:** 🔵 Medium — improves UX or expands capabilities
> **Value:** 🟢 Low — nice to have, polish, or future-proofing
> **Effort:** S = hours, M = days, L = weeks

---

## Safety & Reliability

_These prevent bricks and make field debugging possible. Do these first._

- [x] **🔴S OTA checksum verification** — SHA-256 streamed during upload, verified before `esp_ota_end()`. Prevents bricks from corrupted transfers.
- [x] **🔴S Task + heap watchdogs** — FreeRTOS task watchdog (5 s timeout, panic) and light heap corruption detection enabled in `sdkconfig.defaults`.
- [ ] **🔴S Embed frontend into firmware binary** — Gzip SPA at build time, convert to C byte array. Serve from ROM. One OTA updates everything; no more "forgot `uploadfs`" failures.
- [ ] **🔵M Persistent event logging** — Compressed JSONL ring buffer on flash (last 5 sessions, ~100 KB each). Browsable/downloadable from UI. Optional remote syslog. Debug field issues without a serial cable.
- [x] **🔵S Factory reset from UI** — Settings page button + GPIO0 hold → erase WiFi creds, reset to defaults, reboot into portal.
- [x] **🔵S Graceful WS shutdown before OTA reboot** — HTTP server stopped and WS close frames sent before `esp_restart()`. Browser shows "updating" instead of a hung connection.
- [x] **🔵S Battery low warning** — `battery_warn_pct` threshold surfaced in telemetry JSON (`battery_low` bool) and on the OLED status bar. Prevents deep-discharge damage in the field.
- [x] **🔴S Blocking WiFi scan** — Manual refresh only via `/api/rescan`. No background task = no missed-probe reboots.
- [x] **🔴S Command watchdog** — Auto-disarm on disconnect (400 ms timeout).
- [x] **🔴S Boot-loop guard** — Safe mode after `WIFI_MAX_RESTART_ATTEMPTS`.
- [x] **🔴S OTA firmware + filesystem** — Separate endpoints, rollback support via `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.
- [x] **🟢S Boot-loop diagnostics** — Log `esp_reset_reason()` to serial at boot and display the reset cause on the safe mode screen.
- [x] **🔵S I2C re-init after WiFi** — Handles ESP32-S3 peripheral reset via `display_reinit_i2c()`.

---

## User Experience

_These reduce friction for non-technical users and cut your support burden._

- [x] **🔴M Runtime user settings (JSON in NVS/LittleFS)** — Hostname, motor deadband, UI template, timezone, notification topic. Survives OTA. Stop recompiling `config.h` for every change.
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

## Frontend & Web UI

_These make the browser experience richer and more debuggable._

- [x] **🔵S JS error reporting** — `window.onerror` → POST `/api/jserror` so frontend bugs appear in the serial log.
- [x] **🔵S Latency display** — Show round-trip ping time in the control UI. Helps diagnose WiFi congestion.
- [x] **🟢S Connection quality indicator** — WiFi signal bars + packet loss estimate in the UI header.
- [x] **🔴M WebSocket control** — Joystick, keyboard, arming, telemetry push at 5 Hz.

---

## Communications

_These turn the robot from a LAN toy into an internet-aware device._

- [ ] **🔵M Home Assistant / MQTT** — Publish telemetry to configurable broker. Accept command topics (`robot/cmd/stop`, `robot/cmd/led`). Auto-discover via HA MQTT integration.
- [ ] **🔵S NTP time sync** — Sync RTC on boot and every 4 h. Enables timestamped logs, scheduled autonomous missions, accurate uptime reporting.
- [x] **🔵M WebSocket vs UDP evaluation** — Prototype UDP control channel. Measure latency + jitter under load vs current WebSocket. Decide before committing to UDP for low-latency use cases.
- [ ] **🟢L BLE provisioning path** — Alternative to SoftAP for phones that handle BLE pairing better than captive portals. ESP32-S3 has BLE 5.0. Keep SoftAP as fallback.
- [ ] **🟢L LoRa integration** — Join [Meshtastic](https://meshtastic.org/) network or custom point-to-point protocol. Must enforce regional duty-cycle limits (EU 1%, US ISM unrestricted). Requires separate radio HAL.
- [ ] **🟢L Device-to-device control** — One ESP32 as controller, another as actuator over WiFi (UDP/WS) or LoRa. Pairing via QR code or NFC bump.

---

## Hardware Abstraction

_These let the same firmware run on multiple boards without `#ifdef` spaghetti._

- [ ] **🔵Switch to json instead of .h files for boards HAL**
- [x] **🔵M Board abstraction layer** — GPIO pin tables per board in `components/boards/<name>.h`. Selected by build flag.
- [ ] **🔵M Peripheral auto-discovery** — `i_sensors` already scans I2C. Extend to probe known addresses (BME280 @ 0x76, AXP192 @ 0x34, etc.) and auto-populate `i_peripheral_map_t`. Settings page shows detected hardware.
- [ ] **🔵M TTGO V1 peripherals** — AXP192 PMIC driver (battery voltage, charge state, power management), NEO-6M GPS parser (UART2), onboard temperature sensor.
- [ ] **🟢M SPI display support** — ST7735 / ILI9341 for boards without I2C OLED. u8g2 already supports these; needs HAL config in board header.
- [ ] **🟢L E-paper display** — Low-power / solar use cases. Different refresh model (partial vs full updates). u8g2 has limited support; may need dedicated driver.
- [x] **🔵M Motor driver abstraction** — `MOTOR_MODE_DIR_PWM_EN` and `MOTOR_MODE_BTN8982`.

---

## Quality & Process

_These make the project maintainable as it grows and help others contribute._

- [x] **🔵S Deduplicate `serve_file()`** — Extract the identical gzip-transparent file serving logic from `portal.c` and `app_server.c` into a shared helper (`utils_web.c`).
- [ ] **🔵M GitHub Discussions + Issue conventions** — Require Discussions for questions/setup help. Issues only for confirmed bugs. Naming: `feat:`, `fix:`, `enhance:`, `chore:`, `docs:`, `build:`.
- [ ] **🔵M `/prerelease` bot workflow** — Comment `/prerelease` on PR → GitHub Actions builds all targets, publishes to Releases as `vX.Y.Z-prerelease-N`. Testers flash without compiling.
- [ ] **🟢S Unit tests** — ESP-IDF [Unity framework](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/unit-tests.html). Targets: `qr_gen` boundary values, `url_decode` edge cases, NVS round-trip, `best_qr_scale` clipping.
- [ ] **🟢S CI build** — [GitHub Actions](https://github.com/features/actions) with ESP-IDF Docker. Build all three targets on push; fail on compiler warnings.
- [ ] **🟢S CONTRIBUTING.md** — Build instructions, code style (Allman braces), module boundaries, how to add a WebSocket command, how to add a sensor.
- [ ] **🟢S Architecture decision records (ADRs)** — Short markdown files in `docs/adr/` explaining why: LittleFS over SPIFFS, blocking scan over background scan, WebSocket over UDP, ESP-IDF over Arduino.
- [x] **🔵S Shared LittleFS mount** — Idempotent `vfs_mount_littlefs()` prevents double-mount 404s.
