#pragma once
/**
 * i_battery.h — Battery voltage measurement (GPIO1, ADC1_CH0).
 *
 * The Heltec V3 has a voltage divider: VBAT → 390 kΩ → GPIO1 → 100 kΩ → GND.
 * VBAT = VADC × (390 + 100) / 100 = VADC × 4.9
 *
 * Uses ADC calibration (esp_adc_cal or adc_cali on IDF 5.x) when available
 * so readings compensate for chip-to-chip voltage reference spread.
 *
 * Readings are taken as a rolling average of I_BATTERY_SAMPLES samples to
 * smooth out noise from WiFi radio bursts sharing the power rail.
 */

#include <stdint.h>

#define I_BATTERY_SAMPLES 8   /* rolling average depth */

/**
 * Initialise ADC1 channel 0 with calibration. Call once at boot.
 * Safe to call before or after WiFi start — ADC1 is not affected by WiFi.
 */
void i_battery_init(void);

/**
 * Read current battery voltage in millivolts.
 * Returns 0 if not initialised.
 */
uint32_t i_battery_read_mv(void);

/**
 * Return battery state of charge as a percentage (0–100).
 * Linearly interpolated between BATTERY_V_EMPTY_MV and BATTERY_V_FULL_MV.
 * Clamped to [0, 100].
 */
uint8_t i_battery_percent(void);
