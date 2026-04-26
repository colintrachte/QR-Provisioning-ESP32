/**
 * i_sensors.c — I2C sensor bus owner.
 *
 * I2C re-init timing
 * ──────────────────
 * esp_wifi_start() resets the I2C peripheral on ESP32-S3 as a side-effect
 * of the radio clock tree bringup. i_sensors_init() runs after WiFi is up
 * and creates a fresh i2c_master_bus, then calls display_reinit_i2c() so
 * the display HAL gets new handles before the next flush.
 *
 * A 100 ms delay before re-init lets radio calibration finish, preventing
 * spurious probe timeouts.
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

#define I2C_PROBE_TIMEOUT_MS  50

static bool               s_initialized = false;
static i2c_master_bus_handle_t s_bus    = NULL;
static i_peripheral_map_t s_periph_map  = {0};
static i_sensor_data_t    s_data        = {
    .temperature_c = NAN,
    .humidity_pct  = NAN,
    .pressure_hpa  = NAN,
};

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

void i_sensors_init(void)
{
    ESP_LOGI(TAG, "Installing I2C master bus (post-WiFi)");

    memset(&s_periph_map, 0, sizeof(s_periph_map));

    /* Let WiFi radio calibration finish before probing */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Reset existing bus if valid */
    if (s_bus) {
        i2c_master_bus_reset(s_bus);
        s_bus = NULL;
    }

    esp_err_t err = display_reinit_i2c();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display_reinit_i2c failed: %s — OLED may be absent",
                 esp_err_to_name(err));
    }

    s_bus = u8g2_esp32_hal_get_bus();
    if (!s_bus) {
        ESP_LOGE(TAG, "I2C bus not available — sensors unavailable");
        display_set_available(false);
        return;
    }

    run_scan();

    /* Fallback: probe can spuriously fail while u8g2 transmit works */
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
    /* Poll registered sensor drivers here as hardware is added */
}

const i_peripheral_map_t *i_sensors_get_peripheral_map(void)
{
    return &s_periph_map;
}

i_sensor_data_t i_sensors_get_data(void)
{
    return s_data;
}

i2c_master_bus_handle_t i_sensors_get_bus(void)
{
    return s_bus;
}
