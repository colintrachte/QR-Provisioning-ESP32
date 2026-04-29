#pragma once

#include "board.h"
/**
 * config.h — Compile-time configuration for the robot provisioning firmware.
 *
 * One file. Every application-level pin assignment, threshold, timing
 * constant, and debug flag lives here.
 *
 * Board-specific constants (display pins, LED GPIO, battery ADC, LEDC
 * channels/timers, re-provision button) live in boards/<board>.h and are
 * pulled in via board.h. Do NOT redefine those here.
 *
 * Modules own logic; this file owns values.
 */

/* ── SoftAP credentials ─────────────────────────────────────────────────────*/
#define AP_SSID     "RobotSetup"
#define AP_PASSWORD "robot1234"   /* ≥8 chars for WPA2; "" = open network   */
#define AP_GW_IP    "192.168.4.1" /* Gateway IP for DNS hijack + portal URL  */

/* ── Main loop timing ───────────────────────────────────────────────────────*/
#define MAIN_LOOP_MS              100   /* 10 Hz UI tick                     */
#define WIFI_INIT_FAIL_DELAY_MS  10000  /* Error screen hold before restart  */
#define WIFI_MAX_RESTART_ATTEMPTS    3  /* Boot-loop guard threshold         */

/* ── mDNS ───────────────────────────────────────────────────────────────────*/
#define MDNS_ENABLE   1
#define MDNS_HOSTNAME "robot"   /* Advertised as robot.local                 */

/* ── Display — application behaviour ───────────────────────────────────────
 * Hardware pins (SDA, SCL, RST, VEXT, I2C_ADDR, WIDTH, HEIGHT) are defined
 * in the board header. Only application-level tuning lives here.
 */
#define DISP_SLEEP_TIMEOUT_S 120  /* 0 = always on                          */

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
 * Wiring choice — not board-specific (board defines safe GPIO ranges).
 * Use GPIOs from the safe list for your board:
 *   Heltec V3:  1,2,4,5,6,7,19,20,47,48
 *   TTGO V1:    avoid 33–38 (flash), 21/22 (I2C), 25 (LED), 16 (OLED RST)
 * Avoid 33–38 (Flash/SubSPI on S3), DISP_PIN_RST, LED_GPIO, DISP_PIN_VEXT.
 */
#define MOTOR_L_DIR_GPIO    4
#define MOTOR_L_PWM_GPIO    5
#define MOTOR_L_EN_GPIO     6

#define MOTOR_R_DIR_GPIO    7
#define MOTOR_R_PWM_GPIO   19
#define MOTOR_R_EN_GPIO    20

/* LEDC channels and timer for motor PWM are defined in the board header:
 *   MOTOR_L_LEDC_CHANNEL, MOTOR_R_LEDC_CHANNEL
 *   MOTOR_LEDC_TIMER, MOTOR_LEDC_FREQ_HZ, MOTOR_LEDC_RESOLUTION
 */

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

/* BTN8982 uses LEDC channels/timer/freq/resolution from the board header. */

/* ── Drive controller ───────────────────────────────────────────────────────
 * DRIVE_DEADBAND: joystick axis values below this magnitude are treated as zero.
 *   Prevents motor creep from joystick centre drift.
 * DRIVE_MAX_DELTA_PER_TICK: maximum change in output per 10 ms tick.
 *   Soft acceleration ramp to avoid current spikes and wheel slip.
 */
#define DRIVE_DEADBAND           0.05f
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
