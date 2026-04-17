#pragma once
/**
 * health_monitor.h — System health aggregator and telemetry builder.
 *
 * Previously this module owned I2C scanning and peripheral detection.
 * That responsibility now belongs to i_sensors.c, which re-inits the bus
 * correctly after WiFi startup.
 *
 * health_monitor now:
 *   1. Reads WiFi RSSI on HEALTH_SCAN_INTERVAL_MS cadence.
 *   2. Reads i_battery and i_sensors snapshots.
 *   3. Builds and caches the JSON telemetry blob that app_server pushes
 *      over WebSocket.
 *
 * Call health_monitor_init() after i_sensors_init().
 * Call health_monitor_tick() from the main loop at ~10 Hz.
 */

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialise the health monitor.
 * Caches the i_sensors peripheral map for logging. No I2C operations.
 */
void health_monitor_init(void);

/**
 * Periodic tick. Call from the main loop at ~10 Hz.
 * Polls RSSI on HEALTH_SCAN_INTERVAL_MS cadence.
 * Rebuilds the telemetry JSON blob every tick (it is cheap).
 */
void health_monitor_tick(void);

/**
 * Return the most recently measured RSSI in dBm, or 0 if not connected.
 */
int8_t health_monitor_get_rssi(void);

/**
 * Return a pointer to the cached telemetry JSON string.
 * Updated each tick. Safe to read between ticks — no partial writes.
 * The string is null-terminated and fits in HEALTH_JSON_BUF_LEN bytes.
 */
const char *health_monitor_get_telemetry_json(void);

#define HEALTH_JSON_BUF_LEN 256
