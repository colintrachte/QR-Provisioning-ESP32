# ESP8266 D1 Mini Port — Full Feature Stack

## Files

| File | Description |
|------|-------------|
| `main_d1mini.cpp` | Application entry point, WebSocket, motor control, LED patterns, WiFi callbacks |
| `settings_mgr.h/.cpp` | Runtime settings singleton (LittleFS + ArduinoJson, wire-compatible with S3) |
| `settings_server.h/.cpp` | HTTP /api/settings endpoints (GET, POST partial update, POST reset) |
| `udp_ctrl.h/.cpp` | UDP control socket on port 4210 (same packet format as S3) |
| `health_monitor.h/.cpp` | Telemetry JSON builder (double-buffered, wire-compatible) |
| `i_battery.h/.cpp` | Battery voltage via A0 + voltage divider |
| `wifi_manager.h/.cpp` | Non-blocking STA/AP state machine polled from loop() |
| `shared/drive_mixer.h` | Unchanged arcade-drive mixer from S3 project |

## PlatformIO Configuration

Add to your `platformio.ini`:

```ini
[env:d1_mini]
platform = espressif8266
board = d1_mini
framework = arduino
monitor_speed = 74880
board_build.filesystem = littlefs
lib_deps =
    me-no-dev/ESPAsyncTCP@^1.2.2
    me-no-dev/ESPAsyncWebServer@^1.2.3
    bblanchon/ArduinoJson@^6.21.0
    ; Optional for OLED:
    ; olikraus/U8g2@^2.34.0
build_flags =
    -D BATTERY_R_TOP=100000
    -D BATTERY_R_BOT=100000
    -D BATTERY_V_EMPTY_MV=3200
    -D BATTERY_V_FULL_MV=4200
    ; -D HAS_OLED
```

## Web UI Compatibility

The `/api/settings` JSON schema is identical to the ESP32-S3 version:
- `GET` returns all fields with `ap_password=""` and `ota_token_set` boolean
- `POST` accepts partial updates (only present keys are applied)
- `POST` response includes `reboot_required: true` when AP/mDNS fields change

The WebSocket text protocol is unchanged:
- `ping` → `pong`
- `arm`, `disarm`, `stop`
- `led:0.5`
- `x:0.5,y:-0.3` (or use binary frames from the JS)

The UDP packet format is byte-identical to the S3:
```
[0] = 0xA5 (magic)
[1] = seq
[2:3] = x_fp (int16 LE)
[4:5] = y_fp (int16 LE)
[6] = flags (ARMED=0x01, ESTOP=0x02)
[7] = XOR checksum of [0:6]
```

## Resource Usage (estimated)

| Component | RAM |
|-----------|-----|
| Arduino core + WiFi | ~35 KB |
| AsyncWebServer + WS | ~12 KB |
| Settings (StaticJsonDocument) | ~1 KB reusable |
| Health telemetry (double buffer) | ~512 B |
| UDP socket | ~200 B |
| Motor state | ~100 B |
| **Total free heap after boot** | **~35-40 KB** |

## Optional OLED

Uncomment `#define HAS_OLED` in `main_d1mini.cpp` and add the U8g2 library.
**Warning:** Default D1 Mini I2C pins (D1/D2) conflict with the motor pinout.
Either move motor pins to D0/D4/D7/D8 or use a GPIO expander / different I2C pins.

## Differences from ESP32-S3

| Feature | S3 | D1 Mini |
|---------|-----|---------|
| Settings storage | NVS blob | LittleFS `/settings.json` |
| JSON library | cJSON | ArduinoJson v6 |
| WiFi manager | FreeRTOS task + event groups | Cooperative state machine in `loop()` |
| UDP control | Dedicated task on Core 1 | Polled from `loop()` |
| Internal temp sensor | Yes (die temp) | No (returns JSON `null`) |
| mDNS | `mdns.h` (auto) | `ESP8266mDNS` + `MDNS.update()` in loop |
| DHCP Option 114 | Yes (captive portal URI) | No (not exposed by Arduino core) |
| OTA rollback | Yes (`esp_ota_ops`) | No (ESP8266 bootloader limitation) |
| Second core | Yes (APP_CPU) | No (single core) |

## Migration Path

If you have an existing D1 Mini deployment with `/wifi.json` credentials:
1. The first boot with this firmware will find no `/settings.json` and use defaults
2. The AP SSID/password from `config.h` defaults will be used
3. After connecting via portal, credentials are saved to `/settings.json`
4. The old `/wifi.json` is ignored; you can delete it after migration
