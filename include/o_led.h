#pragma once
/**
 * o_led.h — Onboard white LED output driver (GPIO35, Heltec V3).
 *
 * Uses LEDC for PWM brightness control. The LED is a simple white indicator,
 * not RGB. Brightness maps linearly to duty cycle.
 *
 * Blink patterns run on a FreeRTOS timer — o_led_blink() returns immediately.
 * A call to o_led_set() cancels any active blink pattern.
 *
 * Thread safety: all public functions are safe to call from any task.
 */

#include <stdbool.h>
#include "esp_err.h"

/* ── Blink patterns ─────────────────────────────────────────────────────────
 * Each pattern is a short human-readable sequence. The LED driver cycles
 * through the pattern until replaced or cancelled.
 */
typedef enum {
    LED_PATTERN_OFF,          /* Solid off                                   */
    LED_PATTERN_ON,           /* Solid on at full brightness                 */
    LED_PATTERN_SLOW_BLINK,   /* 1 Hz — "waiting / idle"                    */
    LED_PATTERN_FAST_BLINK,   /* 5 Hz — "activity / connecting"             */
    LED_PATTERN_DOUBLE_BLINK, /* Two quick pulses then pause — "error"       */
    LED_PATTERN_HEARTBEAT,    /* Long on, short off, long on — "running"     */
} led_pattern_t;

/**
 * Initialise LEDC peripheral and configure GPIO35.
 * Call once at boot before any other o_led_* function.
 * @return ESP_OK or an esp_err_t on LEDC configuration failure.
 */
esp_err_t o_led_init(void);

/**
 * Set LED brightness immediately. Cancels any active blink pattern.
 * @param brightness  0.0 = off, 1.0 = full brightness. Clamped to [0, 1].
 */
void o_led_set(float brightness);

/**
 * Start a non-blocking blink pattern. Replaces any pattern currently running.
 * @param pattern  One of the LED_PATTERN_* values.
 */
void o_led_blink(led_pattern_t pattern);
