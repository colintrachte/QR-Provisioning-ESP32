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
 *                              if absent. All subsequent modules read from here.
 *  7.  prov_ui_init()          Pre-generate WiFi + URL QR codes (~50 ms).
 *  8.  reprovision check       Non-blocking GPIO poll over REPROV_HOLD_MS.
 *  9.  wifi_manager_start()    Try stored credentials; fall back to portal.
 *  10. i_sensors_init()        Re-init I2C post-WiFi; scan bus; map peripherals.
 *                              Calls display_reinit_i2c() internally.
 *  11. ctrl_drive_init()       Init motor driver hardware.
 *  12. health_monitor_init()   Cache peripheral map; baseline telemetry.
 *  13. app_server_start()      HTTP file server + WebSocket on port 80.
 *                              (Actually started by wifi_manager once STA connects)
 *  14. udp_ctrl_start()        UDP control socket on port 4210 (post-STA).
 *  15. Main loop               prov_ui_tick(), health_monitor_tick(), 10 Hz.
 *                              Spawns app_task once STA is connected.
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
    o_led_blink(LED_PATTERN_FAST_BLINK);  /* visual feedback during hold */

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
    o_led_set(1.0f);   /* solid on while erasing */
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

    /* Show reset reason on the last line instead of generic "to recover" */
    char rst_str[20];
    snprintf(rst_str, sizeof(rst_str), "Rst:%s",
             rst == ESP_RST_PANIC    ? "PANIC" :
             rst == ESP_RST_INT_WDT  ? "IWD" :
             rst == ESP_RST_TASK_WDT ? "TWD" :
             rst == ESP_RST_WDT      ? "WD" : "OTHER");
    display_draw_text( 2, 55, rst_str, DISP_FONT_SMALL);

    display_flush();
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}

/* ── Application task ───────────────────────────────────────────────────────
 * Spawned once STA is connected. Keeps robot logic off the UI tick.
 */
static void app_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "app_task started");

    uint32_t last_telemetry_ms = 0;

    while (1) {
        ctrl_drive_tick();
        i_sensors_tick();

        /* Push telemetry at the configured interval (default 200 ms / 5 Hz). */
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - last_telemetry_ms >= settings_get()->telemetry_interval_ms) {
            last_telemetry_ms = now_ms;
            app_server_push_telemetry();
        }

        vTaskDelay(pdMS_TO_TICKS(10));   /* 100 Hz tick; motor ramp uses this */
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
             rst == ESP_RST_POWERON   ? "POWERON" :
             rst == ESP_RST_SW        ? "SW" :
             rst == ESP_RST_PANIC     ? "PANIC" :
             rst == ESP_RST_INT_WDT   ? "INT_WDT" :
             rst == ESP_RST_TASK_WDT  ? "TASK_WDT" :
             rst == ESP_RST_WDT       ? "WDT" :
             rst == ESP_RST_DEEPSLEEP ? "DEEPSLEEP" :
             rst == ESP_RST_BROWNOUT  ? "BROWNOUT" :
             rst == ESP_RST_USB       ? "USB" :
             rst == ESP_RST_JTAG      ? "JTAG" : "UNKNOWN");
    /* 1. Display: must be first so every subsequent step can show status. */
    display_init();

    /* 2. LED: second so reprovision check and safe mode can blink patterns. */
    o_led_init();
    o_led_blink(LED_PATTERN_SLOW_BLINK);  /* "booting" indicator */

    /* 3. Battery ADC: safe before WiFi; ADC1 is unaffected by radio init. */
    i_battery_init();

    /* 4. UI splash — shown immediately so the user sees activity. */
    prov_ui_show_boot();

    /* 5. NVS: must be initialised before settings_load() and before
     *    wifi_manager_start() (which also calls nvs_flash_init internally;
     *    the second call returns ESP_ERR_INVALID_STATE which wifi_manager
     *    already handles as a no-op). */
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

    /* 6. Settings: load runtime tunables from NVS. All modules below read
     *    from settings_get() rather than config.h #defines for their
     *    tuneable parameters. Falls back to compiled-in defaults on first
     *    boot or after a factory reset — never blocks startup. */
    {
        esp_err_t sload = settings_load();
        if (sload == ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGI(TAG, "No saved settings — using defaults");
        else if (sload != ESP_OK)
            ESP_LOGW(TAG, "settings_load: %s — defaults in use",
                     esp_err_to_name(sload));
    }

    /* 7. QR pre-generation: uses AP creds from settings. */
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
    display_power(false);//save power during WiFi init; re-enable after reprovision check and safe mode
    /* 9. WiFi — AP credentials come from settings, not config.h #defines.
     *    wifi_manager_start() calls nvs_flash_init() internally; it returns
     *    ESP_ERR_INVALID_STATE on the second call which the manager treats as
     *    a no-op, so the double-init is harmless. */
    wifi_manager_config_t cfg = WIFI_MANAGER_CONFIG_DEFAULT();
    cfg.ap_ssid         = settings_get()->ap_ssid;
    cfg.ap_password     = settings_get()->ap_password;
    cfg.sta_max_retries = 3;
    cfg.on_state_change = prov_ui_on_state_change;
    cfg.on_connected    = prov_ui_on_connected;
    cfg.on_radio_reset  = on_radio_reset;

    o_led_blink(LED_PATTERN_FAST_BLINK);  /* "connecting" indicator */

    esp_err_t err = wifi_manager_start(&cfg);
    if (err != ESP_OK) {
        s_restart_count++;
        ESP_LOGE(TAG, "wifi_manager_start failed (%d/%d): %s",
                 s_restart_count, WIFI_MAX_RESTART_ATTEMPTS, esp_err_to_name(err));

        o_led_blink(LED_PATTERN_DOUBLE_BLINK);
        display_clear();
        display_draw_text(4, 16, "WiFi init failed!",  DISP_FONT_NORMAL);
        display_draw_text(4, 28, esp_err_to_name(err), DISP_FONT_SMALL);
        display_draw_text(4, 42, "Restarting...",      DISP_FONT_SMALL);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(WIFI_INIT_FAIL_DELAY_MS));
        esp_restart();
    }

    s_restart_count = 0;

    /* 10. I2C re-init after WiFi. No delay needed — re-init is explicit.
     *     i_sensors_init() calls display_reinit_i2c() before scanning. */
    i_sensors_init();
    i2c_master_bus_handle_t bus = i_sensors_get_bus();
    if (bus) {
        esp_err_t err = display_reinit_i2c(bus);
        /* reinit_i2c now sets s_available internally based on success/fail */
        display_set_available(err == ESP_OK);
    } else {
        display_set_available(false);
    }
    display_power(true);
    /* 11. Motor driver. */
    ctrl_drive_init();
    /* Spawn app_task immediately — motors work on AP or STA */
    xTaskCreatePinnedToCore(app_task, "app_task", 4096, NULL, 5, NULL, 1);
    udp_ctrl_start();   /* also works on AP interface */
    bool app_task_spawned = true;
    /* 12. Health monitor. */
    health_monitor_init();

    /* 13. App server is started by wifi_manager once STA connects;
     *     see mgr_task in wifi_manager.c. */

    o_led_blink(LED_PATTERN_HEARTBEAT);  /* "running" indicator */

    /* 14. Main loop — 10 Hz UI tick + health checks. */
    ESP_LOGI(TAG, "Entering main loop");

    while (1) {
        prov_ui_tick();
        health_monitor_tick();

        if (wifi_manager_is_connected() && !app_task_spawned) {
            ESP_LOGI(TAG, "STA connected — spawning app_task");
            /* Pin app_task to Core 1 (APP_CPU) so motor command
             * processing on the httpd task (Core 0 / PRO_CPU) is
             * never preempted by telemetry or sensor work. */
            xTaskCreatePinnedToCore(app_task, "app_task", 4096,
                                    NULL, 5, NULL, 1);
            /* UDP control socket also runs on Core 1. udp_ctrl_start()
             * is safe to call after STA is up — it binds INADDR_ANY so
             * it works on any interface. app_server_start() is called by
             * wifi_manager's mgr_task just before this branch is reached. */
            udp_ctrl_start();
            app_task_spawned = true;
        }

        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_MS));
    }
}
