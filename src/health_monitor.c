/**
 * health_monitor.c — System health monitoring.
 *
 * I2C scan
 * ────────
 *  Probes every address in the 7-bit I2C space (0x08 – 0x77) by sending a
 *  zero-byte write.  If the bus ACKs, the device is present.
 *
 *  The u8g2 mkfrey HAL owns I2C_NUM_0 initialisation (called inside
 *  display_init()).  This module reuses that bus without re-initialising it —
 *  i2c_master_write_to_device() works on an already-configured port.  The
 *  probe must therefore run after display_init() has returned successfully.
 *
 *  The scan result for DISP_I2C_ADDR drives display_set_available() so the
 *  display driver switches to graceful no-op mode if the OLED is absent.
 *
 * RSSI monitoring
 * ───────────────
 *  Calls esp_wifi_sta_get_ap_info() every HEALTH_SCAN_INTERVAL_MS while
 *  the STA is connected.  Logs a warning when RSSI < HEALTH_RSSI_WARN_DBM.
 *
 * Debug flags
 * ───────────
 *  Controlled by DEBUG_HEALTH in config.h.
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
static health_peripheral_map_t s_peripherals       = {0};
static int8_t                  s_rssi              = 0;
static uint32_t                s_last_rssi_check_ms = 0;

/* ── I2C probe ───────────────────────────────────────────────────────────────
 * Short timeout keeps the scan fast; a missing device NACKs immediately.
 */
#define I2C_PROBE_TIMEOUT_MS 10

/**
 * Probe a single 7-bit I2C address.
 * @param addr_7bit  7-bit device address (not shifted).
 * @return true if the device ACKs.
 */
static bool i2c_probe(uint8_t addr_7bit)
{
    /* Zero-byte write: just START + addr + STOP.  The HAL has already
     * configured I2C_NUM_0; we piggyback on its initialised port here. */
    uint8_t dummy = 0;
    esp_err_t err = i2c_master_write_to_device(
        I2C_NUM_0,
        addr_7bit,
        &dummy, 0,
        pdMS_TO_TICKS(I2C_PROBE_TIMEOUT_MS));
    return (err == ESP_OK);
}

static void run_i2c_scan(void)
{
    ESP_LOGI(TAG, "+-- I2C bus scan (I2C_NUM_0) ----------------------------");
    int found = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++)
    {
        if (i2c_probe(addr))
        {
            ESP_LOGI(TAG, "| 0x%02X  FOUND", addr);
            found++;

            /* DISP_I2C_ADDR is the 8-bit form (0x3C << 1 = 0x78).
             * i2c_probe takes the 7-bit address (0x3C). */
            if (addr == (DISP_I2C_ADDR >> 1))
                s_peripherals.oled = true;
        }
        else if (DEBUG_HEALTH)
        {
            ESP_LOGD(TAG, "| 0x%02X  --", addr);
        }
    }

    ESP_LOGI(TAG, "| %d device(s) found", found);
    ESP_LOGI(TAG, "+--------------------------------------------------------");

    /* Tell display.c whether the OLED is physically present. */
    //this flag kills the display when wifi causes momentary disconnection. refactor.
    //display_set_available(s_peripherals.oled);

    if (!s_peripherals.oled)
        ESP_LOGW(TAG, "OLED not detected at 0x%02X — display calls will be no-ops",
                 DISP_I2C_ADDR >> 1);
    else
        ESP_LOGI(TAG, "OLED confirmed at 0x%02X", DISP_I2C_ADDR >> 1);
}

/* ── RSSI check ─────────────────────────────────────────────────────────────*/
static void check_rssi(void)
{
    if (!wifi_manager_is_connected()) return;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return;

    s_rssi = ap_info.rssi;

    if (s_rssi < HEALTH_RSSI_WARN_DBM)
        ESP_LOGW(TAG, "Weak WiFi signal: %d dBm (threshold: %d dBm)",
                 s_rssi, HEALTH_RSSI_WARN_DBM);
    else if (DEBUG_HEALTH)
        ESP_LOGD(TAG, "RSSI: %d dBm  (ok)", s_rssi);
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

void health_monitor_init(void)
{
    ESP_LOGI(TAG, "health_monitor_init: scanning I2C bus");
    run_i2c_scan();
    ESP_LOGI(TAG, "Peripheral map — OLED: %s",
             s_peripherals.oled ? "PRESENT" : "ABSENT");
}

void health_monitor_tick(void)
{
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now_ms - s_last_rssi_check_ms >= (uint32_t)HEALTH_SCAN_INTERVAL_MS)
    {
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
