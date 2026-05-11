/**
 * i_battery.cpp — Battery voltage measurement for ESP8266.
 *
 * The ESP8266 ADC is 10-bit (0-1023) with a 0-1.0V range on the bare chip,
 * but most dev boards (Wemos D1 Mini) include an internal 220k/100k divider
 * on A0, giving ~0-3.3V range. If your board has a different divider or an
 * external one, adjust BATTERY_R_TOP / BATTERY_R_BOT.
 *
 * Rolling average over I_BATTERY_SAMPLES to smooth noise.
 */

#include "i_battery.h"

static const char *TAG = "i_battery";

static uint32_t s_samples[I_BATTERY_SAMPLES] = {0};
static int      s_sample_idx = 0;
static bool     s_buf_full   = false;
static bool     s_initialized = false;

/* Reference voltage for the ADC. Most D1 Mini boards use ~3.3V after the
 * internal divider. If you have an external divider directly on the battery,
 * measure Vref with a multimeter and adjust. */
#ifndef BATTERY_ADC_VREF_MV
#define BATTERY_ADC_VREF_MV  3300
#endif

void i_battery_init(void)
{
    s_initialized = true;
    Serial.printf("[%s] Battery ADC init (A0, Vref=%d mV)\n", TAG, BATTERY_ADC_VREF_MV);
}

static uint32_t rolling_average_mv(uint32_t new_mv)
{
    s_samples[s_sample_idx] = new_mv;
    s_sample_idx = (s_sample_idx + 1) % I_BATTERY_SAMPLES;
    if (s_sample_idx == 0) s_buf_full = true;

    int count = s_buf_full ? I_BATTERY_SAMPLES : s_sample_idx;
    uint32_t sum = 0;
    for (int i = 0; i < count; i++) sum += s_samples[i];
    return sum / count;
}

uint32_t i_battery_read_mv(void)
{
    if (!s_initialized) return 0;

    int raw = analogRead(A0);
    /* Vadc = raw * Vref / 1023 */
    uint32_t vadc_mv = (uint32_t)raw * BATTERY_ADC_VREF_MV / 1023;

    /* Scale by external divider ratio: VBAT = VADC * (R_TOP + R_BOT) / R_BOT */
    uint32_t vbat_mv = vadc_mv * (BATTERY_R_TOP + BATTERY_R_BOT) / BATTERY_R_BOT;

    return rolling_average_mv(vbat_mv);
}

uint8_t i_battery_percent(void)
{
    uint32_t mv = i_battery_read_mv();
    if (mv <= BATTERY_V_EMPTY_MV) return 0;
    if (mv >= BATTERY_V_FULL_MV)  return 100;
    return (uint8_t)(100UL * (mv - BATTERY_V_EMPTY_MV) /
                             (BATTERY_V_FULL_MV - BATTERY_V_EMPTY_MV));
}
