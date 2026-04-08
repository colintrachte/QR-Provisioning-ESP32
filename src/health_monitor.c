/**
 * health_monitor.c — System health monitoring.
 *
 * I2C scan
 * ────────
 *  Probes every address in the 7-bit I2C space (0x08 – 0x77) by attempting
 *  a zero-byte write.  If the bus ACKs, the device is present.  The scan
 *  result for DISP_I2C_ADDR is used to set display_set_available() so
 *  display.c switches to graceful no-op mode if the OLED is missing.
 *
 * RSSI monitoring
 * ───────────────
 *  Calls esp_wifi_sta_get_ap_info() every HEALTH_SCAN_INTERVAL_MS while
 *  the STA is connected.  Logs a warning if RSSI < HEALTH_RSSI_WARN_DBM.
 *
 * Debug flags
 * ───────────
 *  Controlled by DEBUG_HEALTH in config.h.  Set to 0 to suppress verbose
 *  ESP_LOGD output without touching this file.
 */

#include "health_monitor.h"
#include "display.h"
#include "wifi_manager.h"
#include "config.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "health";

/* ── State ──────────────────────────────────────────────────────────────────*/

static health_peripheral_map_t s_peripherals = {0};
static int8_t                  s_rssi        = 0;
static uint32_t                s_last_rssi_check_ms = 0;

/* ── I2C scan ────────────────────────────────────────────────────────────────
 * We reuse I2C_NUM_0 which is already configured by the u8g2 HAL in
 * display_init().  The HAL owns the bus initialisation; we just send probe
 * transactions.  If display_init() has not been called yet this will fail
 * gracefully — the i2c_master_write_to_device() call returns an error and
 * the device is marked absent.
 *
 * Note: scanning is done once at boot, not periodically, to avoid interfering
 * with the OLED during normal operation.
 */
#define I2C_SCAN_TIMEOUT_MS  10   /* short timeout per address */

/**
 * Probe one I2C address.  Returns true if the device ACKs.
 */
static bool i2c_probe(uint8_t addr_7bit)
{
    /* i2c_master_write_to_device sends a START + address + STOP.
     * A zero-byte payload is a valid probe — we just want the ACK/NACK. */
    uint8_t dummy = 0;
    esp_err_t err = i2c_master_write_to_device(
        I2C_NUM_0,
        addr_7bit,
        &dummy, 0,                          /* zero bytes to write */
        pdMS_TO_TICKS(I2C_SCAN_TIMEOUT_MS));

    return (err == ESP_OK);
}

static void run_i2c_scan(void)
{
    ESP_LOGI(TAG, "+-- I2C bus scan (I2C_NUM_0) ----------------------------");
    int found = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_probe(addr)) {
            ESP_LOGI(TAG, "| 0x%02X  FOUND", addr);
            found++;

            /* Map known addresses to peripheral flags. */
            if (addr == (DISP_I2C_ADDR >> 1)) {
                /* DISP_I2C_ADDR is already shifted (0x78 = 0x3C << 1).
                 * i2c_probe takes the 7-bit address (0x3C). */
                s_peripherals.oled = true;
            }
        } else {
            if (DEBUG_HEALTH) {
                ESP_LOGD(TAG, "| 0x%02X  --", addr);
            }
        }
    }

    ESP_LOGI(TAG, "| %d device(s) found", found);
    ESP_LOGI(TAG, "+--------------------------------------------------------");

    /* Inform display.c of the authoritative hardware-detected result. */
    display_set_available(s_peripherals.oled);

    if (!s_peripherals.oled) {
        ESP_LOGW(TAG, "OLED not detected at 0x%02X — display calls will be no-ops",
                 DISP_I2C_ADDR >> 1);
    } else {
        ESP_LOGI(TAG, "OLED confirmed at 0x%02X", DISP_I2C_ADDR >> 1);
    }
}

/* ── RSSI check ──────────────────────────────────────────────────────────────*/

static void check_rssi(void)
{
    if (!wifi_manager_is_connected()) return;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return;

    s_rssi = ap_info.rssi;

    if (s_rssi < HEALTH_RSSI_WARN_DBM) {
        ESP_LOGW(TAG, "Weak WiFi signal: %d dBm (threshold: %d dBm)",
                 s_rssi, HEALTH_RSSI_WARN_DBM);
    } else {
        if (DEBUG_HEALTH) {
            ESP_LOGD(TAG, "RSSI: %d dBm  (ok)", s_rssi);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────────*/

void health_monitor_init(void)
{
    ESP_LOGI(TAG, "health_monitor_init: scanning peripherals");
    run_i2c_scan();

    ESP_LOGI(TAG, "Peripheral map — OLED: %s",
             s_peripherals.oled ? "PRESENT" : "ABSENT");
}

void health_monitor_tick(void)
{
    /* Throttle RSSI checks to HEALTH_SCAN_INTERVAL_MS. */
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now_ms - s_last_rssi_check_ms >= (uint32_t)HEALTH_SCAN_INTERVAL_MS) {
        s_last_rssi_check_ms = now_ms;
        check_rssi();
    }
}

const health_peripheral_map_t *health_monitor_get_peripherals(void)
{
    return &s_peripherals;
}

int8_t health_monitor_get_rssi(void)
{
    return s_rssi;
}
