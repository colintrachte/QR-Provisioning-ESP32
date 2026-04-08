#pragma once
/**
 * config.h — Central compile-time configuration for the robot provisioning
 * firmware.
 *
 * Every magic number, pin assignment, timing constant, and debug toggle that
 * was previously scattered across source files lives here.  Individual modules
 * still own their own logic — this file is only for values that are tuned at
 * project level.
 *
 * To adjust for a different board, change the pin block.  To silence a noisy
 * subsystem during development, flip its DEBUG flag to 0.
 */

/* ── SoftAP credentials ─────────────────────────────────────────────────────
 * AP_SSID     : Name of the setup network the device broadcasts.
 * AP_PASSWORD : Must be ≥8 chars for WPA2.  Set to "" for an open network.
 */
#define AP_SSID     "RobotSetup"
#define AP_PASSWORD "robot1234"
#define AP_GW_IP "192.168.4.1"

/* ── Re-provisioning button ─────────────────────────────────────────────────
 * GPIO0 = BOOT / USER_SW on Heltec V3.  Active-low (internal pull-up).
 * REPROV_HOLD_MS: how long the button must be held to trigger a wipe.
 */
#define REPROV_GPIO    0
#define REPROV_HOLD_MS 3000

/* ── Main loop timing ───────────────────────────────────────────────────────
 * MAIN_LOOP_MS  : Base tick period.  10 Hz is adequate for UI cycling.
 *                 Motor control must run in its own higher-priority task.
 * WIFI_INIT_FAIL_DELAY_MS : How long to show the error screen before restart.
 * WIFI_MAX_RESTART_ATTEMPTS : Boot-loop guard — enter safe mode after this many
 *                             consecutive failed wifi_manager_start() calls.
 */
#define MAIN_LOOP_MS               100
#define WIFI_INIT_FAIL_DELAY_MS   10000
#define WIFI_MAX_RESTART_ATTEMPTS     3

/* ── Health monitor thresholds ──────────────────────────────────────────────
 * HEALTH_RSSI_WARN_DBM : RSSI below this triggers a "weak signal" warning.
 * HEALTH_SCAN_INTERVAL_MS : How often the health monitor polls RSSI.
 */
#define HEALTH_RSSI_WARN_DBM       -75
#define HEALTH_SCAN_INTERVAL_MS   5000

/* ── Debug category flags ───────────────────────────────────────────────────
 * Set to 1 to enable verbose ESP_LOGD output for that subsystem.
 * Set to 0 to suppress it without touching the source files.
 * These are evaluated at compile time — no runtime overhead when off.
 */
#define DEBUG_WIFI    1
#define DEBUG_DISPLAY 1
#define DEBUG_UI      1
#define DEBUG_HEALTH  1
#define DEBUG_MAIN    1
