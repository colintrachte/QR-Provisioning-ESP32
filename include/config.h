#pragma once
/**
 * config.h — Compile-time configuration for the robot provisioning firmware.
 *
 * One file. Every pin, threshold, timing constant, and debug flag lives here.
 * Modules own logic; this file owns values.
 */

/* ── SoftAP credentials ─────────────────────────────────────────────────────*/
#define AP_SSID     "RobotSetup"
#define AP_PASSWORD "robot1234"   /* ≥8 chars for WPA2; "" = open network   */
#define AP_GW_IP    "192.168.4.1" /* Gateway IP for DNS hijack + portal URL  */

/* ── Re-provisioning button ─────────────────────────────────────────────────
 * GPIO0 = BOOT/USER_SW on Heltec V3. Active-low with internal pull-up.
 */
#define REPROV_GPIO    0
#define REPROV_HOLD_MS 3000

/* ── Main loop timing ───────────────────────────────────────────────────────*/
#define MAIN_LOOP_MS              100   /* 10 Hz UI tick                     */
#define WIFI_INIT_FAIL_DELAY_MS  10000  /* Error screen hold before restart  */
#define WIFI_MAX_RESTART_ATTEMPTS    3  /* Boot-loop guard threshold         */

/* ── mDNS ───────────────────────────────────────────────────────────────────*/
#define MDNS_ENABLE   1
#define MDNS_HOSTNAME "mulebot"   /* Advertised as mulebot.local            */

/* ── Display — Heltec V3 SSD1306 via I2C_NUM_0 ─────────────────────────────
 * GPIO36 controls the Vext rail that powers the OLED.
 * GPIO21 is the OLED hardware reset line.
 * I2C address is the 8-bit form (0x3C << 1 = 0x78) as u8g2 expects.
 * WARNING: GPIO33–38 are SubSPI/Flash lines on this board. Do not reassign.
 */
#define DISP_PIN_SDA   17
#define DISP_PIN_SCL   18
#define DISP_PIN_RST   21
#define DISP_PIN_VEXT  36
#define DISP_I2C_ADDR  0x78      /* 0x3C << 1                               */
#define DISP_WIDTH     128
#define DISP_HEIGHT     64
#define DISP_SLEEP_TIMEOUT_S 120  /* 0 = always on                          */

/* ── Onboard white LED ──────────────────────────────────────────────────────
 * GPIO35 = "LED Write Ctrl" per Heltec V3 datasheet. Active-high.
 * Driven via LEDC for PWM brightness control.
 * LED_LEDC_CHANNEL: pick any free LEDC channel (0–7 on S3, 0 not used by motors).
 * LED_LEDC_TIMER:   LEDC timer index.
 */
#define LED_GPIO          35
#define LED_LEDC_CHANNEL   0
#define LED_LEDC_TIMER     0
#define LED_LEDC_FREQ_HZ  5000   /* 5 kHz — above audible, smooth at 8-bit  */
#define LED_LEDC_RESOLUTION LEDC_TIMER_8_BIT   /* 256 steps                 */

/* ── Battery ADC ────────────────────────────────────────────────────────────
 * GPIO1 = ADC1_CH0 = battery voltage sense.
 * Voltage divider: VBAT = VADC × (100 + 390) / 100  →  factor 4.9
 * Typical LiPo: 3.0 V empty, 4.2 V full.
 */
#define BATTERY_ADC_CHANNEL  ADC_CHANNEL_0   /* GPIO1 = ADC1_CH0            */
#define BATTERY_ADC_UNIT     ADC_UNIT_1
#define BATTERY_R_TOP        390             /* kΩ, top of divider          */
#define BATTERY_R_BOT        100             /* kΩ, bottom of divider       */
#define BATTERY_V_FULL_MV   4200
#define BATTERY_V_EMPTY_MV  3000

/* ── Motor driver mode ──────────────────────────────────────────────────────
 * Select ONE of the two modes by setting MOTOR_DRIVER_MODE.
 *
 * MOTOR_MODE_DIR_PWM_EN
 *   Generic H-bridge with separate DIR, PWM, and EN pins per channel.
 *   DIR = GPIO output (HIGH = forward), PWM = LEDC output, EN = GPIO output.
 *   Examples: L298N, TB6612, many Chinese H-bridge modules.
 *
 * MOTOR_MODE_BTN8982
 *   Infineon BTN8982 half-bridge pair. Two PWM inputs per motor side (IN1/IN2).
 *   Braking: IN1=0, IN2=0. Forward: IN1=PWM, IN2=0. Reverse: IN1=0, IN2=PWM.
 *   IS (current sense) pins are optional ADC reads — set to -1 to disable.
 *   EN (inhibit) pins are active-high, can be tied high if not needed in software.
 */
#define MOTOR_MODE_DIR_PWM_EN  0
#define MOTOR_MODE_BTN8982     1

#define MOTOR_DRIVER_MODE  MOTOR_MODE_DIR_PWM_EN   /* <<< change me */

/* ── Motor pins — DIR/PWM/EN mode ──────────────────────────────────────────
 * Use GPIOs from the safe list: 1,2,4,5,6,7,19,20,47,48
 * Avoid 33–38 (Flash/SubSPI), 21 (OLED RST), 35 (LED), 36 (Vext).
 */
#define MOTOR_L_DIR_GPIO    4
#define MOTOR_L_PWM_GPIO    5
#define MOTOR_L_EN_GPIO     6

#define MOTOR_R_DIR_GPIO    7
#define MOTOR_R_PWM_GPIO   19
#define MOTOR_R_EN_GPIO    20

/* LEDC channels for PWM (channels 1 and 2; channel 0 is the LED) */
#define MOTOR_L_LEDC_CHANNEL   1
#define MOTOR_R_LEDC_CHANNEL   2
#define MOTOR_LEDC_TIMER       1        /* separate timer from LED          */
#define MOTOR_LEDC_FREQ_HZ  5000
#define MOTOR_LEDC_RESOLUTION  LEDC_TIMER_10_BIT   /* 1024 steps            */

/* ── Motor pins — BTN8982 mode ──────────────────────────────────────────────
 * Each BTN8982 half-bridge pair needs IN1, IN2 (PWM), and optionally EN + IS.
 * Set IS_GPIO to -1 to skip current-sense ADC reads.
 */
#define MOTOR_L_IN1_GPIO    4
#define MOTOR_L_IN2_GPIO    5
#define MOTOR_L_EN_BTN_GPIO 6    /* tie high externally to hardwire enable  */
#define MOTOR_L_IS_GPIO    -1    /* -1 = not connected                      */

#define MOTOR_R_IN1_GPIO    7
#define MOTOR_R_IN2_GPIO   19
#define MOTOR_R_EN_BTN_GPIO 20
#define MOTOR_R_IS_GPIO    -1

/* BTN8982 uses same LEDC channels and timer as DIR_PWM mode above */

/* ── Drive controller ───────────────────────────────────────────────────────
 * DRIVE_DEADBAND: joystick axis values below this magnitude are treated as zero.
 *   Prevents motor creep from joystick centre drift.
 * DRIVE_MAX_DELTA_PER_TICK: maximum change in output per 10 ms tick.
 *   Soft acceleration ramp to avoid current spikes and wheel slip.
 */
#define DRIVE_DEADBAND          0.05f
#define DRIVE_MAX_DELTA_PER_TICK 0.05f  /* 0→full in ~200 ms at 10 Hz tick */

/* ── Health monitor ─────────────────────────────────────────────────────────*/
#define HEALTH_RSSI_WARN_DBM      -75
#define HEALTH_SCAN_INTERVAL_MS  5000

/* ── Debug flags (set 0 to silence a subsystem) ─────────────────────────────*/
#define DEBUG_WIFI    1
#define DEBUG_DISPLAY 1
#define DEBUG_UI      1
#define DEBUG_HEALTH  1
#define DEBUG_MAIN    1
#define DEBUG_MOTORS  1
#define DEBUG_SENSORS 1
