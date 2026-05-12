# Firmware Architecture

## Module Map

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

  ── Settings ────────────────────────────────────────────────────────────────
  settings_mgr.c/.h   NVS-backed JSON configuration store
  settings_server.c/.h  HTTP endpoint layer for runtime settings

  ── Utilities ───────────────────────────────────────────────────────────────
  utils_filesystem.c/.h  LittleFS mount, file inventory
  utils_json.c/.h        WiFi scan JSON builders
  utils_web.c/.h         HTTP response helpers, URL decode
```

## Boot Sequence

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
udp_ctrl_start()        → UDP control socket (once STA connects)
```

## I2C Timing Constraint

[`esp_wifi_start()`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html#_CPPv415esp_wifi_startv) resets the I2C peripheral on ESP32-S3 as a side-effect of radio clock tree bringup. `display_init()` runs before WiFi. After `wifi_manager_start()`, `i_sensors_init()` calls `display_reinit_i2c()` to recreate bus/device handles via the new [`driver/i2c_master.h`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2c.html) API. No delay hacks — handle lifetimes are managed explicitly.

## Motor Driver Modes

Set `MOTOR_DRIVER_MODE` in `config.h`:

| Mode | Description |
|------|-------------|
| `MOTOR_MODE_DIR_PWM_EN` | DIR + PWM + EN per channel. One LEDC channel per motor. |
| `MOTOR_MODE_BTN8982` | [BTN8982](https://www.infineon.com/cms/en/product/power/motor-control-ics/intelligent-motor-control-ics/multi-half-bridge-drivers/btn8982ta/) H-bridge. IN1/IN2 PWM per side. Four LEDC channels. |

## Thread Safety Patterns

### Fixed-point atomic storage (ctrl_drive.c)

`int16_t` writes are single-instruction atomic on Xtensa LX7. `ctrl_drive` uses fixed-point (`-10000..10000` maps to `-1.0..1.0`) for `s_target_x_fp` / `s_target_y_fp` to avoid the torn-read hazard present with `volatile float` (FPU store is not guaranteed single-instruction across a task switch).

### Double-buffered JSON (health_monitor.c)

`health_monitor_tick()` writes into the inactive buffer, then flips `s_json_active` in a single volatile int store (atomic on ESP32-S3). `app_server_push_telemetry()` always reads from `s_json[s_json_active]`. Worst case: one telemetry period stale (acceptable for 5 Hz).

### Mutex hierarchy

- `display.c`: `s_mutex` guards all draw calls + buffer flush
- `app_server.c`: `s_ws_mutex` guards WebSocket client list; `s_scan_mutex` guards scan results
- `settings_mgr.c`: `s_mutex` guards NVS read/write

No deadlock risk: no module holds more than one mutex at a time.
