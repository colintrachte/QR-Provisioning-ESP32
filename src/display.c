/**
 * display.c — u8g2-backed SSD1306 driver for Heltec WiFi LoRa 32 V3
 *
 * What this file contains (after replacing the 584-line custom driver):
 *   - Vext / RST GPIO sequencing (Heltec-specific, u8g2 doesn't know about it)
 *   - u8g2 init with the mkfrey HAL for ESP-IDF I2C
 *   - A dirty flag so callers control when the frame is pushed to hardware
 *   - Thin wrappers around u8g2 draw calls so prov_ui.c stays clean
 *   - display_draw_qr() which plots nayuki QR modules via u8g2_DrawBox()
 *
 * Everything else (fonts, framebuffer, init sequences, I2C transactions)
 * is handled by u8g2 + mkfrey HAL.
 */

#include "display.h"
#include "qrcodegen.h"

#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

/* ── Module state ─────────────────────────────────────────────────────── */
static u8g2_t s_u8g2;
static bool   s_dirty = false;
static bool   s_initialized = false;

/* ── GPIO helper ──────────────────────────────────────────────────────── */
static void set_gpio_out(int pin, int level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, level);
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void display_init(void)
{
    ESP_LOGI(TAG, "display_init: powering Vext GPIO%d", DISP_PIN_VEXT);

    /* 1. Power the OLED panel via Vext */
    set_gpio_out(DISP_PIN_VEXT, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2. Hardware reset the SSD1306 */
    set_gpio_out(DISP_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    set_gpio_out(DISP_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 3. Configure the mkfrey HAL: tell it which ESP32 pins to use */
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = DISP_PIN_SDA;
    hal.bus.i2c.scl = DISP_PIN_SCL;
    /* RST is managed by us above; pass -1 so the HAL doesn't touch it */
    hal.reset = -1;
    u8g2_esp32_hal_init(hal);

    /* 4. Set up u8g2 for SSD1306 128x64 in full-frame buffer mode (_f suffix).
     *    Full-frame = 1024 bytes in RAM; u8g2_SendBuffer() pushes the whole
     *    frame in one shot.  This matches our "flush when dirty" contract.
     *
     *    Setup function chosen: noname variant — works with the Heltec V3's
     *    generic SSD1306 panel.  U8G2_R0 = no rotation.
     */
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &s_u8g2,
        U8G2_R0,
        u8g2_esp32_i2c_byte_cb,
        u8g2_esp32_gpio_and_delay_cb
    );

    /* 5. Set I2C address (0x3C shifted left = 0x78, as u8g2 expects) */
    u8x8_SetI2CAddress(&s_u8g2.u8x8, DISP_I2C_ADDR);

    /* 6. Send init sequence to the SSD1306 (wakes it from sleep) */
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);  /* 0 = display ON */

    /* Use top-of-glyph as the y reference for all DrawStr calls.
     * This means y=0 means top pixel, matching our pixel layout.
     * Without this, y is the text baseline which varies by font. */
    u8g2_SetFontPosTop(&s_u8g2);

    /* 7. Clear buffer and push a blank frame to GDDRAM */
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);

    s_dirty      = false;
    s_initialized = true;
    ESP_LOGI(TAG, "SSD1306 ready via u8g2");
}

void display_power(bool on)
{
    u8g2_SetPowerSave(&s_u8g2, on ? 0 : 1);
}

void display_contrast(uint8_t level)
{
    u8g2_SetContrast(&s_u8g2, level);
}

/* ── Raw handle ───────────────────────────────────────────────────────── */

u8g2_t *display_u8g2(void) { return &s_u8g2; }

/* ── Dirty flag ───────────────────────────────────────────────────────── */

void display_mark_dirty(void)  { s_dirty = true;  }
void display_clear_dirty(void) { s_dirty = false; }
bool display_is_dirty(void)    { return s_dirty;  }

/* ── Drawing wrappers ─────────────────────────────────────────────────── */

void display_clear(void)
{
    u8g2_ClearBuffer(&s_u8g2);
    s_dirty = true;
}

void display_clear_region(int x, int y, int w, int h)
{
    u8g2_SetDrawColor(&s_u8g2, 0);  /* 0 = background (black) */
    u8g2_DrawBox(&s_u8g2, x, y, w, h);
    u8g2_SetDrawColor(&s_u8g2, 1);  /* restore to foreground  */
    s_dirty = true;
}

int display_draw_text(int x, int y, const char *str, const uint8_t *font)
{
    if (!str) return x;
    u8g2_SetFont(&s_u8g2, font);
    u8g2_SetDrawColor(&s_u8g2, 1);
    /* FontPosTop is set in display_init() so y is top-of-glyph, not baseline. */
    int width = u8g2_DrawStr(&s_u8g2, x, y, str);
    s_dirty = true;
    return x + width;
}

void display_fill_rect(int x, int y, int w, int h, bool on)
{
    u8g2_SetDrawColor(&s_u8g2, on ? 1 : 0);
    u8g2_DrawBox(&s_u8g2, x, y, w, h);
    u8g2_SetDrawColor(&s_u8g2, 1);
    s_dirty = true;
}

void display_draw_rect(int x, int y, int w, int h)
{
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawFrame(&s_u8g2, x, y, w, h);
    s_dirty = true;
}

void display_draw_hline(int x, int y, int len)
{
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawHLine(&s_u8g2, x, y, len);
    s_dirty = true;
}

void display_draw_vline(int x, int y, int len)
{
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawVLine(&s_u8g2, x, y, len);
    s_dirty = true;
}

void display_draw_qr(int x, int y, int scale, const uint8_t *qrcode)
{
    int size = qrcodegen_getSize(qrcode);
    if (size <= 0) {
        ESP_LOGW(TAG, "display_draw_qr: invalid QR (size=%d)", size);
        return;
    }

    int quiet = scale * 2;  /* 2-module quiet zone per spec */
    int total = size * scale + quiet * 2;

    /* White (background) quiet zone */
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawBox(&s_u8g2, x, y, total, total);

    /* Dark modules */
    u8g2_SetDrawColor(&s_u8g2, 1);
    for (int qy = 0; qy < size; qy++) {
        for (int qx = 0; qx < size; qx++) {
            if (qrcodegen_getModule(qrcode, qx, qy)) {
                u8g2_DrawBox(
                    &s_u8g2,
                    x + quiet + qx * scale,
                    y + quiet + qy * scale,
                    scale,
                    scale
                );
            }
        }
    }
    u8g2_SetDrawColor(&s_u8g2, 1);  /* restore */
    s_dirty = true;
}

/* ── Flush ────────────────────────────────────────────────────────────── */

void display_flush(void)
{
    if (!s_dirty) return;
    u8g2_SendBuffer(&s_u8g2);
    s_dirty = false;
}

void display_flush_force(void)
{
    u8g2_SendBuffer(&s_u8g2);
    s_dirty = false;
}
