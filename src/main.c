/**
 * main.c — Application entry point.
 *
 * Startup sequence
 * ────────────────
 *  1. display_init()           Power OLED, send init commands.
 *  2. prov_ui_show_boot()      Render boot splash immediately.
 *  3. prov_ui_init()           Pre-generate WiFi + URL QR codes (~50 ms).
 *  4. reprovision check        Non-blocking GPIO poll over REPROV_HOLD_MS.
 *  5. wifi_manager_start()     Try stored credentials; fall back to portal.
 *  6. Main loop                Calls prov_ui_tick().  Once connected, spawns
 *                              a dedicated FreeRTOS app_task so heavy robot
 *                              logic never jitters the UI tick timing.
 *
 * Boot-loop guard
 * ───────────────
 *  A persistent retry counter in RTC memory tracks consecutive
 *  wifi_manager_start() failures.  After WIFI_MAX_RESTART_ATTEMPTS the
 *  firmware enters "safe mode": it stays on screen showing a diagnostic
 *  message rather than restarting indefinitely.
 *
 * Re-provision button
 * ───────────────────
 *  Hold GPIO0 (USER_SW / BOOT button on Heltec V3) for REPROV_HOLD_MS ms
 *  on power-up to erase stored credentials and force the portal to open.
 *  The check is non-blocking: the display and watchdog continue running
 *  during the hold window.
 */

#include "config.h"
#include "display.h"
#include "qr_gen.h"
#include "wifi_manager.h"
#include "prov_ui.h"
#include "health_monitor.h"

#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/* ── Boot-loop guard ────────────────────────────────────────────────────────
 * RTC_DATA_ATTR survives deep sleep and software resets but is cleared by
 * a power-on reset, which is exactly what we want: a power cycle resets
 * the counter while a crash-induced restart increments it.
 */
RTC_DATA_ATTR static int s_restart_count = 0;

/* ── Re-provision button ────────────────────────────────────────────────────
 * Non-blocking poll: samples the button every 50 ms for REPROV_HOLD_MS total.
 * The display and FreeRTOS tick continue running throughout.
 */
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

    if (gpio_get_level(REPROV_GPIO) != 0) {
        return false; /* button not held at all — fast path */
    }

    ESP_LOGI(TAG, "BOOT held — polling for %d ms to confirm re-provision", REPROV_HOLD_MS);

    const int poll_interval_ms = 50;
    const int polls = REPROV_HOLD_MS / poll_interval_ms;

    for (int i = 0; i < polls; i++) {
        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        if (gpio_get_level(REPROV_GPIO) != 0) {
            ESP_LOGI(TAG, "BOOT released early — re-provision cancelled");
            return false;
        }
    }

    ESP_LOGW(TAG, "Re-provision confirmed after %d ms hold", REPROV_HOLD_MS);
    return true;
}

/* ── Safe mode ──────────────────────────────────────────────────────────────
 * Called when wifi_manager_start() has failed WIFI_MAX_RESTART_ATTEMPTS times
 * in a row.  Displays a persistent diagnostic screen and halts so the user
 * can power-cycle or reflash rather than watch an infinite reboot loop.
 */
static void enter_safe_mode(const char *reason)
{
    ESP_LOGE(TAG, "SAFE MODE: %s (restart count=%d)", reason, s_restart_count);

    display_clear();
    display_draw_text(20,  0, "SAFE MODE",         DISP_FONT_BOLD);
    display_draw_hline(0, 14, DISP_WIDTH);
    display_draw_text( 2, 17, "WiFi init failed",  DISP_FONT_NORMAL);
    display_draw_text( 2, 29, reason,              DISP_FONT_SMALL);
    display_draw_hline(0, 44, DISP_WIDTH);
    display_draw_text( 2, 47, "Power cycle",       DISP_FONT_SMALL);
    display_draw_text( 2, 55, "to recover",        DISP_FONT_SMALL);
    display_flush();

    /* Halt here. The watchdog is not fed — a WDT reset would re-enter safe
     * mode anyway since s_restart_count persists.  Use an infinite delay so
     * FreeRTOS is not starved (other tasks, if any, keep running). */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* ── Application task ───────────────────────────────────────────────────────
 * Spawned once STA is connected.  Keeps heavy robot logic off the UI tick.
 * Replace the body with your robot_app_tick() or sensor_read() calls.
 */
static void app_task(void *arg)
{
    ESP_LOGI(TAG, "app_task started");
    (void)arg;

    while (1) {
        /* ── Your robot application code goes here ──────────────────────
         * This task runs at whatever rate your application needs.
         * The provisioning UI tick in app_main() runs independently.
         *
         * Examples:
         *   sensor_read_and_publish();
         *   motor_controller_tick();
         *   health_monitor_tick();    <- already called below; shown for
         *                               illustration only
         */
        vTaskDelay(pdMS_TO_TICKS(10)); /* 100 Hz application loop */
    }
}

/* ── Application entry point ────────────────────────────────────────────────*/

void app_main(void)
{
    ESP_LOGI(TAG, "=== Robot Provisioning Firmware ===");
    ESP_LOGI(TAG, "Board: Heltec WiFi LoRa 32 V3 (ESP32-S3)");
    ESP_LOGI(TAG, "Restart count (this power cycle): %d", s_restart_count);

    /* 1. Display first — every subsequent step can show status. */
    display_init();

    /* 2. Boot splash — immediate visual feedback. */
    prov_ui_show_boot();

    /* 3. QR pre-generation (CPU-bound, ~50 ms) before WiFi init. */
    prov_ui_init(AP_SSID, AP_PASSWORD);

    /* 4. Non-blocking re-provision check. */
    if (reprovision_requested()) {
        ESP_LOGW(TAG, "Re-provision confirmed — erasing credentials");
        wifi_manager_erase_credentials();

        display_clear();
        display_draw_text(4, 24, "Credentials erased.", DISP_FONT_NORMAL);
        display_draw_text(4, 36, "Restarting...",       DISP_FONT_NORMAL);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));

        /* A re-provision request is intentional — reset the boot-loop counter
         * so the subsequent restart does not count as a failure. */
        s_restart_count = 0;
        esp_restart();
    }

    /* 5. Boot-loop guard — enter safe mode if we have been restarting repeatedly. */
    if (s_restart_count >= WIFI_MAX_RESTART_ATTEMPTS) {
        enter_safe_mode("Too many failed starts");
        /* Does not return. */
    }

    /* 6. WiFi manager start. */
    wifi_manager_config_t cfg = WIFI_MANAGER_CONFIG_DEFAULT();
    cfg.ap_ssid         = AP_SSID;
    cfg.ap_password     = AP_PASSWORD;
    cfg.sta_max_retries = 5;
    cfg.on_state_change = prov_ui_on_state_change;
    cfg.on_connected    = prov_ui_on_connected;

    esp_err_t err = wifi_manager_start(&cfg);
    if (err != ESP_OK) {
        s_restart_count++;
        ESP_LOGE(TAG, "wifi_manager_start failed (%d/%d): %s",
                 s_restart_count, WIFI_MAX_RESTART_ATTEMPTS,
                 esp_err_to_name(err));

        display_clear();
        display_draw_text(4, 16, "WiFi init failed!",  DISP_FONT_NORMAL);
        display_draw_text(4, 28, esp_err_to_name(err), DISP_FONT_SMALL);
        display_draw_text(4, 42, "Restarting...",      DISP_FONT_SMALL);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(WIFI_INIT_FAIL_DELAY_MS));
        esp_restart();
    }

    /* Success — reset boot-loop counter. */
    s_restart_count = 0;

    /* 7. Initialise health monitor (I2C scan, peripheral map). */
    health_monitor_init();

    /* 8. Main loop — UI tick + health checks.  Motor control runs in app_task. */
    ESP_LOGI(TAG, "Entering main loop");

    bool app_task_spawned = false;

    while (1) {
        prov_ui_tick(); /* non-blocking; redraws only when state changes */

        health_monitor_tick(); /* RSSI checks, peripheral status, etc. */

        if (wifi_manager_is_connected() && !app_task_spawned) {
            ESP_LOGI(TAG, "STA connected — spawning app_task");
            xTaskCreate(app_task, "app_task", 4096, NULL, 3, NULL);
            app_task_spawned = true;
        }

        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_MS)); /* 10 Hz UI tick */
    }
}
