/**
 * o_motors.c — Brushed DC motor output driver.
 *
 * Topology is selected at compile time by MOTOR_DRIVER_MODE in config.h.
 * Only the relevant init and drive functions are compiled in; the unused
 * topology's pin constants are ignored by the compiler.
 *
 * LEDC notes:
 *   Motor channels use MOTOR_LEDC_TIMER (timer index 1) and 10-bit resolution
 *   (1024 steps). Channel 0 is reserved for the onboard LED.
 *   On BTN8982 mode, four LEDC channels are used: L_IN1, L_IN2, R_IN1, R_IN2.
 *   On DIR_PWM_EN mode, two channels are used: L_PWM, R_PWM.
 */

#include "o_motors.h"
#include "config.h"

#include <math.h>
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "o_motors";

#define DUTY_MAX  ((1 << 10) - 1)   /* 1023 for 10-bit LEDC */

static bool s_initialized = false;
static bool s_enabled     = true;

/* ── Shared helpers ─────────────────────────────────────────────────────────*/

static void gpio_out(int pin, int level)
{
    if (pin < 0) return;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, level);
}

static void ledc_ch_set(ledc_channel_t ch, uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

static esp_err_t ledc_ch_init(ledc_channel_t ch, int gpio, ledc_timer_t timer)
{
    ledc_channel_config_t cfg = {
        .gpio_num   = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = ch,
        .timer_sel  = timer,
        .duty       = 0,
        .hpoint     = 0,
    };
    return ledc_channel_config(&cfg);
}

static esp_err_t init_ledc_timer(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = MOTOR_LEDC_RESOLUTION,
        .timer_num       = MOTOR_LEDC_TIMER,
        .freq_hz         = MOTOR_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    return ledc_timer_config(&timer);
}

/* ── DIR/PWM/EN topology ────────────────────────────────────────────────────*/
#if MOTOR_DRIVER_MODE == MOTOR_MODE_DIR_PWM_EN

static esp_err_t init_dir_pwm_en(void)
{
    esp_err_t err = init_ledc_timer();
    if (err != ESP_OK) return err;

    err = ledc_ch_init(MOTOR_L_LEDC_CHANNEL, MOTOR_L_PWM_GPIO, MOTOR_LEDC_TIMER);
    if (err != ESP_OK) return err;
    err = ledc_ch_init(MOTOR_R_LEDC_CHANNEL, MOTOR_R_PWM_GPIO, MOTOR_LEDC_TIMER);
    if (err != ESP_OK) return err;

    gpio_out(MOTOR_L_DIR_GPIO, 0);
    gpio_out(MOTOR_R_DIR_GPIO, 0);
    gpio_out(MOTOR_L_EN_GPIO,  0);   /* disabled until first drive call */
    gpio_out(MOTOR_R_EN_GPIO,  0);
    return ESP_OK;
}

static void drive_dir_pwm_en(float left, float right)
{
    /* Left */
    gpio_set_level(MOTOR_L_DIR_GPIO, left >= 0.0f ? 1 : 0);
    gpio_set_level(MOTOR_L_EN_GPIO,  1);
    ledc_ch_set(MOTOR_L_LEDC_CHANNEL, (uint32_t)(fabsf(left)  * DUTY_MAX));

    /* Right */
    gpio_set_level(MOTOR_R_DIR_GPIO, right >= 0.0f ? 1 : 0);
    gpio_set_level(MOTOR_R_EN_GPIO,  1);
    ledc_ch_set(MOTOR_R_LEDC_CHANNEL, (uint32_t)(fabsf(right) * DUTY_MAX));
}

static void stop_dir_pwm_en(void)
{
    ledc_ch_set(MOTOR_L_LEDC_CHANNEL, 0);
    ledc_ch_set(MOTOR_R_LEDC_CHANNEL, 0);
    gpio_set_level(MOTOR_L_EN_GPIO, 0);
    gpio_set_level(MOTOR_R_EN_GPIO, 0);
}

static void enable_dir_pwm_en(bool en)
{
    gpio_set_level(MOTOR_L_EN_GPIO, en ? 1 : 0);
    gpio_set_level(MOTOR_R_EN_GPIO, en ? 1 : 0);
    if (!en) {
        ledc_ch_set(MOTOR_L_LEDC_CHANNEL, 0);
        ledc_ch_set(MOTOR_R_LEDC_CHANNEL, 0);
    }
}

/* ── BTN8982 topology ───────────────────────────────────────────────────────*/
#elif MOTOR_DRIVER_MODE == MOTOR_MODE_BTN8982

/* Four LEDC channels: repurpose channel defines from config.h and add two more.
 * L_IN1 = MOTOR_L_LEDC_CHANNEL, L_IN2 = MOTOR_L_LEDC_CHANNEL + 2,
 * R_IN1 = MOTOR_R_LEDC_CHANNEL, R_IN2 = MOTOR_R_LEDC_CHANNEL + 2.
 * Channels 1,2,3,4 are used total. */
#define CH_L_IN1  MOTOR_L_LEDC_CHANNEL       /* channel 1 */
#define CH_L_IN2  (MOTOR_L_LEDC_CHANNEL + 2) /* channel 3 */
#define CH_R_IN1  MOTOR_R_LEDC_CHANNEL       /* channel 2 */
#define CH_R_IN2  (MOTOR_R_LEDC_CHANNEL + 2) /* channel 4 */

static esp_err_t init_btn8982(void)
{
    esp_err_t err = init_ledc_timer();
    if (err != ESP_OK) return err;

    err = ledc_ch_init(CH_L_IN1, MOTOR_L_IN1_GPIO, MOTOR_LEDC_TIMER); if (err != ESP_OK) return err;
    err = ledc_ch_init(CH_L_IN2, MOTOR_L_IN2_GPIO, MOTOR_LEDC_TIMER); if (err != ESP_OK) return err;
    err = ledc_ch_init(CH_R_IN1, MOTOR_R_IN1_GPIO, MOTOR_LEDC_TIMER); if (err != ESP_OK) return err;
    err = ledc_ch_init(CH_R_IN2, MOTOR_R_IN2_GPIO, MOTOR_LEDC_TIMER); if (err != ESP_OK) return err;

    /* EN pins: assert high if connected */
    gpio_out(MOTOR_L_EN_BTN_GPIO, 1);
    gpio_out(MOTOR_R_EN_BTN_GPIO, 1);
    return ESP_OK;
}

static void drive_btn8982(float left, float right)
{
    uint32_t l_duty = (uint32_t)(fabsf(left)  * DUTY_MAX);
    uint32_t r_duty = (uint32_t)(fabsf(right) * DUTY_MAX);

    if (left >= 0.0f) {
        ledc_ch_set(CH_L_IN1, l_duty);
        ledc_ch_set(CH_L_IN2, 0);
    } else {
        ledc_ch_set(CH_L_IN1, 0);
        ledc_ch_set(CH_L_IN2, l_duty);
    }

    if (right >= 0.0f) {
        ledc_ch_set(CH_R_IN1, r_duty);
        ledc_ch_set(CH_R_IN2, 0);
    } else {
        ledc_ch_set(CH_R_IN1, 0);
        ledc_ch_set(CH_R_IN2, r_duty);
    }
}

static void stop_btn8982(void)
{
    /* IN1=0, IN2=0 = brake to GND on BTN8982 */
    ledc_ch_set(CH_L_IN1, 0); ledc_ch_set(CH_L_IN2, 0);
    ledc_ch_set(CH_R_IN1, 0); ledc_ch_set(CH_R_IN2, 0);
}

static void enable_btn8982(bool en)
{
    gpio_set_level(MOTOR_L_EN_BTN_GPIO, en ? 1 : 0);
    gpio_set_level(MOTOR_R_EN_BTN_GPIO, en ? 1 : 0);
    if (!en) stop_btn8982();
}

#else
#error "MOTOR_DRIVER_MODE must be MOTOR_MODE_DIR_PWM_EN or MOTOR_MODE_BTN8982"
#endif

/* ── Public API — topology-agnostic ────────────────────────────────────────*/

esp_err_t o_motors_init(void)
{
    esp_err_t err;
#if MOTOR_DRIVER_MODE == MOTOR_MODE_DIR_PWM_EN
    err = init_dir_pwm_en();
#else
    err = init_btn8982();
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Motor init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    s_enabled     = false;  /* explicitly disabled until first drive or enable call */
    ESP_LOGI(TAG, "Motor init OK (mode=%d)", MOTOR_DRIVER_MODE);
    return ESP_OK;
}

void o_motors_drive(float left, float right)
{
    if (!s_initialized) return;

    left  = left  < -1.0f ? -1.0f : left  > 1.0f ? 1.0f : left;
    right = right < -1.0f ? -1.0f : right > 1.0f ? 1.0f : right;

    s_enabled = true;
#if MOTOR_DRIVER_MODE == MOTOR_MODE_DIR_PWM_EN
    drive_dir_pwm_en(left, right);
#else
    drive_btn8982(left, right);
#endif

    if (DEBUG_MOTORS)
        ESP_LOGD(TAG, "drive L=%.3f R=%.3f", left, right);
}

void o_motors_stop(void)
{
    if (!s_initialized) return;
#if MOTOR_DRIVER_MODE == MOTOR_MODE_DIR_PWM_EN
    stop_dir_pwm_en();
#else
    stop_btn8982();
#endif
    s_enabled = false;
}

void o_motors_enable(bool en)
{
    if (!s_initialized) return;
    s_enabled = en;
#if MOTOR_DRIVER_MODE == MOTOR_MODE_DIR_PWM_EN
    enable_dir_pwm_en(en);
#else
    enable_btn8982(en);
#endif
}
