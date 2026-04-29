/**
 * boards/heltec_wifi_lora_32_v3.h — Pin and peripheral constants.
 *
 * Selected by -DBOARD_HELTEC_LORA32_V3 in platformio.ini build_flags.
 * All application code reads from these names only — never hardcode GPIO
 * numbers in .c files.
 */
#pragma once

/* ── Display (SSD1306 128×64, I2C) ─────────────────────────────────────────*/
#define DISP_PIN_VEXT   36   /* HIGH = Vext rail on. -1 = no GPIO power switch */
#define DISP_PIN_RST    21
#define DISP_PIN_SDA    17
#define DISP_PIN_SCL    18
#define DISP_I2C_ADDR   0x78 /* 8-bit (0x3C << 1) */
#define DISP_WIDTH      128
#define DISP_HEIGHT     64

/* ── LED (onboard white, active HIGH) ───────────────────────────────────────
 * GPIO35 = "LED Write Ctrl" per Heltec V3 datasheet. Active-high.
 * Driven via LEDC for PWM brightness control.
 */
#define LED_GPIO            35
#define LED_LEDC_CHANNEL    LEDC_CHANNEL_0
#define LED_LEDC_TIMER      LEDC_TIMER_0
#define LED_LEDC_RESOLUTION LEDC_TIMER_8_BIT   /* 256 steps                 */
#define LED_LEDC_FREQ_HZ    5000               /* above audible, smooth PWM */

/* ── Battery ADC (voltage divider on GPIO1) ─────────────────────────────────
 * GPIO1 = ADC1_CH0. Divider: VBAT = VADC × (100 + 390) / 100 → factor 4.9
 */
#define BATTERY_ADC_UNIT     ADC_UNIT_1
#define BATTERY_ADC_CHANNEL  ADC_CHANNEL_0  /* GPIO1 = ADC1_CH0             */
#define BATTERY_R_TOP        390            /* kΩ — top of divider          */
#define BATTERY_R_BOT        100            /* kΩ — bottom of divider       */
#define BATTERY_V_FULL_MV    4200
#define BATTERY_V_EMPTY_MV   3300           /* LiPo safe cutoff             */
#define I_BATTERY_SAMPLES    8

/* ── Re-provision button ────────────────────────────────────────────────────
 * GPIO0 = BOOT/USER_SW. Active-low with internal pull-up.
 */
#define REPROV_GPIO     0
#define REPROV_HOLD_MS  3000

/* ── Motor LEDC (separate timer from LED) ───────────────────────────────────
 * Channels 1 and 2 reserved for motors; channel 0 is the LED.
 * 20 kHz: above audible range, suitable for most motor drivers.
 * WARNING: GPIO33–38 are SubSPI/Flash lines on this board. Do not reassign.
 */
#define MOTOR_LEDC_TIMER      LEDC_TIMER_1
#define MOTOR_LEDC_RESOLUTION LEDC_TIMER_10_BIT   /* 1024 steps             */
#define MOTOR_LEDC_FREQ_HZ    20000
#define MOTOR_L_LEDC_CHANNEL  LEDC_CHANNEL_1
#define MOTOR_R_LEDC_CHANNEL  LEDC_CHANNEL_2

/* ── Board identity ─────────────────────────────────────────────────────────*/
#define BOARD_NAME             "Heltec WiFi LoRa 32 V3"
#define I2C_POST_WIFI_DELAY_MS 100
