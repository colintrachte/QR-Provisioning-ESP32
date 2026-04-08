#pragma once
/**
 * health_monitor.h — System health monitoring for the robot provisioning firmware.
 *
 * Responsibilities
 * ─────────────────
 *  1. I2C peripheral scan on boot — builds a presence map so the rest of the
 *     firmware knows what hardware is actually connected.  Results are cached
 *     and used to set display_set_available() for graceful OLED degradation.
 *
 *  2. RSSI monitoring while STA is connected — logs a warning and optionally
 *     updates the display status bar when WiFi signal is weak.
 *
 *  3. Serial telemetry — structured ESP_LOG output for every health event so
 *     problems are immediately visible in the monitor without guessing.
 *
 * Usage
 * ─────
 *  Call health_monitor_init() once after display_init() and wifi_manager_start().
 *  Call health_monitor_tick() from your main loop (10 Hz is fine).
 *
 * Debug output
 * ─────────────
 *  Controlled by DEBUG_HEALTH in config.h.  Set to 0 to suppress verbose
 *  ESP_LOGD output from this module.
 */

#include <stdbool.h>
#include <stdint.h>

/* ── Peripheral presence map ────────────────────────────────────────────────
 * Each flag is set to true if the device was detected on the I2C bus during
 * health_monitor_init().
 */
typedef struct {
    bool oled;      /**< SSD1306 at DISP_I2C_ADDR (0x3C)   */
    /* Add more peripherals here as the hardware expands. */
} health_peripheral_map_t;

/**
 * Initialise the health monitor.
 *
 * - Scans the I2C bus and populates the peripheral presence map.
 * - Calls display_set_available() based on whether the OLED was found.
 * - Logs the full scan results to the serial monitor.
 *
 * Must be called after display_init() so display_set_available() is safe.
 * Should be called after wifi_manager_start() so the I2C bus has had time
 * to settle (WiFi init takes ~200 ms and does not use I2C, but the delay
 * helps in practice).
 */
void health_monitor_init(void);

/**
 * Periodic tick — call from the main loop at ~10 Hz.
 *
 * - Checks WiFi RSSI every HEALTH_SCAN_INTERVAL_MS and logs a warning if
 *   signal is below HEALTH_RSSI_WARN_DBM (both defined in config.h).
 * - Placeholder for future battery voltage monitoring.
 */
void health_monitor_tick(void);

/**
 * Return a pointer to the peripheral presence map populated by
 * health_monitor_init().  The map is valid for the lifetime of the firmware.
 */
const health_peripheral_map_t *health_monitor_get_peripherals(void);

/**
 * Return the most recently measured RSSI in dBm, or 0 if not connected.
 * Updated each time health_monitor_tick() polls the WiFi driver.
 */
int8_t health_monitor_get_rssi(void);
