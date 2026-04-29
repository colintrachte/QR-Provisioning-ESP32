/*
 * u8g2_esp32_hal.c
 *
 * Migrated from legacy driver/i2c.h to driver/i2c_master.h (ESP-IDF 5.x).
 *
 * I2C transfer model change
 * ─────────────────────────
 * The old HAL built an i2c_cmd_link transaction byte-by-byte during
 * U8X8_MSG_BYTE_SEND, then fired it in one shot at U8X8_MSG_BYTE_END_TRANSFER
 * via i2c_master_cmd_begin().  That API is gone in the new driver.
 *
 * The new model:
 *   START_TRANSFER  — record the target I2C address; reset tx buffer index.
 *   BYTE_SEND       — append bytes to s_tx_buf[].
 *   END_TRANSFER    — call i2c_master_transmit(dev, s_tx_buf, len, timeout).
 *
 * The tx buffer is statically allocated (U8G2_I2C_TX_BUF_LEN bytes).
 * A full SSD1306 frame flush is 1 + 1024 bytes — well within the limit.
 *
 * Bus/device lifecycle
 * ────────────────────
 * U8X8_MSG_BYTE_INIT  — create bus + device (first display_init() call).
 * u8g2_esp32_hal_reinit_bus() — delete and recreate bus + device handles
 *                               after WiFi resets the I2C peripheral.
 *                               Called from display_reinit_i2c().
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "u8g2_esp32_hal.h"

static const char *TAG = "u8g2_hal";
static uint8_t s_i2c_addr7 = 0x3C;   /* learned during BYTE_INIT */
/* Timeout passed to i2c_master_transmit(). 1 s is generous for SSD1306. */
static const int I2C_TIMEOUT_MS = 1000;

/* ── Module state ───────────────────────────────────────────────────────────*/
static spi_device_handle_t       s_spi_dev  = NULL;
static i2c_master_bus_handle_t   s_i2c_bus  = NULL;
static i2c_master_dev_handle_t   s_i2c_dev  = NULL;
static u8g2_esp32_hal_t          s_hal      = {0};

/* Accumulation buffer for one I2C transaction (START → END). */
static uint8_t  s_tx_buf[U8G2_I2C_TX_BUF_LEN];
static uint16_t s_tx_len = 0;

/* ── Internal: create bus + device from current s_hal settings ──────────────*/

static esp_err_t i2c_bus_create(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = s_hal.i2c_port,
        .sda_io_num    = s_hal.sda,
        .scl_io_num    = s_hal.scl,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t i2c_dev_add(uint8_t dev_addr_7bit)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = dev_addr_7bit,
        .scl_speed_hz    = s_hal.i2c_clk_speed,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ── Public: store HAL parameters ───────────────────────────────────────────*/

void u8g2_esp32_hal_init(u8g2_esp32_hal_t u8g2_esp32_hal_param)
{
    s_hal = u8g2_esp32_hal_param;
}
void u8g2_esp32_hal_release_bus(void)
{
    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
    }
    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
    ESP_LOGI(TAG, "HAL bus released for handoff to i_sensors");
}
/* ── Public: HAL reinit after WiFi resets I2C peripheral ───────────────────*/
esp_err_t u8g2_esp32_hal_reinit_bus(i2c_master_bus_handle_t new_bus)
{
    if (s_hal.sda == U8G2_ESP32_HAL_UNDEFINED ||
        s_hal.scl == U8G2_ESP32_HAL_UNDEFINED) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
    }

    // If the HAL created its own temporary bus at boot, delete it now.
    // Ownership transfers to i_sensors permanently after this point.
    if (s_i2c_bus && s_i2c_bus != new_bus) {
        i2c_del_master_bus(s_i2c_bus);
    }

    s_i2c_bus = new_bus;

    esp_err_t err = i2c_dev_add(s_i2c_addr7);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "I2C device re-attached (addr=0x%02X)", s_i2c_addr7);
    return ESP_OK;
}

/* ── SPI callback (unchanged logic, kept for completeness) ──────────────────*/

uint8_t u8g2_esp32_spi_byte_cb(u8x8_t *u8x8,
                                uint8_t msg,
                                uint8_t arg_int,
                                void   *arg_ptr)
{
    ESP_LOGD(TAG, "spi_byte_cb: msg=%d arg_int=%d", msg, arg_int);

    switch (msg) {
    case U8X8_MSG_BYTE_SET_DC:
        if (s_hal.dc != U8G2_ESP32_HAL_UNDEFINED)
            gpio_set_level(s_hal.dc, arg_int);
        break;

    case U8X8_MSG_BYTE_INIT: {
        if (s_hal.clk  == U8G2_ESP32_HAL_UNDEFINED ||
            s_hal.mosi == U8G2_ESP32_HAL_UNDEFINED ||
            s_hal.cs   == U8G2_ESP32_HAL_UNDEFINED) break;

        spi_bus_config_t bus_cfg = {
            .sclk_io_num     = s_hal.clk,
            .mosi_io_num     = s_hal.mosi,
            .miso_io_num     = -1,
            .quadwp_io_num   = -1,
            .quadhd_io_num   = -1,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

        spi_device_interface_config_t dev_cfg = {
            .clock_speed_hz = 10000,
            .spics_io_num   = s_hal.cs,
            .queue_size     = 200,
        };
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi_dev));
        break;
    }

    case U8X8_MSG_BYTE_SEND: {
        spi_transaction_t t = {
            .length    = 8 * arg_int,
            .tx_buffer = arg_ptr,
        };
        ESP_ERROR_CHECK(spi_device_transmit(s_spi_dev, &t));
        break;
    }
    }
    return 0;
}

/* ── I2C callback ───────────────────────────────────────────────────────────*/

uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8,
                                uint8_t msg,
                                uint8_t arg_int,
                                void   *arg_ptr)
{
    ESP_LOGD(TAG, "i2c_cb: msg=%d arg_int=%d", msg, arg_int);

    switch (msg) {

    case U8X8_MSG_BYTE_INIT: {
        if (s_hal.sda == U8G2_ESP32_HAL_UNDEFINED ||
            s_hal.scl == U8G2_ESP32_HAL_UNDEFINED) {
            break;
        }

        // Do NOT call i2c_bus_create() here. At boot, display_init() runs
        // before i_sensors_init(), so there's no shared bus yet. We create
        // a temporary owned bus only if no external bus has been injected.
        // After i_sensors_init() runs, u8g2_esp32_hal_reinit_bus() will
        // replace it with the shared handle.
        if (!s_i2c_bus) {
            i2c_bus_create();  // temporary; will be replaced by reinit_bus()
        }

        uint8_t addr7 = u8x8_GetI2CAddress(u8x8) >> 1;
        s_i2c_addr7 = addr7;

        if (!s_i2c_dev) {
            i2c_dev_add(addr7);
        }
        break;
    }

    case U8X8_MSG_BYTE_SET_DC:
        /* Not used in I2C mode — no D/C line. */
        break;

    case U8X8_MSG_BYTE_START_TRANSFER:
        /* Begin accumulating bytes for this transaction. */
        s_tx_len = 0;
        break;

    case U8X8_MSG_BYTE_SEND: {
        /* Append incoming bytes to the accumulation buffer. */
        uint8_t *src   = (uint8_t *)arg_ptr;
        uint16_t count = (uint16_t)arg_int;

        if (s_tx_len + count > U8G2_I2C_TX_BUF_LEN) {
            ESP_LOGE(TAG, "BYTE_SEND: tx buffer overflow (%u + %u > %u) — "
                     "increase U8G2_I2C_TX_BUF_LEN",
                     s_tx_len, count, U8G2_I2C_TX_BUF_LEN);
            /* Truncate rather than corrupt memory. */
            count = U8G2_I2C_TX_BUF_LEN - s_tx_len;
        }

        memcpy(s_tx_buf + s_tx_len, src, count);
        s_tx_len += count;
        break;
    }

    case U8X8_MSG_BYTE_END_TRANSFER: {
        /* Fire the accumulated buffer as a single i2c_master_transmit() call. */
        if (!s_i2c_dev) {
            ESP_LOGE(TAG, "END_TRANSFER: no device handle — bus not initialised");
            break;
        }
        if (s_tx_len == 0) break;

        esp_err_t err = i2c_master_transmit(s_i2c_dev,
                                            s_tx_buf, s_tx_len,
                                            I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_master_transmit failed (%u bytes): %s",
                     s_tx_len, esp_err_to_name(err));
        }
        s_tx_len = 0;
        break;
    }
    }
    return 0;
}

/* ── GPIO / delay callback (unchanged) ─────────────────────────────────────*/

uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8,
                                      uint8_t msg,
                                      uint8_t arg_int,
                                      void   *arg_ptr)
{
    ESP_LOGD(TAG, "gpio_delay_cb: msg=%d arg_int=%d", msg, arg_int);

    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT: {
        uint64_t bitmask = 0;
        if (s_hal.dc    != U8G2_ESP32_HAL_UNDEFINED) bitmask |= 1ULL << s_hal.dc;
        if (s_hal.reset != U8G2_ESP32_HAL_UNDEFINED) bitmask |= 1ULL << s_hal.reset;
        if (s_hal.cs    != U8G2_ESP32_HAL_UNDEFINED) bitmask |= 1ULL << s_hal.cs;
        if (bitmask == 0) break;

        gpio_config_t gc = {
            .pin_bit_mask = bitmask,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&gc);
        break;
    }

    case U8X8_MSG_GPIO_RESET:
        if (s_hal.reset != U8G2_ESP32_HAL_UNDEFINED)
            gpio_set_level(s_hal.reset, arg_int);
        break;

    case U8X8_MSG_GPIO_CS:
        if (s_hal.cs != U8G2_ESP32_HAL_UNDEFINED)
            gpio_set_level(s_hal.cs, arg_int);
        break;

    case U8X8_MSG_GPIO_I2C_CLOCK:
        if (s_hal.scl != U8G2_ESP32_HAL_UNDEFINED)
            gpio_set_level(s_hal.scl, arg_int);
        break;

    case U8X8_MSG_GPIO_I2C_DATA:
        if (s_hal.sda != U8G2_ESP32_HAL_UNDEFINED)
            gpio_set_level(s_hal.sda, arg_int);
        break;

    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;
    }
    return 0;
}

i2c_master_bus_handle_t u8g2_esp32_hal_get_bus(void)
{
    return s_i2c_bus;
}
