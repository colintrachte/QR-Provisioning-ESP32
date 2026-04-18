/**
 * shared/drive_mixer.h — Portable arcade-drive mixer and command parser.
 *
 * Pure C99, zero SDK dependencies. Included by both the ESP32-S3 (ESP-IDF)
 * and ESP8266 (Arduino) targets so the mixing math and command grammar stay
 * identical on both platforms.
 *
 * Mixer sign convention
 * ─────────────────────
 * The caller supplies DRIVE_LEFT_SIGN and DRIVE_RIGHT_SIGN before including
 * this header (or accepts the defaults below). This handles the common case
 * where one motor is physically wired in reverse relative to the other, which
 * changes the sign of that track's contribution without changing the math.
 *
 *   Default (S3 wiring):  left = +(y + x),  right = +(y - x)
 *   D1 Mini wiring:       left = -(y - x),  right = +(y + x)
 *     → achieved by #define DRIVE_LEFT_SIGN  -1
 *                   #define DRIVE_RIGHT_SIGN  1
 *
 * Usage
 * ─────
 *   #define DRIVE_LEFT_SIGN  -1     // if needed
 *   #define DRIVE_RIGHT_SIGN  1
 *   #include "drive_mixer.h"
 *
 *   drive_out_t out = drive_mix(x, y);
 *   // out.left and out.right are in [-1.0, +1.0]
 *
 *   drive_cmd_t cmd;
 *   if (drive_parse(msg, &cmd)) { ... }
 */

#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifndef DRIVE_LEFT_SIGN
#define DRIVE_LEFT_SIGN   1
#endif
#ifndef DRIVE_RIGHT_SIGN
#define DRIVE_RIGHT_SIGN  1
#endif

/* ── Types ──────────────────────────────────────────────────────────────────*/

typedef struct {
    float left;   /* [-1.0, +1.0] */
    float right;
} drive_out_t;

typedef enum {
    DRIVE_CMD_NONE,
    DRIVE_CMD_AXES,   /* x/y move command */
    DRIVE_CMD_STOP,
    DRIVE_CMD_PING,
    DRIVE_CMD_ARM,
    DRIVE_CMD_DISARM,
    DRIVE_CMD_LED,
} drive_cmd_type_t;

typedef struct {
    drive_cmd_type_t type;
    float x;
    float y;
    float led;    /* 0.0–1.0, valid when type == DRIVE_CMD_LED */
} drive_cmd_t;

/* ── Helpers ────────────────────────────────────────────────────────────────*/

static inline float drive_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ── Mixer ──────────────────────────────────────────────────────────────────
 * Inputs are post-deadzone, post-expo values in [-1, +1].
 * Outputs are clamped to [-1, +1] and sign-adjusted per wiring convention. */
static inline drive_out_t drive_mix(float x, float y)
{
    drive_out_t out;
    out.left  = drive_clampf((float)DRIVE_LEFT_SIGN  * (y + x), -1.0f, 1.0f);
    out.right = drive_clampf((float)DRIVE_RIGHT_SIGN * (y - x), -1.0f, 1.0f);
    return out;
}

/* ── Command parser ─────────────────────────────────────────────────────────
 * Parses the WebSocket text protocol shared by all targets.
 * Returns 1 on success, 0 if the message is unrecognised.
 *
 * Recognised messages:
 *   "x:F,y:F"   → DRIVE_CMD_AXES   (F = float, e.g. "x:0.500,y:-1.000")
 *   "stop"      → DRIVE_CMD_STOP
 *   "ping"      → DRIVE_CMD_PING
 *   "arm"       → DRIVE_CMD_ARM
 *   "disarm"    → DRIVE_CMD_DISARM
 *   "led:F"     → DRIVE_CMD_LED
 */
static inline int drive_parse(const char *msg, drive_cmd_t *out)
{
    if (!msg || !out) return 0;
    out->x = out->y = out->led = 0.0f;

    if (strcmp(msg, "stop")   == 0) { out->type = DRIVE_CMD_STOP;   return 1; }
    if (strcmp(msg, "ping")   == 0) { out->type = DRIVE_CMD_PING;   return 1; }
    if (strcmp(msg, "arm")    == 0) { out->type = DRIVE_CMD_ARM;    return 1; }
    if (strcmp(msg, "disarm") == 0) { out->type = DRIVE_CMD_DISARM; return 1; }

    if (strncmp(msg, "led:", 4) == 0) {
        out->type = DRIVE_CMD_LED;
        out->led  = drive_clampf((float)atof(msg + 4), 0.0f, 1.0f);
        return 1;
    }

    /* "x:F,y:F" — two floats separated by a comma */
    if (strncmp(msg, "x:", 2) == 0) {
        const char *comma = strchr(msg, ',');
        if (comma && strncmp(comma + 1, "y:", 2) == 0) {
            out->type = DRIVE_CMD_AXES;
            out->x    = drive_clampf((float)atof(msg + 2),    -1.0f, 1.0f);
            out->y    = drive_clampf((float)atof(comma + 3),  -1.0f, 1.0f);
            return 1;
        }
    }

    out->type = DRIVE_CMD_NONE;
    return 0;
}
