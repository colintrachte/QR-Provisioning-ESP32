#pragma once
/**
 * i_sensors.h — I2C sensor bus owner and peripheral scanner.
 *
 * This module owns I2C_NUM_0 after WiFi startup. It must be initialised
 * AFTER wifi_manager_start() returns because WiFi init on ESP32-S3 resets
 * the I2C peripheral as a side-effect of radio bringup.
 *
 * I2C API
 * ───────
 * Uses driver/i2c_master.h (ESP-IDF 5.x) exclusively. The legacy
 * driver/i2c.h is not used. Bus and device handles are managed here and
 * shared with sensor drivers via i_sensors_get_bus().
 *
 * Responsibilities:
 *   1. Create a fresh i2c_master_bus post-WiFi and call display_reinit_i2c()
 *      so the OLED HAL gets consistent handles.
 *   2. Scan 0x08–0x77 with i2c_master_probe() and build a peripheral map.
 *   3. Tick registered sensor drivers and update a shared snapshot struct
 *      that health_monitor and app_server read for telemetry.
 *
 * Adding a sensor:
 *   Implement a driver that accepts an i2c_master_bus_handle_t in its _init()
 *   call and calls i2c_master_bus_add_device() internally.
 *   Register it in i_sensors_init() with an address check against the scan.
 *   i_sensors_tick() calls the read function on a configurable interval.
 *
 * Thread safety:
 *   i_sensors_get_data() returns a snapshot copy — no lock needed by callers.
 *   The internal tick runs from app_task, the only writer.
 */

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

/* ── Peripheral presence map ────────────────────────────────────────────────
 * Populated by i_sensors_init(). Read-only after init.
 */
typedef struct {
    bool oled;     /* SSD1306 at 0x3C */
    /* Add peripheral flags here as hardware expands, e.g.:
     * bool imu;      // MPU-6050 at 0x68
     * bool tof;      // VL53L0X at 0x29
     * bool env;      // BME280 at 0x76 or 0x77
     */
} i_peripheral_map_t;

/* ── Sensor data snapshot ───────────────────────────────────────────────────
 * Written by i_sensors_tick(). Read by health_monitor and app_server.
 * Extend as sensors are added.
 */
typedef struct {
    float    temperature_c;  /* NaN if no sensor present */
    float    humidity_pct;   /* NaN if no sensor present */
    float    pressure_hpa;   /* NaN if no sensor present */
    /* IMU, ToF, etc. go here */
} i_sensor_data_t;

/**
 * Initialise the I2C master bus, scan all addresses, and build the peripheral map.
 * Calls display_reinit_i2c() so the OLED HAL uses the re-created bus handles.
 *
 * MUST be called after wifi_manager_start() returns.
 * MUST be called after display_init() (display_reinit_i2c requires the mutex).
 */
void i_sensors_init(void);

/**
 * Periodic tick — call from app_task alongside ctrl_drive_tick().
 * Polls registered sensor drivers on their configured intervals.
 * Does nothing if not yet initialised.
 */
void i_sensors_tick(void);

/** Return the peripheral presence map populated by i_sensors_init(). */
const i_peripheral_map_t *i_sensors_get_peripheral_map(void);

/**
 * Return a snapshot copy of the latest sensor readings.
 * Safe to call from any task after i_sensors_init().
 */
i_sensor_data_t i_sensors_get_data(void);

/**
 * Return the shared i2c_master_bus_handle_t for sensor drivers that need
 * to call i2c_master_bus_add_device() themselves.
 * Returns NULL if i_sensors_init() has not yet run or bus creation failed.
 */
i2c_master_bus_handle_t i_sensors_get_bus(void);
