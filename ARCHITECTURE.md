# Firmware Architecture

## Module Map

```
main/
  main.c              Boot sequence only. No logic beyond init ordering and the main loop.
  config.h            All compile-time constants: pins, timing, thresholds, debug flags.

  ── WiFi / Network ──────────────────────────────────────────────────────────
  wifi_manager.c/.h   STA/AP state machine + NVS credential storage. No HTTP, no display.
  portal.c/.h         SoftAP captive portal: DNS hijack task, OS probe handlers,
                      network scan endpoint, credential POST handler. Owns one httpd.
  app_server.c/.h     Robot control HTTP file server + WebSocket dispatch (port 80).
                      Calls into ctrl_drive and o_led. Owns a second httpd.

  ── Display ─────────────────────────────────────────────────────────────────
  display.c/.h        u8g2 wrapper: mutex-guarded draw calls, dirty-flag flush.
                      Graceful no-op when OLED absent.
  prov_ui.c/.h        OLED state machine driven by wifi_manager callbacks.
  qr_gen.c/.h         QR encode helpers (WiFi URI, URL).

  ── Inputs (i_) ─────────────────────────────────────────────────────────────
  i_battery.c/.h      ADC read of VBAT on GPIO1. Returns voltage and percent.
  i_sensors.c/.h      I2C sensor bus owner. Re-inits I2C after WiFi startup.
                      Provides a tick that polls registered sensor drivers.
                      Exposes a snapshot struct read by health_monitor and app_server.

  ── Outputs (o_) ────────────────────────────────────────────────────────────
  o_led.c/.h          Onboard white LED on GPIO35 via LEDC (PWM brightness 0–100%).
                      o_led_set(float brightness)  0.0–1.0, thread-safe.
                      o_led_blink(pattern)         non-blocking blink patterns.

  o_motors.c/.h       Two brushed DC motors. Abstracts both driver topologies:
                        MODE_DIR_PWM_EN  — DIR + PWM + EN per channel
                        MODE_BTN8982     — BTN8982 H-bridge (two PWM inputs per side)
                      Motor mode selected by MOTOR_DRIVER_MODE in config.h.
                      Exposes o_motors_drive(float left, float right) -1.0…+1.0.

  ── Controllers (ctrl_) ─────────────────────────────────────────────────────
  ctrl_drive.c/.h     Differential drive mixing. Converts joystick (x, y) to
                      per-track velocities and calls o_motors_drive().
                      Owns arming state; ignores drive commands when disarmed.

  ── Health ──────────────────────────────────────────────────────────────────
  health_monitor.c/.h Periodic RSSI check. Reads i_battery and i_sensors snapshots.
                      Builds the telemetry JSON blob pushed by app_server.
                      Does NOT own I2C — delegates to i_sensors.
```

## Boot Sequence

```
display_init()          Power OLED, init u8g2.
prov_ui_show_boot()     Immediate splash.
prov_ui_init()          Pre-generate QR codes (~50 ms).
reprovision_check()     Non-blocking GPIO0 poll.
wifi_manager_start()    Try NVS → fall back to portal (portal.c).
  [portal runs until credentials submitted]
i_sensors_init()        Re-init I2C bus (WiFi startup may reset it), scan, map peripherals.
o_led_init()            Configure LEDC on GPIO35.
o_motors_init()         Configure LEDC + GPIO for selected driver mode.
health_monitor_init()   Cache peripheral map, start RSSI polling.
app_server_start()      HTTP file server + WebSocket on port 80.
[main loop]             prov_ui_tick(), health_monitor_tick(), 10 Hz.
[on connect]            spawn app_task → ctrl_drive_tick(), sensor reads, telemetry push.
```

## I2C Timing Fix

WiFi init on ESP32-S3 resets peripherals including I2C_NUM_0.
`display_init()` runs before WiFi and owns the bus for OLED init.
After `wifi_manager_start()` returns, `i_sensors_init()` calls
`i2c_driver_delete(I2C_NUM_0)` then re-installs the driver.
display.c re-registers its HAL at the same time via `display_reinit_i2c()`.
The health_monitor I2C scan therefore runs on a freshly initialised bus
with no timing dependency on WiFi settling.

## Motor Driver Modes

Set MOTOR_DRIVER_MODE in config.h:

  MOTOR_MODE_DIR_PWM_EN   — Generic: DIR pin sets direction, PWM sets speed, EN enables.
                             One LEDC channel per motor.
  MOTOR_MODE_BTN8982      — BTN8982: two PWM inputs per half-bridge.
                             IN1/IN2 per channel from LEDC. EN always high.

Pin assignments for both modes live in config.h under "Motor pins".

## Naming Conventions

  i_*.c/.h    Input drivers: read sensors or ADC. No side effects on outputs.
  o_*.c/.h    Output drivers: actuate hardware. No sensor reads.
  ctrl_*.c/.h Controllers: combine inputs and outputs, own state machines.
  No prefix:  Infrastructure (wifi_manager, display, health_monitor, etc.)
