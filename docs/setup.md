# Setup & Flashing

## Prerequisites

- [Python](https://www.python.org/downloads/)
- [VS Code](https://code.visualstudio.com/) + [PlatformIO IDE](https://platformio.org/install/ide?install=vscode)
- [Git](https://git-scm.com/)
- ESP-IDF 5.x (PlatformIO downloads this automatically)

## Clone & Components

```bash
git clone <this-repo> robot-provisioning
cd robot-provisioning

# QR code generator (C port, optimised for embedded)
git submodule add https://github.com/nayuki/QR-Code-generator.git \
  components/QR-Code-generator

# u8g2 display library
git submodule add https://github.com/olikraus/u8g2.git components/u8g2

# u8g2 ESP32 HAL — patched for ESP32-S3 I2C master API
git submodule add https://github.com/mdvorak/esp-u8g2-hal.git components/u8g2_hal

# Managed via idf_component.yml:
#   - joltwallet/littlefs
#   - espressif/mdns
#   - espressif/cJson
```

## Configure

Edit `main/config.h`:

```c
#define AP_SSID     "RobotSetup"   // Device's own setup AP name
#define AP_PASSWORD "robot1234"    // Min 8 chars. "" for open (not recommended)
```

For runtime settings (no recompile needed), use the web UI Settings page or POST `/api/settings`.

## Build, Flash, Monitor

```bash
# Build & flash firmware
pio run -e robot_esp32 -t upload

# Flash web UI to LittleFS
pio run -e robot_esp32 -t uploadfs

# Serial monitor
pio device monitor -e robot_esp32
```

Or use VS Code shortcuts: **Ctrl+Alt+B** (Build), **Ctrl+Alt+U** (Upload), **Ctrl+Alt+S** (Monitor).

## First-Time Build Note

`platformio.ini` embeds gzipped web assets into the firmware binary. On a fresh clone, `compress_assets.py` runs automatically before the first build to generate `.gz` files. If you see linker errors about missing `_binary_*_start` symbols, run:

```bash
python compress_assets.py
pio run -e robot_esp32
```

## Partition Layouts

**Heltec V3 — 8 MB:**

| Partition | Size | Purpose |
|-----------|------|---------|
| nvs | 20 kB | Credentials + settings |
| otadata | 8 kB | Active OTA slot selector |
| ota_0 | 3.19 MB | Firmware slot A |
| ota_1 | 3.19 MB | Firmware slot B |
| storage | 1.56 MB | LittleFS web UI |

**TTGO V1 — 4 MB:**

| Partition | Size | Purpose |
|-----------|------|---------|
| nvs | 20 kB | Credentials + settings |
| otadata | 8 kB | Active OTA slot selector |
| ota_0 | 1.75 MB | Firmware slot A |
| ota_1 | 1.75 MB | Firmware slot B |
| storage | 448 kB | LittleFS web UI |

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Web files 404 | LittleFS not mounted or mismatched file references | `pio run -t uploadfs`; check `data/` contents |
| Build fails with missing `_binary_*` symbols | `.gz` files not generated | Run `python compress_assets.py` manually |
| Segfault during build | File Explorer locking managed components | Close file browsers open to project directory |
| WebSocket not connecting | `CONFIG_HTTPD_WS_SUPPORT` missing | Add to `sdkconfig.defaults` |
| OLED blank | Vext GPIO (36), I2C address, cable | Check hardware; verify `i_sensors_init()` log |
| QR not scannable | SSID/password too long | Reduce length or lower ECC in `qr_gen.c` |
| Motors don't move | Wrong `MOTOR_DRIVER_MODE` / not armed | Check `config.h`; arm in UI |
| Boot loops | Corrupt NVS or WiFi init fail | Hold GPIO0 3s to erase credentials |
| I2C errors after WiFi | `i_sensors_init()` order wrong | Must run **after** `wifi_manager_start()` |
| OTA 500 | Image too large for partition | Check partition sizes vs `.bin` |
| OTA 401 | Missing `X-OTA-Token` | Check `OTA_AUTH_TOKEN` in build flags |
| No rollback | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` missing | Add to `sdkconfig.defaults` |
| RGB display driver issue | Replaced whole files via file explorer | Full clean, rebuild intellisense index, rebuild |
