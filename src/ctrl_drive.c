/**
 * ctrl_drive.c — Differential drive controller.
 *
 * Mixing: arcade drive.
 *   left  = y + x
 *   right = y - x
 * Both outputs clamped to [-1, +1] after mixing.
 *
 * Ramp limiting (drive_ramp_rate)
 * ────────────────────────────────
 * Prevents instantaneous jumps in motor current when a joystick snaps to
 * full deflection or zero. On brushed DC motors with no current limiting,
 * large sudden current spikes can trigger the driver IC's protection and
 * cause a brief stall. Rate is read from settings_get()->drive_ramp_rate
 * on every tick so changes take effect immediately without a reboot.
 *
 * Command watchdog
 * ────────────────
 * ctrl_drive_feed_watchdog() must be called each time a valid drive command
 * is received from the WebSocket handler. ctrl_drive_tick() checks elapsed
 * time; if no command arrives within settings_get()->drive_watchdog_ms the
 * controller disarms itself and calls ctrl_drive_emergency_stop().
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
 *
 * Settings read points
 * ────────────────────
 * drive_deadband    — read in ctrl_drive_set_axes() on every WS frame.
 * drive_ramp_rate   — read in ramp() on every ctrl_drive_tick() call.
 * drive_max_duty    — read in ctrl_drive_tick() before output clamp.
 * drive_watchdog_ms — read in ctrl_drive_tick() watchdog check.
 * All four are const* reads with no locking needed (settings_get() is
 * safe for concurrent readers; saves are mutex-protected inside settings_mgr).
 */

#include "ctrl_drive.h"
#include "o_motors.h"
#include "o_led.h"
#include "settings_mgr.h"
#include "app_server.h"
#include "config.h"

#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ctrl_drive";

/* ── Watchdog ───────────────────────────────────────────────────────────────
 * s_last_cmd_ms is written by the WS task, read by app_task.
 * uint32_t write on ESP32-S3 is atomic; volatile prevents register caching.
 * Timeout threshold is read from settings_get()->drive_watchdog_ms each tick. */
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
    /* Read ramp rate from settings on every call — takes effect immediately
     * when the user changes it via /api/settings without requiring a reboot. */
    float max = settings_get()->drive_ramp_rate;
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
    ESP_LOGI(TAG, "ctrl_drive_init OK (watchdog=%lu ms, deadband=%.3f, ramp=%.3f, max_duty=%.3f)",
             (unsigned long)settings_get()->drive_watchdog_ms,
             settings_get()->drive_deadband,
             settings_get()->drive_ramp_rate,
             settings_get()->drive_max_duty);
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

    /* Deadband: read from settings so it can be tuned at runtime without
     * reflashing. Do NOT rescale here — rescaling is done client-side so the
     * firmware sees a fully-processed value. If rescaling were done both
     * places the curve would be applied twice. */
    float deadband = settings_get()->drive_deadband;
    float fx = (fabsf(x) < deadband) ? 0.0f : x;
    float fy = (fabsf(y) < deadband) ? 0.0f : y;

    /* Convert to fixed-point for atomic storage. int16_t write is a single
     * instruction on Xtensa LX7, avoiding the torn-read race that exists with
     * direct float stores across task boundaries. */
    s_target_x_fp = (int16_t)(fx * AXES_SCALE);
    s_target_y_fp = (int16_t)(fy * AXES_SCALE);
}

void ctrl_drive_set_armed(bool armed)
{
    s_armed = armed;
    if (!s_armed) {
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
    static int log_divider = 0;

    ESP_LOGD(TAG, "Target X FP: %d", s_target_x_fp);
    ESP_LOGD(TAG, "Target Y FP: %d", s_target_y_fp);

    /* ── Watchdog check ──────────────────────────────────────────────────── */
    if (s_armed && s_watchdog_ok) {
        uint32_t elapsed     = now_ms() - s_last_cmd_ms;
        uint32_t watchdog_ms = settings_get()->drive_watchdog_ms;
        if (elapsed > watchdog_ms) {
            ESP_LOGW(TAG, "Command watchdog expired (%lu ms > %lu ms) — disarming",
                     (unsigned long)elapsed, (unsigned long)watchdog_ms);
            s_armed       = false;
            s_watchdog_ok = false;
            ctrl_drive_emergency_stop();
            o_led_blink(LED_PATTERN_FAST_BLINK);
            app_server_push_arm_state(false, "watchdog");
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

    /* Apply drive_max_duty: read from settings so it takes effect immediately
     * when changed via the setup wizard or /api/settings, same pattern as
     * drive_ramp_rate in ramp(). No locking needed — see thread-safety note. */
    float max_duty   = settings_get()->drive_max_duty;
    want_left        = clampf(want_left,  -max_duty, max_duty);
    want_right       = clampf(want_right, -max_duty, max_duty);

    /* Advance ramp */
    if (log_divider++ % 10 == 0) {
        ESP_LOGD(TAG, "System is %s", s_armed ? "ARMED" : "DISARMED");
        ESP_LOGI(TAG, "Want L: %f", want_left);
        ESP_LOGI(TAG, "Want R: %f", want_right);
    }

    s_current_left  = ramp(s_current_left,  want_left);
    s_current_right = ramp(s_current_right, want_right);

    if (fabsf(s_current_left) < 0.005f && fabsf(s_current_right) < 0.005f) {
        o_motors_stop();
    } else {
        o_motors_drive(s_current_left, s_current_right);
    }
}

/**
 * @brief Read the current ramped motor outputs.
 *
 * Returns the values after ramp limiting and clamping, i.e. what is
 * currently being sent to o_motors_drive(). Useful for UI feedback.
 *
 * @param[out] left   Current left motor output, -1.0..1.0
 * @param[out] right  Current right motor output, -1.0..1.0
 */
void ctrl_drive_get_outputs(float *left, float *right)
{
    if (left)  *left  = s_current_left;
    if (right) *right = s_current_right;
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
