#pragma once

#include <stdint.h>

extern const uint8_t ICON_WIFI_0[8];
extern const uint8_t ICON_WIFI_1[8];
extern const uint8_t ICON_WIFI_2[8];
extern const uint8_t ICON_WIFI_3[8];

extern const uint8_t ICON_BAT_0[8];
extern const uint8_t ICON_BAT_25[8];
extern const uint8_t ICON_BAT_50[8];
extern const uint8_t ICON_BAT_75[8];
extern const uint8_t ICON_BAT_100[8];

extern const uint8_t ICON_ARMED[8];
extern const uint8_t ICON_DISARMED[8];

extern const uint8_t ICON_TEMP_WARN[8];

extern const uint8_t ICON_MOTOR_L[8];
extern const uint8_t ICON_MOTOR_R[8];

const uint8_t* ui_icon_get_battery(uint8_t percentage);
const uint8_t* ui_icon_get_wifi(int8_t rssi);
