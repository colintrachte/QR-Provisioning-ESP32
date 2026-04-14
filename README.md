# robot-provisioning

WiFi provisioning firmware for the **Heltec WiFi LoRa 32 V3** (ESP32-S3).

Pure ESP-IDF 5.x — no Arduino, no Espressif provisioning app, no BLE.
Connect via browser on your phone or PC.

---

## How it works

1. On boot the device tries stored credentials from NVS.
2. If none exist (or connection fails) it starts a **SoftAP + DNS hijack + captive portal**.
3. Your phone sees the AP. The OLED shows two QR codes:
   - **WIFI QR** — `WIFI:S:RobotSetup;T:WPA;P:robot1234;;` — scan with camera to auto-join.
   - **URL QR** — `http://192.168.4.1` — scan after joining if the captive redirect didn't fire.
4. The portal lists nearby networks. Pick yours, enter your password, submit.
5. Credentials saved to NVS. Device reboots and connects as STA, shows IP on OLED.
6. On future boots the device connects straight to your network — no portal.

Once connected, the OLED shows the IP address. If no browser is open on `/`, a **QR code for the robot control page** is shown alongside the IP so you can navigate there instantly. The QR disappears and is replaced by a plain IP display once a WebSocket client connects.

## PROJECT VISION

Inspired by Meshtastic — but built for robotics, not messaging.
Goal: a self-contained ESP-IDF foundation that handles the hard
infrastructure (WiFi provisioning, OTA, captive portal, health
monitoring, display, WebSocket control) so that new use cases
can be bootstrapped quickly by swapping a single frontend template
and implementing a thin application task.

No cloud required. No subscriptions. Optional cloud integrations
are fine as an add-on.

Target use cases (all share the same core):
  - Realtime RC control (tank, rover, arm)
  - Device-to-device control (direct or through a gateway)
  - Sequenced command playback (pre-arranged move lists)
  - Autonomous with periodic status push (e.g. chicken coop door → Home Assistant)
  - Smart home platform integration (Home Assistant, Google, Amazon)
  - Custom Unity / game-engine app as frontend

## ARCHITECTURE NOTES (decisions made, rationale preserved)

File system:    LittleFS only. SPIFFS is deprecated.
Framework:      ESP-IDF 5.x + PlatformIO as developer shell only.
                PlatformIO library manager is NOT used for components.
Libraries:      Vendored as git submodules in components/ except
                u8g2_hal which is downloaded manually due to S3 patches.
Config split:   sdkconfig.defaults → silicon/driver settings.
                platformio.ini → build environment metadata.
                config.h → application constants (baked in at compile time).
                LittleFS JSON → runtime user settings (survives OTA).
Code style:     Allman braces, concise comments, @param on every function.
UI philosophy:  All actions independent of display/web UI.
                Motor runs if OLED dead. Motor runs if browser disconnected.
                UI is telemetry + convenience, not a dependency.

### Re-provisioning

Hold the **USER button (GPIO0)** for 3 seconds at boot → credentials erased → portal starts.

---

## Hardware

| Board   | Heltec WiFi LoRa 32 V3 (HTIT-WB32LA) |
| ------- | ------------------------------------- |
| MCU     | ESP32-S3FN8 @ 240 MHz dual-core LX7  |
| Display | 0.96" SSD1306 OLED 128×64 I2C        |
| Flash   | 8 MB                                  |
| PSRAM   | None (V3) / 2 MB (V4)                 |

### Pin assignments

| Function    | GPIO |
| ----------- | ---- |
| OLED SDA    | 17   |
| OLED SCL    | 18   |
| OLED RST    | 21   |
| Vext power  | 36   |
| User button | 0    |

---

## Setup

### 1. Prerequisites

- [VS Code](https://code.visualstudio.com/) + [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- ESP-IDF 5.x (PlatformIO downloads this automatically)
- Git

### 2. Clone and get components

```bash
git clone <this-repo> robot-provisioning
cd robot-provisioning

# QR code generator (C port, optimised for embedded)
git submodule add https://github.com/nayuki/QR-Code-generator.git components/QR-Code-generator

# u8g2 main library
git submodule add https://github.com/olikraus/u8g2.git components/u8g2

# u8g2 HAL — download manually, do NOT add as submodule.
# Needs edits to work with ESP32-S3 (delete idf_component.yml, patch I2C host names).
# Source: https://github.com/mdvorak/esp-u8g2-hal -> components/u8g2_hal

# LittleFS
git submodule add https://github.com/joltwallet/esp_littlefs.git components/esp_littlefs
cd components/esp_littlefs && git submodule update --init --recursive && cd ../..
```

### 3. Configure

Edit `main/config.h`:

```c
#define AP_SSID     "RobotSetup"   // Name of the device's own setup AP
#define AP_PASSWORD "robot1234"    // Min 8 chars. "" for open (not recommended)
```

All other tunables (pin assignments, timing constants, debug flags) are also in `config.h`.

### 4. Build and flash

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

---

## Provisioning walkthrough

### First boot

```
OLED shows:
  WiFi Manager
  Starting...

  → Tries NVS — empty on first boot
  → AP starts → WIFI QR + "Scan to join: RobotSetup / robot1234"
```

### On your phone

1. Open camera, point at OLED QR — camera offers **"Join RobotSetup"**, tap it.
2. Browser opens to the setup page automatically (captive portal redirect).
   - If not: scan the second QR (`http://192.168.4.1`) manually.
3. Select your home WiFi from the list.
4. Enter password → **Save & Connect**.
5. OLED shows "Connecting: <SSID>", then the robot control page QR + IP.

**Skip link:** The setup page has a "Test robot now →" link that jumps straight to the robot control page without completing WiFi setup. Useful when the device is already reachable on the network or during local development.

### Subsequent boots

Device connects directly. OLED shows the IP (and QR if no browser is connected).

---

## Robot control page (`index.html`)

Served by the app server on port 80 after STA connects.

### Controls

| Input | How |
| ----- | --- |
| Touch joystick | Drag the puck — works in any browser |
| WASD keyboard | Hold keys; axes ramp 0→1 over 180 ms to avoid speed snapping |
| Arm / Disarm button | Must be armed before any movement command is sent |
| PC ↔ Touch toggle | Switches joystick visibility; keyboard always active |

### Telemetry received from firmware (WebSocket JSON)

| Field | Type | Description |
| ----- | ---- | ----------- |
| `left.speed` | int | Left track PWM value (0–1023) |
| `left.state` | string | `STOPPED` / `RAMP_UP` / `RUNNING` |
| `left.forward` | bool | Direction |
| `right.*` | — | Same as left |
| `rssi` | int | WiFi signal dBm |
| `battery` | int | Battery % |
| `temp` | float | Temperature °C |
| `uptime` | int | Seconds since boot |
| `heap` | int | Free heap bytes |
| `cpu` | int | CPU load % |
| `errors` | int | Cumulative error count |

### Commands sent to firmware (WebSocket text)

| Message | Meaning |
| ------- | ------- |
| `x:F,y:F` | Drive axes, −1.0 to 1.0, sent at 20 Hz |
| `stop` | Halt — sent when disarmed or both axes are zero |
| `led:F` | LED brightness 0.00–1.00, sent on change only |
| `ping` | Latency probe; firmware replies `pong` |

---

## File map

| File | Purpose |
| ---- | ------- |
| `main/config.h` | All compile-time constants — pins, timing, debug flags, AP credentials |
| `main/main.c` | Boot sequence, main loop, boot-loop guard, safe mode |
| `main/wifi_manager.c/.h` | SoftAP portal + STA provisioning + app HTTP/WebSocket server |
| `main/prov_ui.c/.h` | OLED state machine; driven by wifi_manager callbacks |
| `main/display.c/.h` | u8g2 wrapper — mutex-guarded draw calls, dirty-flag flush |
| `main/qr_gen.c/.h` | QR encode helpers (WiFi URI, URL) |
| `main/health_monitor.c/.h` | I2C scan at boot, periodic RSSI checks |
| `data/index.html` | Robot control page |
| `data/style.css` | Robot control styles |
| `data/script.js` | Robot control logic (joystick, keyboard, WebSocket, telemetry) |
| `data/setup.html` | WiFi provisioning portal page |
| `data/setup.css` | Portal styles |
| `data/setup.js` | Portal logic (scan, submit, skip link) |

---

## Display architecture

The SSD1306 holds its framebuffer in hardware GDDRAM — pixels stay visible with zero CPU involvement once written.

- All `display_draw_*` calls write to a **1 kB RAM shadow buffer** only — no I2C traffic.
- `display_flush()` pushes the buffer to hardware only when the dirty flag is set.
- Status bar updates (`render_status_bar`) clear and redraw only the bottom 10 px — one 128-byte I2C transaction, leaving the QR code untouched in hardware.
- A FreeRTOS mutex protects the buffer against concurrent task access.
- If the OLED is absent or disconnected, `health_monitor` detects this via I2C probe at boot and sets `display_set_available(false)`. All draw calls become safe no-ops.

---

## Extending for robot application

After provisioning, `wifi_manager_is_connected()` becomes true and `app_task` is spawned in `main.c`. Add your robot logic there:

```c
static void app_task(void *arg)
{
    while (1)
    {
        motor_controller_tick();
        sensor_read_and_publish();   // push JSON telemetry via WebSocket
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

The WebSocket dispatch in `wifi_manager.c` has `TODO` stubs for `motor_drive(x, y)`, `motor_stop()`, and `motor_set_led(val)` — replace these with your motor driver calls.

The display is yours after the success screen. Call `display_draw_*` + `display_flush()` freely; `prov_ui` will not touch it again.

---

## Dev notes

### Why components instead of PlatformIO library manager

1. **S3 compatibility** — many registry versions of `u8g2_hal` have `idf_component.yml` manifests that block the ESP32-S3. Vendoring lets you delete them and patch I2C host names directly.
2. **CMake integration** — ESP-IDF components link correctly with internal drivers (`driver`, `freertos`). The PIO library manager can't reliably handle this.
3. **Version lock** — external authors can't push a breaking update. You own the source.
4. **No duplicate symbol errors** — PIO's `.pio/libdeps` and `components/` defining the same symbols causes "Multiple Definition" linker errors. Using `components/` exclusively avoids this.

### sdkconfig / platformio.ini division of labour

- `sdkconfig.defaults` — framework, silicon, driver settings (memory, PSRAM, clock, partition offsets). Enable `CONFIG_HTTPD_WS_SUPPORT` here.
- `platformio.ini` — project metadata, serial speed, environment names.
- `config.h` — application-level constants baked in at compile time.

```
git config --global --add safe.directory C:/Users/Colin/Desktop/QR-Provisioning-ESP32
```

---

## Troubleshooting

| Symptom | Likely cause |
| ------- | ------------ |
| Web files 404 | Run `pio run -t uploadfs` |
| WebSocket not connecting | `CONFIG_HTTPD_WS_SUPPORT` not set in sdkconfig |
| OLED blank | Check Vext GPIO, I2C address, ribbon cable |
| QR not scannable | Scale too small — increase `QR_PANEL_W` or reduce SSID/password length |
| Boot loops | Hold USER button 3 s to erase credentials |

---

## License

MIT. See individual component licenses:

- nayuki/QR-Code-generator: MIT
- ESP-IDF components: Apache 2.0
- u8g2: 2-clause BSD
