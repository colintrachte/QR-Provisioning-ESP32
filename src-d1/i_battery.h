/**
 * i_battery.h — Battery voltage measurement for ESP8266.
 *
 * Reads A0 via analogRead() and applies a voltage divider ratio.
 * Rolling average over I_BATTERY_SAMPLES for stability.
 */

#pragma once
#include <Arduino.h>

/* Voltage divider resistors -- adjust for your board */
#ifndef BATTERY_R_TOP
#define BATTERY_R_TOP    100000   /* 100k */
#endif
#ifndef BATTERY_R_BOT
#define BATTERY_R_BOT    100000   /* 100k -> 2:1 divider */
#endif

/* Battery chemistry thresholds (Li-ion 1S) */
#ifndef BATTERY_V_EMPTY_MV
#define BATTERY_V_EMPTY_MV  3200
#endif
#ifndef BATTERY_V_FULL_MV
#define BATTERY_V_FULL_MV   4200
#endif

#define I_BATTERY_SAMPLES   8

void i_battery_init(void);
uint32_t i_battery_read_mv(void);
uint8_t  i_battery_percent(void);
