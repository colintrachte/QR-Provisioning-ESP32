/**
 * i_sensors.c — I2C sensor bus owner.
 *
 * Bus lifecycle
 * ─────────────
 * i_sensors_init() is the sole owner of I2C_NUM_0. It must run after
 * wifi_manager_start() because esp_wifi_start() resets the I2C peripheral
 * as a side-effect of radio clock tree bringup.
 *
 * A delay of I2C_POST_WIFI_DELAY_MS before bus creation lets radio
 * calibration finish, preventing spurious transaction failures.
 *
 * Before creating its bus, i_sensors_init() calls
 * u8g2_esp32_hal_release_bus() to tear down the temporary bus the display
 * HAL created at boot. Ownership then transfers permanently to i_sensors.
 * main.c calls display_reinit_i2c() afterward to re-attach the display HAL
 * as a device on the new shared bus.
 *
 * OLED presence
 * ─────────────
 * Address-only probes (i2c_master_probe) are unreliable for the SSD1306 —
 * it does not ACK them consistently post-WiFi. OLED presence is therefore
 * determined by display_reinit_i2c() in main.c, which uses a real
 * transaction. Do not set s_periph_map.oled here.
 *
 * Adding sensors
 * ──────────────
 * Add device handles and probe/init logic below the bus creation block.
 * Use i2c_master_bus_add_device() with s_bus. Poll in i_sensors_tick().
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

static bool                    s_initialized = false;
static i2c_master_bus_handle_t s_bus         = NULL;
static i_peripheral_map_t      s_periph_map  = {0};
static i_sensor_data_t         s_data        = {
    .temperature_c = NAN,
    .humidity_pct  = NAN,
    .pressure_hpa  = NAN,
};

void i_sensors_init(void)
{
    ESP_LOGI(TAG, "Installing I2C master bus (post-WiFi)");
    memset(&s_periph_map, 0, sizeof(s_periph_map));

    vTaskDelay(pdMS_TO_TICKS(I2C_POST_WIFI_DELAY_MS));

    /* Release the HAL's temporary boot bus so we can acquire port 0.
     * From this point until display_reinit_i2c() runs in main.c,
     * display draw calls are no-ops (s_available is false). */
    u8g2_esp32_hal_release_bus();

    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port             = I2C_NUM_0,
        .sda_io_num           = DISP_PIN_SDA,
        .scl_io_num           = DISP_PIN_SCL,
        .clk_source           = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt    = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return;
    }

    /* ── Future sensors: add device handles here ────────────────────────
     * Example:
     *   i2c_device_config_t bme_cfg = {
     *       .dev_addr_length = I2C_ADDR_BIT_LEN_7,
     *       .device_address  = 0x76,
     *       .scl_speed_hz    = 400000,
     *   };
     *   i2c_master_bus_add_device(s_bus, &bme_cfg, &s_bme_dev);
     *   s_periph_map.bme280 = (err == ESP_OK);
     * ─────────────────────────────────────────────────────────────────*/

    s_initialized = true;
    ESP_LOGI(TAG, "i_sensors_init OK (bus handle %p)", (void *)s_bus);
}

void i_sensors_tick(void)
{
    if (!s_initialized) return;
    /* Poll registered sensor drivers here as hardware is added. */
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
