/**
 * o_led.c — Onboard white LED driver for Heltec V3 (GPIO35).
 *
 * LEDC provides PWM brightness control. A FreeRTOS software timer fires
 * every 50 ms to advance blink patterns without blocking any task.
 *
 * Dual-core safety
 * ────────────────
 * s_pattern and s_step are written from the app/WS handler task and read
 * from the timer daemon task. On the dual-core ESP32-S3 these must be
 * volatile so the compiler does not cache them in registers across the
 * task boundary. Single 32-bit aligned writes on Xtensa LX7 are atomic,
 * so volatile is sufficient — no mutex needed.
 *
 * Write order in o_led_blink(): s_step is reset before s_pattern is updated.
 * The timer reads pattern then step. If the timer fires between the two
 * writes it sees the old pattern with step=0, which is a valid state for
 * any pattern. The reverse order would risk seeing new pattern + old step.
 */

#include "o_led.h"
#include "config.h"

#include "esp_log.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "o_led";

static bool                   s_initialized = false;
static TimerHandle_t          s_timer       = NULL;
static volatile led_pattern_t s_pattern     = LED_PATTERN_OFF;
static volatile int           s_step        = 0;

#define DUTY_MAX  ((1u << 8) - 1u)   /* 255 for 8-bit resolution */

static void set_duty(uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL);
}

/* Timer callback — 50 ms period.
 * 1 tick = 50 ms, 20 ticks = 1 s. */
static void pattern_tick(TimerHandle_t xTimer)
{
    (void)xTimer;
    led_pattern_t pat  = s_pattern;   /* read pattern once into a local */
    int           step = s_step;

    switch (pat) {
    case LED_PATTERN_OFF:
        set_duty(0);
        return;

    case LED_PATTERN_ON:
        set_duty(DUTY_MAX);
        return;

    case LED_PATTERN_SLOW_BLINK:
        /* 1 Hz: 10 ticks on, 10 ticks off */
        set_duty(step < 10 ? DUTY_MAX : 0);
        s_step = (step + 1) % 20;
        return;

    case LED_PATTERN_FAST_BLINK:
        /* 5 Hz: 2 ticks on, 2 ticks off */
        set_duty(step < 2 ? DUTY_MAX : 0);
        s_step = (step + 1) % 4;
        return;

    case LED_PATTERN_DOUBLE_BLINK:
        /* on, off, on, off, then dark — 20 tick cycle */
        if      (step < 2)  set_duty(DUTY_MAX);
        else if (step < 4)  set_duty(0);
        else if (step < 6)  set_duty(DUTY_MAX);
        else                set_duty(0);
        s_step = (step + 1) % 20;
        return;

    case LED_PATTERN_HEARTBEAT:
        /* Long on, dim pulse, off — 20 tick cycle */
        if      (step < 14) set_duty(DUTY_MAX);
        else if (step < 16) set_duty(0);
        else if (step < 18) set_duty(DUTY_MAX / 3);
        else                set_duty(0);
        s_step = (step + 1) % 20;
        return;
    }
}

esp_err_t o_led_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LED_LEDC_RESOLUTION,
        .timer_num       = LED_LEDC_TIMER,
        .freq_hz         = LED_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ch = {
        .gpio_num   = LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LED_LEDC_CHANNEL,
        .timer_sel  = LED_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_timer = xTimerCreate("led", pdMS_TO_TICKS(50), pdTRUE, NULL, pattern_tick);
    if (!s_timer) {
        ESP_LOGE(TAG, "Timer create failed");
        return ESP_ERR_NO_MEM;
    }
    xTimerStart(s_timer, 0);

    s_initialized = true;
    ESP_LOGI(TAG, "LED init OK (GPIO%d, %u Hz, 8-bit LEDC)",
             LED_GPIO, (unsigned)LED_LEDC_FREQ_HZ);
    return ESP_OK;
}

void o_led_set(float brightness)
{
    if (!s_initialized) return;
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    /* Reset step before setting pattern=OFF so timer never sees a stale step */
    s_step    = 0;
    s_pattern = LED_PATTERN_OFF;
    set_duty((uint32_t)(brightness * DUTY_MAX));
}

void o_led_blink(led_pattern_t pattern)
{
    if (!s_initialized) return;
    /* Reset step before updating pattern — see dual-core write-order note above */
    s_step    = 0;
    s_pattern = pattern;
}
