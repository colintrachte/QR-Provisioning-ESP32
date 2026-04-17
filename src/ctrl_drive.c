/**
 * ctrl_drive.c — Differential drive controller.
 *
 * Mixing: arcade drive.
 *   left  = y + x
 *   right = y - x
 * Both outputs clamped to [-1, +1] after mixing.
 *
 * The ramp limits DRIVE_MAX_DELTA_PER_TICK prevents instantaneous jumps in
 * motor current when a joystick snaps to full deflection or zero. On real
 * brushed DC motors with no current limiting, large sudden current spikes
 * can trigger the driver IC's protection and cause a brief stall.
 *
 * Thread safety: ctrl_drive_set_axes() and ctrl_drive_set_armed() use a
 * simple volatile read/write for the target axes, which is sufficient for
 * single float writes on ESP32-S3 (32-bit aligned, atomic on Xtensa LX7).
 * ctrl_drive_tick() is called from one task only.
 */

#include "ctrl_drive.h"
#include "o_motors.h"
#include "config.h"

#include <math.h>
#include "esp_log.h"

static const char *TAG = "ctrl_drive";

/* Target axes set by the WebSocket handler */
static volatile float s_target_x = 0.0f;
static volatile float s_target_y = 0.0f;
static volatile bool  s_armed    = false;

/* Current ramped outputs */
static float s_current_left  = 0.0f;
static float s_current_right = 0.0f;

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

/* ── Public API ─────────────────────────────────────────────────────────────*/

void ctrl_drive_init(void)
{
    o_motors_init();
    ESP_LOGI(TAG, "ctrl_drive_init OK");
}

void ctrl_drive_set_axes(float x, float y)
{
    /* Apply deadband before storing */
    s_target_x = (fabsf(x) < DRIVE_DEADBAND) ? 0.0f : x;
    s_target_y = (fabsf(y) < DRIVE_DEADBAND) ? 0.0f : y;
}

void ctrl_drive_set_armed(bool armed)
{
    s_armed = armed;
    if (!armed) {
        s_target_x = 0.0f;
        s_target_y = 0.0f;
    }
    ESP_LOGI(TAG, "%s", armed ? "ARMED" : "DISARMED");
}

bool ctrl_drive_is_armed(void)
{
    return s_armed;
}

void ctrl_drive_tick(void)
{
    if (!s_armed) {
        if (s_current_left != 0.0f || s_current_right != 0.0f) {
            s_current_left  = 0.0f;
            s_current_right = 0.0f;
            o_motors_stop();
        }
        return;
    }

    float x = s_target_x;
    float y = s_target_y;

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
    s_target_x      = 0.0f;
    s_target_y      = 0.0f;
    s_current_left  = 0.0f;
    s_current_right = 0.0f;
    o_motors_stop();
    ESP_LOGW(TAG, "Emergency stop");
}
