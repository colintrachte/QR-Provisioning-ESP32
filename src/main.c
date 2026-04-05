/**
 * main.c — Application entry point.
 *
 * Startup sequence
 * ────────────────
 *  1. display_init()           Power OLED, send init commands.
 *  2. prov_ui_show_boot()      Render boot splash (so the screen isn't blank
 *                              during the QR generation step below).
 *  3. prov_ui_init()           Pre-generate WiFi + URL QR codes (~50 ms,
 *                              CPU-bound; done before WiFi init on purpose).
 *  4. wifi_manager_start()     Try stored credentials; fall back to portal.
 *                              NVS init is handled internally.
 *  5. Main loop                Calls prov_ui_tick() to drive QR cycling.
 *                              Once connected, add your application logic here.
 *
 * Re-provision button
 * ───────────────────
 *  Hold GPIO0 (USER_SW / BOOT button on Heltec V3) for 3 s on power-up to
 *  erase stored credentials and force the portal to open.
 */

#include "display.h"
#include "qr_gen.h"
#include "wifi_manager.h"
#include "prov_ui.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/* ── SoftAP credentials ─────────────────────────────────────────────────
 * Change these for your device.  Password must be ≥8 chars or set to ""
 * for an open (unencrypted) network.
 */
#define AP_SSID     "RobotSetup"
#define AP_PASSWORD "robot1234"

/* ── Re-provision button ────────────────────────────────────────────────
 * GPIO0 = BOOT/USER_SW on Heltec V3.  Active-low (pulled up internally).
 */
#define REPROV_GPIO     0
#define REPROV_HOLD_MS  3000

static bool reprovision_requested(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << REPROV_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    if (gpio_get_level(REPROV_GPIO) != 0) return false;  /* button not held */

    ESP_LOGI(TAG, "BOOT held — keep holding %d ms to re-provision", REPROV_HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(REPROV_HOLD_MS));
    return (gpio_get_level(REPROV_GPIO) == 0);  /* still held → confirmed */
}

/* ── Application entry point ────────────────────────────────────────────*/

void app_main(void)
{
    ESP_LOGI(TAG, "=== WiFi Manager Demo ===");
    ESP_LOGI(TAG, "Board: Heltec WiFi LoRa 32 V3 (ESP32-S3)");

    /* 1. Display must come up first so every subsequent step can show status. */
    display_init();

    /* 2. Boot splash — gives immediate visual feedback while QRs are generated. */
    prov_ui_show_boot();

    /* 3. QR generation (CPU-bound) — do before WiFi init to keep startup clean. */
    prov_ui_init(AP_SSID, AP_PASSWORD);

    /* 4. Re-provision check — erase NVS credentials if button held. */
    if (reprovision_requested()) {
        ESP_LOGW(TAG, "Re-provision confirmed — erasing credentials");
        /* wifi_manager_erase_credentials() handles its own NVS init. */
        wifi_manager_erase_credentials();
        display_clear();
        display_draw_text(4, 24, "Credentials erased.", DISP_FONT_NORMAL);
        display_draw_text(4, 36, "Restarting...",       DISP_FONT_NORMAL);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    /* 5. Start WiFi manager. NVS init is done inside wifi_manager_start(). */
    wifi_manager_config_t cfg = WIFI_MANAGER_CONFIG_DEFAULT();
    cfg.ap_ssid         = AP_SSID;
    cfg.ap_password     = AP_PASSWORD;
    cfg.sta_max_retries = 5;
    cfg.on_state_change = prov_ui_on_state_change;
    cfg.on_connected    = prov_ui_on_connected;

    esp_err_t err = wifi_manager_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_start failed: %s", esp_err_to_name(err));
        display_clear();
        display_draw_text(4, 20, "WiFi init failed!", DISP_FONT_NORMAL);
        display_draw_text(4, 32, esp_err_to_name(err), DISP_FONT_SMALL);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    /* 6. Main loop — prov_ui_tick() drives the QR cycling animation. */
    ESP_LOGI(TAG, "Entering main loop");
    while (1) {
        prov_ui_tick();  /* non-blocking; only redraws when cycling timer fires */

        if (wifi_manager_is_connected()) {
            /* ── Your application code goes here ──────────────────────
             * The success screen persists until you call display_clear().
             * Example:
             *   sensor_read_and_publish();
             *   robot_drive_tick();
             */
        }

        vTaskDelay(pdMS_TO_TICKS(100));  /* 10 Hz — adequate for QR cycling */
    }
}
