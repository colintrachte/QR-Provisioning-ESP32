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
    vTaskDelay(pdMS_TO_TICKS(I2C_POST_WIFI_DELAY_MS));  // 100 ms S3, 250 ms classic

    /* ── Bus lifecycle ─────────────────────────────────────────────────
     * We are the sole owner of the I2C master bus. After esp_wifi_start()
     * the peripheral is reset; the old handle is stale. Delete it fully
     * before creating a new one, or classic ESP32 will give you
     * ESP_ERR_INVALID_STATE on every transaction.
     * ─────────────────────────────────────────────────────────────────*/
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = I2C_NUM_0,
        .sda_io_num    = DISP_PIN_SDA,   // from board.h
        .scl_io_num    = DISP_PIN_SCL,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return; /* Don't call display_set_available here — main.c will handle it */
    }

    run_scan();

    /* Fallback logic unchanged... */
    if (!s_periph_map.oled && err == ESP_OK) {
        ESP_LOGW(TAG, "Scan missed OLED, but bus OK — forcing present");
        s_periph_map.oled = true;
    }

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
