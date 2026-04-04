# robot-provisioning

WiFi provisioning firmware for the **Heltec WiFi LoRa 32 V3** (ESP32-S3).

Pure ESP-IDF 5.x — no Arduino, no Espressif provisioning app, no BLE.
Connect via browser on your phone. No typing if your camera app supports
Wi-Fi QR codes.

---

## How it works

1. On boot the device tries stored credentials from NVS.
2. If none exist (or connection fails) it starts a **SoftAP + DNS + captive portal**.
3. Your phone sees the AP. Two QR codes cycle on the OLED every 5 seconds:
   - **WIFI QR** — `WIFI:S:RobotSetup;T:WPA;P:robot1234;;`
     Scan with your camera → phone offers to join the AP automatically.
   - **URL QR** — `http://192.168.4.1`
     Scan after you're on the AP → opens the setup page immediately if the
     captive redirect didn't fire.
4. The portal page **scans nearby networks** and lists them. Pick yours, enter
   your password, submit.
5. Credentials are saved to NVS. Device connects as STA, shows IP on OLED.
6. On future boots the device connects straight to your network — no portal.

### Re-provisioning

Hold the **USER button (GPIO0)** for 3 seconds at boot → credentials erased →
portal starts.

---

## Hardware

| Board       | Heltec WiFi LoRa 32 V3 (HTIT-WB32LA)  |
|-------------|----------------------------------------|
| MCU         | ESP32-S3FN8 @ 240 MHz dual-core LX7   |
| Display     | 0.96" SSD1306 OLED 128×64 I2C         |
| Flash       | 8 MB                                  |
| PSRAM       | None (V3) / 2MB (V4)                  |

### Pin assignments used

| Function     | GPIO |
|--------------|------|
| OLED SDA     | 17   |
| OLED SCL     | 18   |
| OLED RST     | 21   |
| Vext power   | 36   |
| User button  | 0    |

---

## Project structure

```
robot-provisioning/
├── platformio.ini          PlatformIO project (ESP-IDF framework)
├── CMakeLists.txt          Root cmake
├── sdkconfig.defaults      ESP32-S3 tuned SDK settings
├── components/
│   └── qr_code_generator/  Wraps nayuki/QR-Code-generator C library
│       ├── CMakeLists.txt
│       ├── nayuki/         ← git clone goes here (see Setup)
│       └── stub/           Compile stub used before submodule is cloned
└── main/
    ├── CMakeLists.txt
    ├── main.c              app_main — entry point, main loop
    ├── display.h / .c      SSD1306 driver: shadow buffer + dirty-page flush
    ├── qr_gen.h / .c       QR code generation (WIFI URI + URL)
    ├── wifi_prov.h / .c    SoftAP + DNS + HTTP captive portal
    └── prov_ui.h / .c      Display state machine, dual-QR cycling
```

---

## Setup

### 1. Prerequisites

- [VS Code](https://code.visualstudio.com/) + [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- ESP-IDF 5.x (PlatformIO will download this automatically)
- Git

### 2. Clone and get submodules

```bash
git clone <this-repo> robot-provisioning
cd robot-provisioning

# nayuki QR code generator
cd components/qr_code_generator
git clone https://github.com/nayuki/QR-Code-generator nayuki
cd ../..

# u8g2 display library + ESP-IDF HAL
cd components/u8g2_hal
git clone https://github.com/olikraus/u8g2.git u8g2
git clone https://github.com/mkfrey/u8g2-hal-esp-idf.git hal
cd ../..
```

### 3. Configure AP credentials

Edit `main/main.c`:

```c
#define AP_SSID     "RobotSetup"   // Name of the ESP32's setup network
#define AP_PASSWORD "robot1234"    // Min 8 chars. Set "" for open (not recommended)
```

These are the credentials for the **device's own AP** — not your home network.
Your home network credentials are entered via the browser during provisioning.

### 4. Build and flash

Open the project folder in VS Code with PlatformIO installed, then:

```
PlatformIO: Build    (Ctrl+Alt+B)
PlatformIO: Upload   (Ctrl+Alt+U)
PlatformIO: Monitor  (Ctrl+Alt+S)
```

Or from the terminal:

```bash
pio run -t upload -t monitor
```

---

## Provisioning walkthrough

### First boot

```
Display shows:
  ┌─────────────────┐
  │   RobotOS       │
  │─────────────────│
  │ WiFi Provision  │
  │   Starting...   │
  │─────────────────│
  │ Heltec V3 S3   │
  └─────────────────┘
  
  → "Connecting..." (tries NVS — empty on first boot)
  → AP starts → WIFI QR appears
```

### On your phone

1. Open camera app, point at the OLED QR code
2. Camera offers **"Join RobotSetup"** — tap it
3. Browser opens automatically with the setup page
   - If not: wait for the second QR (URL type) to appear, scan it
4. Select your home WiFi from the dropdown
5. Enter password → tap Connect
6. OLED shows connecting animation, then success screen with IP address

### Subsequent boots

Device connects directly. No portal, no QR codes. The success screen shows
the IP address.

---

## Display architecture

The SSD1306 has its own GDDRAM. Once pixels are written they stay up with
**zero CPU involvement**. The firmware exploits this:

- All `display_draw_*` calls write to a **1024-byte RAM shadow buffer** only.
  No I2C traffic.
- `display_flush()` pushes only **dirty pages** (8-bit bitmask tracks which
  of the 8 horizontal pages changed). A single-line status update at the
  bottom costs **one 128-byte I2C transaction** (~3ms at 400kHz) and leaves
  the rest of the screen — including any QR code — completely untouched in
  hardware.
- `display_flush_page(n)` for surgical single-page updates.
- No refresh timer, no display task. The hardware holds the image.

### QR cycling

When the portal is active and no phone is connected, `prov_ui_tick()` (called
at 10Hz from the main loop) switches between the two QR codes every 5 seconds.
Only pages 0–6 are redrawn on a switch; the status bar (page 7) is left alone.
Cycling stops the moment a phone connects.

---

## Extending for robot application

After provisioning completes the `wifi_prov_is_connected()` check in the main
loop becomes true. Add your robot code there:

```c
if (wifi_prov_is_connected()) {
    robot_app_tick();   // your code here
}
```

The display is yours to use at any point — just call `display_draw_*` and
`display_flush()`. The provisioning UI will not touch it again after the
success screen.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Blank display | Vext not powered | Check GPIO36 init; ensure `display_init()` called before any draw |
| Garbage on display | Reset not asserted | GPIO21 RST pulse required; `display_init()` handles this |
| QR not scannable | Too small / no quiet zone | Ensure scale≥2; quiet zone is rendered automatically |
| Captive portal doesn't pop up | OS-dependent behaviour | Use the URL QR (second QR) to open manually |
| `idf_component_register` error | nayuki submodule missing | `cd components/qr_code_generator && git clone ... nayuki` |
| Can't connect after password entry | Wrong password / SSID | Check router; hold USER button 3s to re-provision |
| I2C timeout in display_init | Pull-ups / wiring | Internal pull-ups enabled in driver; check for short on SDA/SCL |

---

## License

MIT. See individual component licenses:
- nayuki/QR-Code-generator: MIT
- ESP-IDF components: Apache 2.0
