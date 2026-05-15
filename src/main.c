/**
 * main.c — Application entry point.
 *
 * Startup sequence
 * ────────────────
 *  1.  display_init()          Power OLED, send init commands.
 *  2.  o_led_init()            Configure LEDC on GPIO35.
 *  3.  i_battery_init()        Configure ADC1_CH0.
 *  4.  prov_ui_show_boot()     Render boot splash.
 *  5.  nvs_flash_init()        Must precede settings_load() and wifi_manager.
 *  6.  settings_load()         Load runtime tunables from NVS; apply defaults
 *                              if absent.
 *  7.  prov_ui_init()          Pre-generate WiFi + URL QR codes (~50 ms).
 *  8.  reprovision check       Non-blocking GPIO poll over REPROV_HOLD_MS.
 *  9.  ctrl_drive_init()       Init motor driver hardware.
 *  9b. TCP/IP stack init       esp_netif_init + event loop — must precede
 *                              app_server_start (httpd_start calls socket()).
 * 10.  app_server_start()      Start HTTP server on port 80. Always-on;
 *                              serves control page immediately. wifi_manager
 *                              will call enter/exit_provisioning_mode() as
 *                              needed — no second httpd_start() ever happens.
 * 11.  app_task spawn          Motor tick + telemetry push, Core 1.
 *                              Spawned here so it works on AP or STA.
 * 12.  udp_ctrl_start()        UDP control socket, port 4210, Core 1.
 * 13.  display_power(false)    Blank OLED during WiFi association to reduce
 *                              rail load and improve RSSI on initial connect.
 * 14.  wifi_manager_start()    Try stored credentials; fall back to portal.
 *                              wifi_manager calls app_server_enter/exit_
 *                              provisioning_mode() rather than starting its
 *                              own httpd.
 * 15.  i_sensors_init()        Re-init I2C post-WiFi; scan bus.
 * 16.  display_reinit_i2c()    Re-attach OLED HAL to fresh bus.
 * 17.  display_power(true)     Restore display.
 * 18.  health_monitor_init()
 * 19.  Main loop               prov_ui_tick(), health_monitor_tick(), 10 Hz.
 *
 * Boot-loop guard
 * ───────────────
 *  RTC_DATA_ATTR restart counter. After WIFI_MAX_RESTART_ATTEMPTS the
 *  firmware enters safe mode: shows a diagnostic screen and halts.
 *
 * Re-provision button
 * ───────────────────
 *  Hold GPIO0 for REPROV_HOLD_MS at power-up to erase stored credentials.
 */

#include "config.h"
#include "settings_mgr.h"
#include "display.h"
#include "qr_gen.h"
#include "wifi_manager.h"
#include "prov_ui.h"
#include "i_sensors.h"
#include "i_battery.h"
#include "o_led.h"
#include "ctrl_drive.h"
#include "health_monitor.h"
#include "app_server.h"
#include "udp_ctrl.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/* Survives deep sleep and software resets; cleared by power-on reset */
RTC_DATA_ATTR static int s_restart_count = 0;

/* ── Re-provision button ────────────────────────────────────────────────────*/

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

    if (gpio_get_level(REPROV_GPIO) != 0) return false;

    ESP_LOGI(TAG, "BOOT held — polling for %d ms", REPROV_HOLD_MS);
    o_led_blink(LED_PATTERN_FAST_BLINK);

    const int poll_ms = 50;
    for (int i = 0; i < REPROV_HOLD_MS / poll_ms; i++) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        if (gpio_get_level(REPROV_GPIO) != 0) {
            ESP_LOGI(TAG, "BOOT released early — cancelled");
            o_led_set(0.0f);
            return false;
        }
    }

    ESP_LOGW(TAG, "Re-provision confirmed");
    o_led_set(1.0f);
    return true;
}

/* ── Safe mode ──────────────────────────────────────────────────────────────*/

static void enter_safe_mode(const char *reason)
{
    esp_reset_reason_t rst = esp_reset_reason();
    ESP_LOGE(TAG, "SAFE MODE: %s (restart_count=%d, reset_reason=%d)",
             reason, s_restart_count, (int)rst);

    o_led_blink(LED_PATTERN_DOUBLE_BLINK);

    display_clear();
    display_draw_text(20,  0, "SAFE MODE",         DISP_FONT_BOLD);
    display_draw_hline(0, 14, DISP_WIDTH);
    display_draw_text( 2, 17, "WiFi init failed",  DISP_FONT_NORMAL);
    display_draw_text( 2, 29, reason,              DISP_FONT_SMALL);
    display_draw_hline(0, 44, DISP_WIDTH);
    display_draw_text( 2, 47, "Power cycle",       DISP_FONT_SMALL);

    char rst_str[20];
    snprintf(rst_str, sizeof(rst_str), "Rst:%s",
             rst == ESP_RST_PANIC    ? "PANIC" :
             rst == ESP_RST_INT_WDT  ? "IWD"   :
             rst == ESP_RST_TASK_WDT ? "TWD"   :
             rst == ESP_RST_WDT      ? "WD"    : "OTHER");
    display_draw_text( 2, 55, rst_str, DISP_FONT_SMALL);

    display_flush();
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}

/* ── Application task ───────────────────────────────────────────────────────
 * Spawned at boot (step 11) on Core 1 so motor control works regardless
 * of whether STA is connected.  Telemetry pushes are no-ops when no WS
 * clients are connected.
 */
static void app_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "app_task started");

    uint32_t last_telemetry_ms = 0;

    while (1) {
        ctrl_drive_tick();
        i_sensors_tick();

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - last_telemetry_ms >= settings_get()->telemetry_interval_ms) {
            last_telemetry_ms = now_ms;
            app_server_push_telemetry();
        }

        vTaskDelay(pdMS_TO_TICKS(10));   /* 100 Hz tick */
    }
}

static void on_radio_reset(void *ctx)
{
    (void)ctx;
    i2c_master_bus_handle_t bus = i_sensors_get_bus();
    if (bus) display_reinit_i2c(bus);
}

/* ── Entry point ────────────────────────────────────────────────────────────*/

void app_main(void)
{
    ESP_LOGI(TAG, BOARD_NAME);
    ESP_LOGI(TAG, "Restart count: %d", s_restart_count);
    esp_reset_reason_t rst = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d (%s)", (int)rst,
             rst == ESP_RST_POWERON   ? "POWERON"   :
             rst == ESP_RST_SW        ? "SW"        :
             rst == ESP_RST_PANIC     ? "PANIC"     :
             rst == ESP_RST_INT_WDT   ? "INT_WDT"   :
             rst == ESP_RST_TASK_WDT  ? "TASK_WDT"  :
             rst == ESP_RST_WDT       ? "WDT"       :
             rst == ESP_RST_DEEPSLEEP ? "DEEPSLEEP"  :
             rst == ESP_RST_BROWNOUT  ? "BROWNOUT"  :
             rst == ESP_RST_USB       ? "USB"       :
             rst == ESP_RST_JTAG      ? "JTAG"      : "UNKNOWN");

    /* 1. Display first — every subsequent step can show status. */
    display_init();

    /* 2. LED — reprovision check and safe mode need blink patterns. */
    o_led_init();
    o_led_blink(LED_PATTERN_SLOW_BLINK);

    /* 3. Battery ADC — safe before WiFi. */
    i_battery_init();

    /* 4. UI splash. */
    prov_ui_show_boot();

    /* 5. NVS. */
    {
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS corrupt — erasing and reinitialising");
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_err = nvs_flash_init();
        }
        if (nvs_err != ESP_OK)
            ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(nvs_err));
    }

    /* 6. Settings. */
    {
        esp_err_t sload = settings_load();
        if (sload == ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGI(TAG, "No saved settings — using defaults");
        else if (sload != ESP_OK)
            ESP_LOGW(TAG, "settings_load: %s — defaults in use",
                     esp_err_to_name(sload));
    }

    /* 7. QR pre-generation. */
    prov_ui_init(settings_get()->ap_ssid, settings_get()->ap_password);

    /* 8. Re-provision check. */
    if (reprovision_requested()) {
        wifi_manager_erase_credentials();

        display_clear();
        display_draw_text(4, 24, "Credentials erased.", DISP_FONT_NORMAL);
        display_draw_text(4, 36, "Restarting...",       DISP_FONT_NORMAL);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));

        s_restart_count = 0;
        esp_restart();
    }

    /* Boot-loop guard. */
    if (s_restart_count >= WIFI_MAX_RESTART_ATTEMPTS) {
        enter_safe_mode("Too many failed starts");
        /* Does not return. */
    }

    /* 9. Motor driver — hardware init only, no WiFi dependency. */
    ctrl_drive_init();

    /* 9b. TCP/IP stack — must be up before httpd_start() (called inside
     *     app_server_start).  wifi_manager_start() also calls these but
     *     it now runs after app_server_start(); initialising here first
     *     is safe because both functions guard against double-init via
     *     the ESP_ERR_INVALID_STATE return value. */
    {
        esp_err_t ni_err = esp_netif_init();
        if (ni_err != ESP_OK && ni_err != ESP_ERR_INVALID_STATE)
            ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(ni_err));

        esp_err_t el_err = esp_event_loop_create_default();
        if (el_err != ESP_OK && el_err != ESP_ERR_INVALID_STATE)
            ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(el_err));
    }

    /* 10. HTTP server on port 80 — always-on, works on AP and STA.
     *     wifi_manager will call enter/exit_provisioning_mode() as needed. */
    {
        esp_err_t srv_err = app_server_start();
        if (srv_err != ESP_OK) {
            ESP_LOGE(TAG, "app_server_start failed: %s — continuing",
                     esp_err_to_name(srv_err));
            /* Non-fatal: robot still works; log and proceed. */
        }
    }

    /* 11. app_task — motor tick + telemetry, Core 1.
     *     Spawned now so control page works immediately over the AP,
     *     before STA connects.  telemetry push is a no-op until a WS
     *     client connects so no waste when unconnected. */
    xTaskCreatePinnedToCore(app_task, "app_task", 4096, NULL, 5, NULL, 1);

    /* 12. UDP control socket — binds INADDR_ANY, works on AP or STA. */
    udp_ctrl_start();

    /* 13. Blank OLED during WiFi association to reduce I2C/display current
     *     draw on the shared power rail, improving radio sensitivity.
     *     The display stays powered; only the pixel driver is put to sleep. */
    display_power(false);
    o_led_blink(LED_PATTERN_FAST_BLINK);

    /* 14. WiFi — spawns mgr_task, returns immediately.
     *     mgr_task calls portal_start() → app_server_enter_provisioning_mode()
     *     if stored credentials are absent or fail. */
    wifi_manager_config_t cfg = WIFI_MANAGER_CONFIG_DEFAULT();
    cfg.ap_ssid         = settings_get()->ap_ssid;
    cfg.ap_password     = settings_get()->ap_password;
    cfg.sta_max_retries = 3;
    cfg.on_state_change = prov_ui_on_state_change;
    cfg.on_connected    = prov_ui_on_connected;
    cfg.on_radio_reset  = on_radio_reset;

    esp_err_t err = wifi_manager_start(&cfg);
    if (err != ESP_OK) {
        s_restart_count++;
        ESP_LOGE(TAG, "wifi_manager_start failed (%d/%d): %s",
                 s_restart_count, WIFI_MAX_RESTART_ATTEMPTS,
                 esp_err_to_name(err));

        o_led_blink(LED_PATTERN_DOUBLE_BLINK);
        display_power(true);
        display_clear();
        display_draw_text(4, 16, "WiFi init failed!",  DISP_FONT_NORMAL);
        display_draw_text(4, 28, esp_err_to_name(err), DISP_FONT_SMALL);
        display_draw_text(4, 42, "Restarting...",      DISP_FONT_SMALL);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(WIFI_INIT_FAIL_DELAY_MS));
        esp_restart();
    }

    s_restart_count = 0;

    /* 15. I2C re-init after WiFi radio bringup resets the peripheral. */
    i_sensors_init();
    i2c_master_bus_handle_t bus = i_sensors_get_bus();
    if (bus) {
        esp_err_t reinit_err = display_reinit_i2c(bus);
        display_set_available(reinit_err == ESP_OK);
    } else {
        display_set_available(false);
    }

    /* 16. Restore display now that the WiFi association window is over. */
    display_power(true);

    /* 17. Health monitor — needs peripheral map from i_sensors. */
    health_monitor_init();

    o_led_blink(LED_PATTERN_HEARTBEAT);

    /* 18. Main loop — 10 Hz UI tick + health monitor. */
    ESP_LOGI(TAG, "Entering main loop");

    while (1) {
        prov_ui_tick();
        health_monitor_tick();
        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_MS));
    }
}
