#pragma once
/**
 * ctrl_drive.h — Differential drive controller.
 *
 * Converts joystick (x, y) axes to left/right motor velocities with:
 *   - Deadband filtering (DRIVE_DEADBAND in config.h)
 *   - Soft acceleration ramp (DRIVE_MAX_DELTA_PER_TICK per 10 ms tick)
 *   - Arming gate: drive commands ignored while disarmed
 *
 * This module owns arming state and calls o_motors_drive() / o_motors_stop().
 * It does not touch the LED or display — those are signalled via the caller.
 *
 * Call ctrl_drive_tick() from app_task at 10 Hz to advance the ramp.
 */

#include <stdbool.h>

/**
 * Initialise the controller. Calls o_motors_init() internally.
 * Must be called before any other ctrl_drive_* function.
 */
void ctrl_drive_init(void);

/**
 * Set target joystick axes. Safe to call from the WebSocket handler task.
 * Axes are applied on the next ctrl_drive_tick().
 * @param x  Lateral axis: -1.0 = full left, +1.0 = full right.
 * @param y  Forward axis: -1.0 = full reverse, +1.0 = full forward.
 */
void ctrl_drive_set_axes(float x, float y);

/**
 * Arm or disarm the drive system.
 * While disarmed: motors are stopped and drive commands are ignored.
 * While armed: ctrl_drive_tick() advances the ramp toward the target axes.
 * @param armed  true to arm, false to disarm.
 */
void ctrl_drive_set_armed(bool armed);

/** Returns true if currently armed. */
bool ctrl_drive_is_armed(void);

/**
 * Advance the velocity ramp and apply motor outputs.
 * Call from app_task at 10 Hz (every MAIN_LOOP_MS).
 */
void ctrl_drive_tick(void);

/**
 * Immediately stop both motors regardless of arming state.
 * Used for emergency stop from WebSocket disconnect or WDT.
 */
void ctrl_drive_emergency_stop(void);
