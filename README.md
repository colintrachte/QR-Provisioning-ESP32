# Robot Provisioning

Inspired by [Meshtastic](https://meshtastic.org/) — but built for robotics, not messaging. A self-contained ESP-IDF foundation handling the hard infrastructure so new use cases boot quickly by swapping a frontend template and implementing a thin application task.

**No cloud required. No subscriptions.** Optional cloud integrations are on the roadmap as add-ons.

WiFi provisioning and RC control firmware for [**Heltec WiFi LoRa 32 V3**](https://heltec.org/project/wifi-lora-32-v3/) (ESP32-S3). Also targeting TTGO LoRa32 V1 and Wemos D1 Mini (ESP8266/Arduino).

> **No Espressif provisioning app required**
> Connect via browser on any phone or PC.

[![PlatformIO](https://img.shields.io/badge/PlatformIO-6.9+-orange.svg)](https://platformio.org/)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.x-blue.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32s3/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

---

## 30-Second Pitch

1. **Boot** → tries stored WiFi credentials.
2. **No creds / fails** → starts **SoftAP + captive portal**.
3. **Phone joins AP** → OLED shows QR codes to auto-join and open the portal.
4. **Portal** → pick network, enter password, submit.
5. **Save & connect** → credentials saved to NVS. Future boots connect straight away.

Once connected, the OLED shows a QR for the robot control page. QR disappears once a WebSocket client connects — reconnects and the QR returns automatically.

### Re-provisioning

Hold **USER button (GPIO0)** for 3 seconds at boot → credentials erased → portal starts fresh.

---

## Quick Start

```bash
git clone <this-repo> robot-provisioning
cd robot-provisioning

# Build & flash firmware
pio run -e robot_esp32 -t upload

# Flash web UI to LittleFS
pio run -e robot_esp32 -t uploadfs

# Serial monitor
pio device monitor -e robot_esp32
```

Or use VS Code shortcuts: **Ctrl+Alt+B** (Build), **Ctrl+Alt+U** (Upload), **Ctrl+Alt+S** (Monitor).

→ **[Full setup guide](docs/setup.md)** — toolchain, first flash, troubleshooting.

---

## Documentation

| Document | What you'll find |
|----------|-----------------|
| [Setup & Flashing](docs/setup.md) | Toolchain, build flags, partition tables, troubleshooting matrix |
| [Architecture](docs/architecture.md) | Module map, boot sequence, I2C timing, motor driver modes |
| [Control Protocol](docs/protocol.md) | WebSocket/UDP frame formats, OTA protocol, checksum verification |
| [Hardware Reference](docs/hardware.md) | Pin tables per board, voltage dividers, wiring diagrams |
| [Roadmap](docs/roadmap.md) | Completed and upcoming features, contribution guidelines |

**API Reference:** Run `doxygen Doxyfile` locally, or view [online docs](https://your-github-pages-link-here) (see [Doxygen setup](#doxygen-setup) below).

---

## Target Use Cases

| Use Case | Notes |
|----------|-------|
| Realtime RC control | Tank, rover, arm — current default |
| Device-to-device control | Direct or through a gateway |
| Sequenced command playback | Pre-arranged move lists |
| Autonomous + status push | e.g. chicken coop door → [Home Assistant](https://www.home-assistant.io/) |
| Smart home integration | Home Assistant, Google, Amazon |
| Custom game-engine frontend | [Unity](https://unity.com/) / [Unreal](https://www.unrealengine.com/) app as UI |

---

## Doxygen Setup

Generate browsable API docs from source comments:

```bash
# Install Doxygen (Ubuntu/Debian)
sudo apt-get install doxygen graphviz

# macOS
brew install doxygen graphviz

# Generate docs
doxygen Doxyfile

# Open in browser
open docs/api/html/index.html        # macOS
xdg-open docs/api/html/index.html    # Linux
```

See [docs/doxygen-setup.md](docs/doxygen-setup.md) for full configuration details, CI integration, and comment style guide.

---

## License

MIT. See individual component licenses in [LICENSE](LICENSE).
