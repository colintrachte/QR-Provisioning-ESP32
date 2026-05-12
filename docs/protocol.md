# Control & OTA Protocols

## WebSocket Control (Port 80)

### Client → Firmware

| Message | Format | Meaning |
|---------|--------|---------|
| Axes | `x:F,y:F` | Drive axes −1.0 to +1.0, sent at ~20 Hz |
| Stop | `stop` | Halt immediately |
| Arm | `arm` | Enable motor output |
| Disarm | `disarm` | Disable motor output |
| LED | `led:F` | LED brightness 0.00–1.00 |
| Ping | `ping` | Latency probe; replies `pong` |

### Binary Axes Frame (5 bytes, little-endian)

```
[0]     uint8  type = 0x01
[1..2]  int16  x * 10000   (range -10000..10000 → -1.0..1.0)
[3..4]  int16  y * 10000
```

Sent as WebSocket BINARY frames for lower overhead than TEXT JSON at high frequency.

### Firmware → Client (JSON telemetry, 5 Hz)

```json
{
  "rssi": -65,
  "battery": 80,
  "battery_low": false,
  "temp": 23.4,
  "uptime": 123,
  "heap": 200000,
  "errors": 0
}
```

| Field | Type | Description |
|-------|------|-------------|
| `rssi` | int | WiFi signal dBm |
| `battery` | int | Battery percentage |
| `battery_low` | bool | True when battery ≤ `battery_warn_pct` |
| `temp` | float \| null | Temperature °C (null if no sensor) |
| `uptime` | int | Seconds since boot |
| `heap` | int | Free heap bytes |
| `errors` | int | Cumulative error count |

## UDP Control (Port 4210)

Low-latency alternative to WebSocket. Pinned to Core 1 (APP_CPU) so HTTP traffic on Core 0 cannot delay motor commands.

### Packet Format (8 bytes)

```
[0]  uint8  magic = 0xA5
[1]  uint8  sequence number (mod 256, window = 8)
[2]  int16  x * 10000   (little-endian)
[4]  int16  y * 10000   (little-endian)
[6]  uint8  flags: bit0 = armed, bit1 = e-stop
[7]  uint8  XOR checksum (bytes 0..6)
```

### Coexistence with WebSocket

Both transports call `ctrl_drive_set_axes()` and `ctrl_drive_feed_watchdog()`. Whichever delivers a frame last wins. The watchdog is fed by both paths so it will not fire as long as either transport is active.

### Out-of-order rejection

Sequence window of 8 prevents accepting stale/reordered packets. Wrap-around (255→0) handled correctly by modular arithmetic: `(0 - 255) & 0xFF = 1`, which is < window → accepted.

## OTA Protocol

### Endpoints

| Endpoint | Method | Body | Effect |
|----------|--------|------|--------|
| `/ota/firmware` | POST | raw `.bin` | Flash new firmware, reboot |
| `/ota/filesystem` | POST | raw LittleFS `.bin` | Reflash web UI, remount — no reboot |

### Authentication (optional)

Set in `platformio.ini`:

```ini
build_flags = -DOTA_AUTH_TOKEN=\"your-secret-token\"
```

Include header in request:

```bash
curl -X POST http://192.168.1.42/ota/firmware \
  -H "X-OTA-Token: your-secret-token" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @firmware.bin
```

Comparison is constant-time (byte-by-byte XOR accumulator) to prevent timing oracle attacks.

### Checksum Verification (SHA-256)

Both endpoints optionally verify a SHA-256 digest streamed during upload. The header is **optional** — existing scripts without it continue to work.

```bash
DIGEST=$(sha256sum firmware.bin | awk '{print $1}')
curl -X POST http://<device-ip>/ota/firmware \
  -H "X-OTA-SHA256: $DIGEST" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @firmware.bin
```

**Verification sequence:**

```
for each 4 kB chunk:
    sha256_update(chunk)     ← runs alongside flash write
    esp_ota_write(chunk)

sha256_finish() → computed digest
compare(computed, X-OTA-SHA256 header)  ← constant-time
    mismatch → esp_ota_abort(), return 400
    match    → esp_ota_end(), set_boot_partition(), reboot
```

**Failure behavior:** Returns `400 Bad Request` with body `SHA-256 checksum mismatch — upload corrupted`. Currently running firmware is unchanged. Device does not reboot.

### Rollback (ESP-IDF)

After flashing, the bootloader boots the new image in a *pending verify* state. `ota_server_mark_valid()` cancels rollback once the HTTP server is up. If the firmware panics before that, the next boot restores the previous image.

Enable in `sdkconfig.defaults`:

```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

### Flashing Commands

```bash
# Firmware (no checksum)
curl -X POST http://<device-ip>/ota/firmware \
  -H "Content-Type: application/octet-stream" \
  --data-binary @.pio/build/robot_esp32/firmware.bin

# Firmware (with checksum — recommended)
DIGEST=$(sha256sum .pio/build/robot_esp32/firmware.bin | awk '{print $1}')
curl -X POST http://<device-ip>/ota/firmware \
  -H "X-OTA-SHA256: $DIGEST" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @.pio/build/robot_esp32/firmware.bin

# Web UI (no reboot)
curl -X POST http://<device-ip>/ota/filesystem \
  -H "Content-Type: application/octet-stream" \
  --data-binary @.pio/build/robot_esp32/littlefs.bin
```

## Settings API

### Routes

| Route | Method | Description |
|-------|--------|-------------|
| `/api/settings` | GET | Current settings as JSON (passwords masked) |
| `/api/settings` | POST | Partial update — only changed fields needed |
| `/api/settings/reset` | POST | Reset to factory defaults |

### Partial Update Semantics

The POST handler starts from current settings and overlays only keys present in the request body. Fields left out are unchanged.

```json
// Send only what you want to change
{ "drive_deadband": 0.08 }
```

### Reboot-Required Fields

Changes to `ap_ssid`, `ap_password`, `ap_channel`, `mdns_enable`, `mdns_hostname` require a reboot. The POST response includes `"reboot_required": true` when any of these were modified.

### Password Semantics

- `ap_password`: empty string in GET response. Send non-empty value to update. Send `"ap_password_clear": true` to set open AP.
- `ota_token`: boolean `ota_token_set` in GET response. Send non-empty value to update. Empty/absent field keeps existing token.

## JS Error Reporting

Frontend exceptions are POSTed to `/api/jserror` and forwarded to the serial log:

```javascript
window.onerror = (msg, url, line, col, err) => {
    fetch('/api/jserror', {
        method: 'POST',
        body: JSON.stringify({ msg, url, line, col, stack: err?.stack })
    });
};
```
