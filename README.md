# Robot Provisioning

This project is meant to be a template for other projects to build on top of. Its purpose is to guide users through the wifi provisioning process with QR codes, and then provide a starter operating system for settings, updates, and control. Copious documentation and comments are available, so you can easily fix bugs or tweak the project with AI.

Inspired by [Meshtastic](https://meshtastic.org/) — but built for robotics, not messaging.

**No cloud required. No subscriptions.** Optional cloud integrations are on the roadmap as add-ons.

Currently the project works with [**Heltec WiFi LoRa 32 V3**](https://heltec.org/project/wifi-lora-32-v3/) (ESP32-S3). Any ESP32 board should work though. Just add a HAL file to boards folder, add a section to the platformio.ini, and configure any relevant sdk config defaults.

Also working on support for TTGO LoRa32 V1 and a port for the arduino framework Wemos D1 Mini (ESP8266/Arduino).

> **No Espressif provisioning app required**
> Connect via browser on any phone or PC.

## Target Use Cases

| Use Case | Notes |
|----------|-------|
| Realtime Control | Tank/Robot Vacuum, 2 drive wheels — current default |
| Device-to-device control | Direct or through a gateway |
| Sequenced command playback | Pre-arranged move lists |
| Autonomous + status push | e.g. chicken coop door → [Home Assistant](https://www.home-assistant.io/) |
| Smart home integration | Home Assistant, Google, Amazon |
| Custom game-engine frontend | [Unity](https://unity.com/) / [Unreal](https://www.unrealengine.com/) app as UI |

---

[![PlatformIO](https://img.shields.io/badge/PlatformIO-6.9+-orange.svg)](https://platformio.org/)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.x-blue.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32s3/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

---

## Workflow:

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

```

Use VS Code shortcuts: **Ctrl+Alt+B** (Build), **Ctrl+Alt+U** (Upload), **Ctrl+Alt+S** (Monitor).

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
