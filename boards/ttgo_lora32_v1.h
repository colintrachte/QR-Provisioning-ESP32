/**
 * boards/ttgo_lora32_v1.h — Pin and peripheral constants.
 *
 * Selected by -DBOARD_TTGO_LORA32_V1 in platformio.ini build_flags.
 *
 * Hardware notes
 * ──────────────
 * OLED power: The AXP192 PMIC keeps the 3.3 V rail on whenever the board
 *   is powered. There is no GPIO-controlled Vext switch — DISP_PIN_VEXT -1
 *   tells display_init() to skip the power-rail step entirely.
 *
 * AXP192: Sits on the same I2C bus as the OLED (addr 0x34). i_sensors
 *   will find it during the bus scan. A dedicated i_axp192 driver should
 *   be added to read battery voltage and charge state from its registers
 *   instead of using a raw ADC divider.
 *
 * Battery ADC: There is no direct GPIO→ADC voltage divider on V1. Battery
 *   voltage is read from the AXP192 over I2C. The ADC constants below are
 *   stubs — i_battery.c will initialise but return 0 until an AXP192 driver
 *   is wired in. Define BOARD_BATTERY_VIA_AXP192 to suppress the ADC init
 *   warning in i_battery.c.
 *
 * GPS: NEO-6M on UART2. Not wired into this firmware yet — placeholder
 *   GPIOs defined for future i_gps driver.
 *
 * LED: GPIO25, active HIGH (same polarity as Heltec, different pin).
 */
#pragma once

#define I2C_POST_WIFI_DELAY_MS  250

/* ── Display (SSD1306 128×64, I2C) ─────────────────────────────────────────*/
#define DISP_PIN_VEXT   (-1) /* No GPIO power switch — PMIC keeps rail on   */
#define DISP_PIN_RST    16
#define DISP_PIN_SDA    21   /* Standard ESP32 I2C SDA */
#define DISP_PIN_SCL    22   /* Standard ESP32 I2C SCL */
#define DISP_I2C_ADDR   0x78 /* 8-bit (0x3C << 1) — same panel as Heltec    */
#define DISP_WIDTH      128
#define DISP_HEIGHT     64

/* ── LED (onboard blue, active HIGH) ────────────────────────────────────────*/
#define LED_GPIO            25
#define LED_LEDC_CHANNEL    LEDC_CHANNEL_0
#define LED_LEDC_TIMER      LEDC_TIMER_0
#define LED_LEDC_RESOLUTION LEDC_TIMER_8_BIT
#define LED_LEDC_FREQ_HZ    1000

/* ── Battery — via AXP192 PMIC, not direct ADC ──────────────────────────────
 * These stubs keep i_battery.c compiling. The ADC init will succeed but
 * read garbage until AXP192 I2C readout replaces the ADC path. */
#define BOARD_BATTERY_VIA_AXP192        /* suppresses ADC-only warning       */
#define BATTERY_ADC_UNIT     ADC_UNIT_1
#define BATTERY_ADC_CHANNEL  ADC_CHANNEL_6  /* GPIO34 — input-only, safe stub */
#define BATTERY_R_TOP        100
#define BATTERY_R_BOT        100
#define BATTERY_V_FULL_MV    4200
#define BATTERY_V_EMPTY_MV   3300
#define I_BATTERY_SAMPLES    8

/* ── AXP192 PMIC ────────────────────────────────────────────────────────────*/
#define AXP192_I2C_ADDR_7BIT  0x34  /* for i_sensors bus scan mapping        */

/* ── Re-provision button ────────────────────────────────────────────────────*/
#define REPROV_GPIO     0
#define REPROV_HOLD_MS  3000

/* ── Motor LEDC ─────────────────────────────────────────────────────────────*/
#define MOTOR_LEDC_TIMER      LEDC_TIMER_1
#define MOTOR_LEDC_RESOLUTION LEDC_TIMER_10_BIT
#define MOTOR_LEDC_FREQ_HZ    20000
#define MOTOR_L_LEDC_CHANNEL  LEDC_CHANNEL_1
#define MOTOR_R_LEDC_CHANNEL  LEDC_CHANNEL_2

/* ── GPS (NEO-6M, UART2) — placeholder for future i_gps driver ─────────────*/
#define GPS_UART_NUM    UART_NUM_2
#define GPS_PIN_RX      34
#define GPS_PIN_TX      12

/* ── Board identity ─────────────────────────────────────────────────────────*/
#define BOARD_NAME  "TTGO LoRa32 V1"
#define I2C_POST_WIFI_DELAY_MS  100
