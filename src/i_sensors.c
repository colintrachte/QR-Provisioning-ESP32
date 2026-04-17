/**
 * i_sensors.c — I2C sensor bus owner.
 *
 * I2C re-init timing
 * ──────────────────
 * esp_wifi_start() (called inside wifi_manager_start()) resets the I2C
 * peripheral on ESP32-S3 as a side-effect of the radio clock tree bringup.
 *
 * With the new driver/i2c_master.h API the crash mode changes: instead of
 * the legacy driver going stale silently, i2c_master_transmit() returns
 * ESP_ERR_INVALID_STATE because the underlying peripheral was reset.
 *
 * i_sensors_init() is therefore the single point of I2C installation after
 * WiFi is up.  It:
 *   1. Creates a fresh i2c_master_bus (the old one is implicitly dead).
 *   2. Calls display_reinit_i2c() → u8g2_esp32_hal_reinit_bus() so the
 *      display HAL gets new bus/device handles before the next flush.
 *   3. Scans the bus using i2c_master_probe().
 *   4. Calls display_set_available() with the scan result.
 *
 * No delay hacks are needed: the new driver manages handle lifetimes
 * explicitly — there is no global state left behind by WiFi.
 *
 * Thread safety
 * ─────────────
 * i_sensors_init() runs from app_main before app_task is spawned.
 * i_sensors_tick() runs from app_task, the only writer of s_data.
 * i_sensors_get_data() returns a snapshot copy — no lock needed.
 */

#include "i_sensors.h"
#include "display.h"
#include "config.h"
#include "u8g2_esp32_hal.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i_sensors";

/* Timeout for i2c_master_probe() per address. Keep short — we scan 0x08..0x77. */
#define I2C_PROBE_TIMEOUT_MS  100

static bool               s_initialized = false;
static i2c_master_bus_handle_t s_bus    = NULL;
static i_peripheral_map_t s_periph_map  = {0};
static i_sensor_data_t    s_data        = {
    .temperature_c = NAN,
    .humidity_pct  = NAN,
    .pressure_hpa  = NAN,
};

/* ── Bus scan ───────────────────────────────────────────────────────────────*/

static void run_scan(void)
{
    ESP_LOGI(TAG, "+-- I2C scan (I2C_NUM_0, post-WiFi) --------------------");
    int found = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_probe(s_bus, addr,
                                         pdMS_TO_TICKS(I2C_PROBE_TIMEOUT_MS));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "| 0x%02X  FOUND", addr);
            found++;

            if (addr == (DISP_I2C_ADDR >> 1))
                s_periph_map.oled = true;
        } else if (DEBUG_SENSORS) {
            ESP_LOGD(TAG, "| 0x%02X  --", addr);
        }
    }

    ESP_LOGI(TAG, "| %d device(s) found", found);
    ESP_LOGI(TAG, "+--------------------------------------------------------");

    if (s_periph_map.oled)
        ESP_LOGI(TAG, "OLED confirmed at 0x%02X", DISP_I2C_ADDR >> 1);
    else
        ESP_LOGW(TAG, "OLED not found at 0x%02X", DISP_I2C_ADDR >> 1);
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

void i_sensors_init(void)
{
    ESP_LOGI(TAG, "Installing I2C master bus (post-WiFi)");

    memset(&s_periph_map, 0, sizeof(s_periph_map));

    /* 1. Let the display HAL recreate its bus (frees old acquisition). */
    esp_err_t err = display_reinit_i2c();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display_reinit_i2c failed: %s — OLED may be absent",
                 esp_err_to_name(err));
    }

    /* 2. Borrow the HAL's bus handle. */
    s_bus = u8g2_esp32_hal_get_bus();
    if (!s_bus) {
        ESP_LOGE(TAG, "I2C bus not available — sensors unavailable");
        display_set_available(false);
        return;
    }

    /* 3. Run the scan for other peripherals. */
    run_scan();

    /* 4. CRITICAL FALLBACK: i2c_master_probe() can spuriously fail on
     *    a post-WiFi bus while i2c_master_transmit() (used by u8g2) works.
     *    If the scan missed the OLED but display reinit succeeded, force it. */
    if (!s_periph_map.oled && err == ESP_OK) {
        ESP_LOGW(TAG, "Scan missed OLED, but display reinit OK — forcing present");
        s_periph_map.oled = true;
    }

    display_set_available(s_periph_map.oled);

    s_initialized = true;
    ESP_LOGI(TAG, "i_sensors_init OK (bus handle %p)", (void *)s_bus);
}

void i_sensors_tick(void)
{
    if (!s_initialized) return;

    /* Poll registered sensor drivers and update s_data.
     * Call each driver's read function here on its own interval.
     *
     * Example (with a BME280 driver):
     *   static uint32_t last_env_ms = 0;
     *   uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
     *   if (now - last_env_ms >= 2000) {
     *       last_env_ms = now;
     *       bme280_read(&s_data.temperature_c,
     *                   &s_data.humidity_pct,
     *                   &s_data.pressure_hpa);
     *   }
     */
}

const i_peripheral_map_t *i_sensors_get_peripheral_map(void)
{
    return &s_periph_map;
}

i_sensor_data_t i_sensors_get_data(void)
{
    return s_data;   /* snapshot copy — no lock needed (single writer) */
}

/**
 * Expose the shared bus handle for sensor drivers that need to add
 * their own device.  Returns NULL if i_sensors_init() has not run yet
 * or if bus creation failed.
 */
i2c_master_bus_handle_t i_sensors_get_bus(void)
{
    return s_bus;
}
