#pragma once
#ifndef U8G2_ESP32_HAL_H
#define U8G2_ESP32_HAL_H
/*
 * u8g2_esp32_hal.h
 *
 * Migrated from legacy driver/i2c.h to driver/i2c_master.h (ESP-IDF 5.x).
 *
 * Key changes from the original kolban HAL:
 *  - i2c_port_t / i2c_clk_speed replaced by i2c_master_bus_handle_t and
 *    i2c_master_dev_handle_t, which are managed entirely within the HAL.
 *  - U8X8_MSG_BYTE_INIT creates the master bus + device.
 *    u8g2_esp32_hal_reinit_bus() recreates them after WiFi resets the
 *    peripheral — call this instead of re-running the full u8g2_InitDisplay().
 *  - The streaming SEND model (byte-by-byte write into a cmd_link) is
 *    replaced by an accumulation buffer flushed on END_TRANSFER, which
 *    matches the new API's single-call i2c_master_transmit() contract.
 *  - i2c_cmd_link_create/delete and i2c_master_cmd_begin are gone.
 */

#include "u8g2.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define U8G2_ESP32_HAL_UNDEFINED  (-1)

/* Default I2C parameters — override in the hal struct before calling
 * u8g2_esp32_hal_init() if your board differs. */
#define I2C_MASTER_NUM            I2C_NUM_0
#define I2C_MASTER_FREQ_HZ        400000   /* 400 kHz — SSD1306 max ~1 MHz */

/* Maximum bytes accumulated between START_TRANSFER and END_TRANSFER.
 * SSD1306 full-frame flush: 1 cmd byte + 1024 data bytes = 1025.
 * 1088 gives comfortable headroom. */
#define U8G2_I2C_TX_BUF_LEN      1088

typedef struct {
    gpio_num_t clk;           /* SPI clock (unused for I2C) */
    gpio_num_t mosi;          /* SPI MOSI  (unused for I2C) */
    gpio_num_t sda;           /* I2C data  */
    gpio_num_t scl;           /* I2C clock */
    gpio_num_t cs;            /* SPI CS    (unused for I2C) */
    gpio_num_t reset;         /* optional hardware reset line */
    gpio_num_t dc;            /* SPI D/C   (unused for I2C) */

    /* New API: port number and speed used when (re-)creating the bus.
     * The live handles are kept internally — callers do not touch them. */
    i2c_port_num_t i2c_port;
    uint32_t       i2c_clk_speed;
} u8g2_esp32_hal_t;

#define U8G2_ESP32_HAL_DEFAULT  \
  {                             \
    U8G2_ESP32_HAL_UNDEFINED,   \
    U8G2_ESP32_HAL_UNDEFINED,   \
    U8G2_ESP32_HAL_UNDEFINED,   \
    U8G2_ESP32_HAL_UNDEFINED,   \
    U8G2_ESP32_HAL_UNDEFINED,   \
    U8G2_ESP32_HAL_UNDEFINED,   \
    U8G2_ESP32_HAL_UNDEFINED,   \
    I2C_MASTER_NUM,             \
    I2C_MASTER_FREQ_HZ          \
  }

/**
 * Store HAL parameters. Does NOT install any driver — the driver is
 * installed lazily on the first U8X8_MSG_BYTE_INIT callback, or
 * explicitly via u8g2_esp32_hal_reinit_bus().
 */
void u8g2_esp32_hal_init(u8g2_esp32_hal_t u8g2_esp32_hal_param);

i2c_master_bus_handle_t u8g2_esp32_hal_get_bus(void);

/**
 * Tear down and recreate the I2C master bus + device handles.
 *
 * Call this after i_sensors_init() has re-installed the I2C driver
 * post-WiFi startup.  It does NOT re-send the SSD1306 init sequence —
 * the panel retains its state.  display_reinit_i2c() calls this.
 *
 * @return ESP_OK on success, propagated esp_err_t otherwise.
 */
/* components/u8g2_hal/include/u8g2_esp32_hal.h */
esp_err_t u8g2_esp32_hal_reinit_bus(i2c_master_bus_handle_t new_bus);

/* u8g2 callback signatures (unchanged — registered with u8g2_Setup_*). */
uint8_t u8g2_esp32_spi_byte_cb(u8x8_t *u8x8, uint8_t msg,
                                uint8_t arg_int, void *arg_ptr);
uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg,
                                uint8_t arg_int, void *arg_ptr);
uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg,
                                      uint8_t arg_int, void *arg_ptr);

#ifdef __cplusplus
}
#endif

#endif /* U8G2_ESP32_HAL_H */
