# Hardware Reference

## Supported Boards

| Board | MCU | Flash | Display | Status |
|-------|-----|-------|---------|--------|
| [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/) | ESP32-S3FN8 @ 240 MHz | 8 MB | SSD1306 128×64 I2C | Primary target |
| [TTGO LoRa32 V1](https://github.com/LilyGO/TTGO-LORA32) | ESP32 @ 240 MHz | 4 MB | SSD1306 128×64 I2C | Supported |
| [Wemos D1 Mini](https://www.wemos.cc/en/latest/d1/d1_mini.html) | ESP8266 @ 160 MHz | 4 MB | None (optional external) | Arduino framework |

## Heltec WiFi LoRa 32 V3 Pinout

| Function | GPIO | Notes |
|----------|------|-------|
| OLED SDA | 17 | I2C data, 400 kHz |
| OLED SCL | 18 | I2C clock |
| OLED RST | 21 | Active-low reset |
| Vext power | 36 | High = OLED powered via external regulator |
| User button | 0 | BOOT/USR, pull-down, hold 3s to erase credentials |
| Onboard LED | 35 | White LED, LEDC PWM |

Motor pins and all other assignments are in `main/config.h` and `boards/heltec_lora32_v3.h`.

## TTGO LoRa32 V1 Differences

| Feature | Heltec V3 | TTGO V1 |
|---------|-----------|---------|
| PMIC | None (direct GPIO) | AXP192 (I2C @ 0x34) |
| Battery read | ADC1_CH0 (GPIO1) | AXP192 ADC |
| GPS | None | NEO-6M (UART2) |
| PSRAM | None | 2 MB |

The AXP192 driver is not yet implemented — battery readings return 0 on TTGO V1 until wired in.

## Battery Voltage Divider

For Heltec V3 and D1 Mini (direct ADC path):

```
VBAT ──[R_TOP]──┬──[R_BOT]─── GND
                │
               ADC
```

Default values (configurable in `platformio.ini` or `config.h`):

| Board | R_TOP | R_BOT | VBAT range | VADC range |
|-------|-------|-------|------------|------------|
| Heltec V3 | 100kΩ | 100kΩ | 0–6.6V | 0–3.3V |
| D1 Mini | 100kΩ | 100kΩ | 0–6.6V | 0–3.3V |
| TTGO V1 | N/A | N/A | Via AXP192 | Via AXP192 |

## Motor Wiring

### DIR/PWM/EN Mode (2-channel H-bridge)

| Signal | Left Motor | Right Motor |
|--------|-----------|-------------|
| PWM | GPIO X | GPIO Y |
| DIR | GPIO A | GPIO B |
| EN | GPIO C | GPIO D |

### BTN8982 Mode (4 half-bridges)

| Signal | Left | Right |
|--------|------|-------|
| IN1 (PWM) | GPIO X | GPIO Z |
| IN2 (PWM) | GPIO Y | GPIO W |
| EN | GPIO E | GPIO F |

See `boards/<name>.h` for specific GPIO numbers per board.

## I2C Bus Sharing

The SSD1306 OLED and any future sensors share I2C_NUM_0:

```
ESP32-S3 ──┬── SDA (GPIO17) ──┬── OLED SDA
           │                  └── Sensor SDA
           └── SCL (GPIO18) ──┬── OLED SCL
                              └── Sensor SCL
```

`i_sensors.c` owns the bus after WiFi init. `display_reinit_i2c()` re-attaches the display as a device on the shared bus.

## Power Budget

| Component | Typical | Peak | Notes |
|-----------|---------|------|-------|
| ESP32-S3 (WiFi active) | 240 mA | 500 mA | During TX burst |
| SSD1306 | 10 mA | 20 mA | All pixels on |
| Motor driver (2×) | 200 mA | 2 A | Per channel, stall current |
| Total | 450 mA | 2.7 A | Use 3A+ regulator for motors |

**Battery:** 2S LiPo (7.4V nominal, 6.0–8.4V range). The voltage divider scales this to 0–3.3V for ADC.
