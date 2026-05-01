/**
 * ctrl_drive.c — Differential drive controller.
 *
 * Mixing: arcade drive.
 *   left  = y + x
 *   right = y - x
 * Both outputs clamped to [-1, +1] after mixing.
 *
 * Ramp limiting (DRIVE_MAX_DELTA_PER_TICK)
 * ─────────────────────────────────────────
 * Prevents instantaneous jumps in motor current when a joystick snaps to
 * full deflection or zero. On brushed DC motors with no current limiting,
 * large sudden current spikes can trigger the driver IC's protection and
 * cause a brief stall.
 *
 * Command watchdog
 * ────────────────
 * ctrl_drive_feed_watchdog() must be called each time a valid command is
 * received from the WebSocket handler. ctrl_drive_tick() checks elapsed
 * time; if no command arrives within DRIVE_WATCHDOG_MS the controller
 * disarms itself and calls ctrl_drive_emergency_stop().
 *
 * This catches: browser crash, WiFi drop, tab navigation, and any other
 * path where the TCP close frame is delayed or never sent.
 *
 * Thread safety
 * ─────────────
 * ctrl_drive_set_axes(), ctrl_drive_set_armed(), and ctrl_drive_feed_watchdog()
 * write shared state from the WS task; ctrl_drive_tick() reads it on app_task.
 *
 * s_target_x/y use int16_t fixed-point (range -10000..10000 = -1.0..1.0).
 * int16_t writes are atomic on Xtensa LX7 (16-bit aligned store is a single
 * instruction). This avoids the torn-read hazard that exists with float:
 * IEEE 754 float stores are 32-bit but the FPU does not guarantee atomicity
 * across a task switch mid-store (exponent and mantissa can be written in two
 * separate micro-ops under some compiler backends).
 *
 * All other shared scalars (uint32_t, bool) are volatile; 32/8-bit aligned
 * writes on Xtensa LX7 are single-instruction atomic.
 * ctrl_drive_tick() runs from one task only.
 */

#include "ctrl_drive.h"
#include "o_motors.h"
#include "config.h"

#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ctrl_drive";

/* ── Watchdog ───────────────────────────────────────────────────────────────
 * s_last_cmd_ms is written by the WS task, read by app_task.
 * uint32_t write on ESP32-S3 is atomic; volatile prevents register caching.
 * DRIVE_WATCHDOG_MS must be defined in config.h — no fallback here. */
static volatile uint32_t s_last_cmd_ms  = 0;
static volatile bool     s_watchdog_ok  = false;  /* false until first feed */

/* ── Target axes (fixed-point, int16_t) ─────────────────────────────────────
 * Range: -10000..10000  maps to  -1.0..1.0
 * int16_t stores are atomic on Xtensa LX7; this avoids the torn-read hazard
 * present with volatile float (FPU store is not guaranteed single-instruction
 * across a task switch). */
#define AXES_SCALE  10000          /* fixed-point scale factor                */
static volatile int16_t s_target_x_fp = 0;   /* fixed-point, range ±AXES_SCALE */
static volatile int16_t s_target_y_fp = 0;
static volatile bool    s_armed       = false;

/* Current ramped outputs */
static float s_current_left  = 0.0f;
static float s_current_right = 0.0f;

/* ── Helpers ────────────────────────────────────────────────────────────────*/

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float ramp(float current, float target)
{
    float delta = target - current;
    float max   = DRIVE_MAX_DELTA_PER_TICK;
    if (delta >  max) delta =  max;
    if (delta < -max) delta = -max;
    return current + delta;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

void ctrl_drive_init(void)
{
    o_motors_init();
    s_last_cmd_ms = now_ms();
    ESP_LOGI(TAG, "ctrl_drive_init OK (watchdog=%d ms)", DRIVE_WATCHDOG_MS);
}

/**
 * Feed the command watchdog. Call this every time a valid drive command
 * (including "stop") is received from the WebSocket.
 */
void ctrl_drive_feed_watchdog(void)
{
    s_last_cmd_ms = now_ms();
    s_watchdog_ok = true;
}

void ctrl_drive_set_axes(float x, float y)
{
    /* Clamp inputs defensively — the JS already does this but belt-and-suspenders */
    x = clampf(x, -1.0f, 1.0f);
    y = clampf(y, -1.0f, 1.0f);

    /* Deadband: zero out, do NOT rescale here — rescaling is done client-side
     * so the firmware sees a fully-processed value. If rescaling were done
     * both places the curve would be applied twice. */
    float fx = (fabsf(x) < DRIVE_DEADBAND) ? 0.0f : x;
    float fy = (fabsf(y) < DRIVE_DEADBAND) ? 0.0f : y;

    /* Convert to fixed-point for atomic storage. int16_t write is a single
     * instruction on Xtensa LX7, avoiding the torn-read race that exists with
     * direct float stores across task boundaries. */
    s_target_x_fp = (int16_t)(fx * AXES_SCALE);
    s_target_y_fp = (int16_t)(fy * AXES_SCALE);
}

void ctrl_drive_set_armed(bool armed)
{
    s_armed = armed;
    if (!armed) {
        s_target_x_fp = 0;
        s_target_y_fp = 0;
    }
    ESP_LOGI(TAG, "%s", armed ? "ARMED" : "DISARMED");
}

bool ctrl_drive_is_armed(void)
{
    return s_armed;
}

void ctrl_drive_tick(void)
{
    /* ── Watchdog check ──────────────────────────────────────────────────── */
    if (s_armed && s_watchdog_ok) {
        uint32_t elapsed = now_ms() - s_last_cmd_ms;
        if (elapsed > DRIVE_WATCHDOG_MS) {
            ESP_LOGW(TAG, "Command watchdog expired (%lu ms) — disarming",
                     (unsigned long)elapsed);
            s_armed       = false;
            s_watchdog_ok = false;
            ctrl_drive_emergency_stop();
            return;
        }
    }

    if (!s_armed) {
        if (s_current_left != 0.0f || s_current_right != 0.0f) {
            s_current_left  = 0.0f;
            s_current_right = 0.0f;
            o_motors_stop();
        }
        return;
    }

    /* Convert fixed-point snapshot to float for mixing math.
     * Read each int16_t once into a local — the 16-bit load is atomic. */
    float x = (float)s_target_x_fp * (1.0f / AXES_SCALE);
    float y = (float)s_target_y_fp * (1.0f / AXES_SCALE);

    /* Arcade mixing */
    float want_left  = clampf(y + x, -1.0f, 1.0f);
    float want_right = clampf(y - x, -1.0f, 1.0f);

    /* Advance ramp */
    s_current_left  = ramp(s_current_left,  want_left);
    s_current_right = ramp(s_current_right, want_right);

    if (fabsf(s_current_left) < 0.005f && fabsf(s_current_right) < 0.005f) {
        o_motors_stop();
    } else {
        o_motors_drive(s_current_left, s_current_right);
    }
}

void ctrl_drive_emergency_stop(void)
{
    s_target_x_fp   = 0;
    s_target_y_fp   = 0;
    s_current_left  = 0.0f;
    s_current_right = 0.0f;
    o_motors_stop();
    ESP_LOGW(TAG, "Emergency stop");
}
