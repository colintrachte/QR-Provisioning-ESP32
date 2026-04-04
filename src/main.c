/**
 * main.c — Robot Provisioning entry point
 *
 * Startup sequence:
 *   1. display_init()          — power OLED, send init commands
 *   2. prov_ui_show_boot()     — render boot screen, flush once
 *   3. prov_ui_init()          — pre-generate both QR codes
 *   4. wifi_prov_start()       — attempt stored creds or start portal
 *   5. main loop — calls prov_ui_tick() which handles QR cycling
 *
 * After provisioning succeeds, the success screen stays up indefinitely
 * and the main loop can be extended with robot application code.
 */

#include "display.h"
#include "qr_gen.h"
#include "wifi_prov.h"
#include "prov_ui.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/* ── AP credentials — change these for your device ─────────────────────*/
#define AP_SSID     "RobotSetup"
#define AP_PASSWORD "robot1234"   /* min 8 chars; set to "" for open AP  */

/* ── Re-provision trigger ───────────────────────────────────────────────*/
/* GPIO0 is the USER_SW button on Heltec V3.
   Hold on boot to force re-provisioning regardless of saved credentials. */
#define REPROV_BUTTON_GPIO  0
#define REPROV_HOLD_MS      3000

#include "driver/gpio.h"

static bool check_reprovision_button(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << REPROV_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    if (gpio_get_level(REPROV_BUTTON_GPIO) != 0) return false;

    /* Button is held — wait to confirm it stays held */
    ESP_LOGI(TAG, "Button held — hold for %dms to re-provision", REPROV_HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(REPROV_HOLD_MS));
    return (gpio_get_level(REPROV_BUTTON_GPIO) == 0);
}

/* ── Application entry point ────────────────────────────────────────────*/

void app_main(void)
{
    ESP_LOGI(TAG, "=== Robot Provisioning Firmware ===");
    ESP_LOGI(TAG, "Board: Heltec WiFi LoRa 32 V3 (ESP32-S3)");

    /* ── 1. Display init — must be first so we can show status ── */
    display_init();

    /* ── 2. Boot splash ── */
    prov_ui_show_boot();

    /* ── 3. Pre-generate QR codes (CPU-intensive, ~50ms, do before WiFi init) ── */
    prov_ui_init(AP_SSID, AP_PASSWORD);

    /* ── 4. Check re-provision button ── */
    if (check_reprovision_button()) {
        ESP_LOGW(TAG, "Re-provision button held — erasing credentials");
        /* NVS must be init'd before we can erase */
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
        wifi_prov_erase_credentials();
        display_clear();
        display_draw_text(4, 24, "Credentials erased.", u8g2_font_6x10_tf);
        display_draw_text(4, 36, "Restarting...",       u8g2_font_6x10_tf);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    /* ── 5. Start WiFi provisioning ── */
    wifi_prov_config_t prov_cfg = WIFI_PROV_CONFIG_DEFAULT();
    prov_cfg.ap_ssid         = AP_SSID;
    prov_cfg.ap_password     = AP_PASSWORD;
    prov_cfg.sta_max_retries = 5;
    prov_cfg.on_state_change = prov_ui_on_state_change;
    prov_cfg.on_connected    = prov_ui_on_connected;
    prov_cfg.cb_ctx          = NULL;

    esp_err_t ret = wifi_prov_start(&prov_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_start failed: %s", esp_err_to_name(ret));
        display_clear();
        display_draw_text(4, 20, "Prov start failed!", u8g2_font_6x10_tf);
        display_draw_text(4, 32, esp_err_to_name(ret), u8g2_font_6x10_tf);
        display_flush();
        /* Halt — reboot in 10s */
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    /* ── 6. Main loop ── */
    ESP_LOGI(TAG, "Entering main loop");
    while (1) {
        /* Drive QR cycling — only redraws when timer fires, otherwise returns
           immediately.  No periodic display refresh happens anywhere else. */
        prov_ui_tick();

        /* Once connected, application code goes here.
           Example: read sensors, run robot logic, etc.
           The display shows the success screen until you call display_* again. */
        if (wifi_prov_is_connected()) {
            /* Robot application placeholder */
            /* robot_app_tick(); */
        }

        vTaskDelay(pdMS_TO_TICKS(100));  /* 10Hz tick rate — adequate for QR cycling */
    }
}
