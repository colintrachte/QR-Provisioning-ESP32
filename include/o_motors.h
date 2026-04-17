#pragma once
/**
 * o_motors.h — Brushed DC motor output driver.
 *
 * Supports two hardware topologies selected by MOTOR_DRIVER_MODE in config.h:
 *
 *   MOTOR_MODE_DIR_PWM_EN
 *     One DIR pin (GPIO, sets direction), one PWM pin (LEDC, sets speed),
 *     one EN pin (GPIO, enables the H-bridge). Compatible with L298N, TB6612,
 *     and most generic dual H-bridge modules.
 *
 *   MOTOR_MODE_BTN8982
 *     Infineon BTN8982 half-bridge pairs. Each motor side has IN1 and IN2,
 *     both driven as PWM. Braking: IN1=0, IN2=0. Forward: IN1=duty, IN2=0.
 *     Reverse: IN1=0, IN2=duty. EN (inhibit) pin can be tied high externally.
 *     Optional IS (current-sense) ADC reads; set IS_GPIO to -1 to skip.
 *
 * This file is an output driver only. It applies values given to it — no
 * joystick mixing, no arming logic. That belongs in ctrl_drive.c.
 *
 * Thread safety: o_motors_drive() and o_motors_stop() are safe from any task.
 * They write atomically to LEDC registers; no additional mutex is needed.
 */

#include <stdbool.h>
#include "esp_err.h"

/* Motor channel indices — used as array subscripts internally */
typedef enum { MOTOR_LEFT = 0, MOTOR_RIGHT = 1 } motor_ch_t;

/**
 * Initialise GPIO and LEDC for the motor driver topology selected in config.h.
 * Must be called before any other o_motors_* function.
 * @return ESP_OK or an esp_err_t on peripheral configuration failure.
 */
esp_err_t o_motors_init(void);

/**
 * Set both motor velocities.
 * @param left   Left motor.  -1.0 = full reverse, 0.0 = stop, +1.0 = full forward.
 * @param right  Right motor. Same scale.
 * Values are clamped to [-1.0, +1.0].
 */
void o_motors_drive(float left, float right);

/**
 * Actively brake both motors (coasts or brakes depending on driver topology).
 * On DIR_PWM_EN: sets PWM duty to 0 and asserts EN low.
 * On BTN8982: sets IN1=0 and IN2=0 (brake to GND).
 */
void o_motors_stop(void);

/**
 * Enable or disable the motor driver hardware output.
 * Useful for safety: call o_motors_enable(false) when disarmed.
 * o_motors_drive() re-enables automatically.
 */
void o_motors_enable(bool en);
