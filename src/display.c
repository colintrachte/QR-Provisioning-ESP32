/**
 * display.c — u8g2-backed SSD1306 driver for Heltec WiFi LoRa 32 V3.
 *
 * Handles:
 *   - Vext / RST GPIO sequencing (Heltec-specific power-on ritual)
 *   - u8g2 initialisation via the mkfrey ESP32 HAL
 *   - "Flush when dirty" contract (zero I2C traffic between frames)
 *   - Thin draw wrappers so callers never touch u8g2 directly
 *   - QR code rendering via qrcodegen module data → u8g2_DrawBox()
 */

#include "display.h"
#include "qrcodegen.h"

#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

static u8g2_t s_u8g2;
static bool   s_dirty       = false;
static bool   s_initialized = false;

/* ── Internal helpers ───────────────────────────────────────────────────*/

/** Configure a GPIO pin as output and drive it to level (0 or 1). */
static void gpio_output_set(int pin, int level)
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

/* ── Lifecycle ──────────────────────────────────────────────────────────*/

void display_init(void)
{
    ESP_LOGI(TAG, "Powering OLED via Vext (GPIO%d)", DISP_PIN_VEXT);

    /* 1. Enable power rail for the OLED panel. */
    gpio_output_set(DISP_PIN_VEXT, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2. Hardware-reset the SSD1306 controller. */
    gpio_output_set(DISP_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_output_set(DISP_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 3. Point the mkfrey HAL at our I2C pins.
     *    RST is passed as -1 — we already handled it above. */
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = DISP_PIN_SDA;
    hal.bus.i2c.scl = DISP_PIN_SCL;
    hal.reset        = -1;
    u8g2_esp32_hal_init(hal);

    /* 4. Initialise u8g2 for SSD1306 128×64 in full-frame-buffer mode.
     *    _f suffix = entire 1024-byte framebuffer in RAM; SendBuffer()
     *    pushes it all in one I2C burst.  U8G2_R0 = no rotation. */
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &s_u8g2,
        U8G2_R0,
        u8g2_esp32_i2c_byte_cb,
        u8g2_esp32_gpio_and_delay_cb);

    /* 5. Set I2C address (0x3C << 1 = 0x78, as u8g2 expects). */
    u8x8_SetI2CAddress(&s_u8g2.u8x8, DISP_I2C_ADDR);

    /* 6. Send the SSD1306 initialisation sequence and turn the display on. */
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);  /* 0 = display on */

    /* 7. Use the top of the glyph bounding box as the y origin for DrawStr.
     *    Without this, y is the text baseline, which varies per font and
     *    makes pixel-exact layout impossible. */
    u8g2_SetFontPosTop(&s_u8g2);

    /* 8. Push a blank frame so GDDRAM starts clean. */
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);

    s_dirty      = false;
    s_initialized = true;
    ESP_LOGI(TAG, "SSD1306 ready");
}

void display_power(bool on)
{
    u8g2_SetPowerSave(&s_u8g2, on ? 0 : 1);
}

void display_contrast(uint8_t level)
{
    u8g2_SetContrast(&s_u8g2, level);
}

/* ── Escape hatch ───────────────────────────────────────────────────────*/

u8g2_t *display_get_u8g2(void) { return &s_u8g2; }

/* ── Dirty flag ─────────────────────────────────────────────────────────*/

void display_mark_dirty(void)  { s_dirty = true;  }
void display_clear_dirty(void) { s_dirty = false; }
bool display_is_dirty(void)    { return s_dirty;  }

/* ── Draw calls ─────────────────────────────────────────────────────────*/

void display_clear(void)
{
    u8g2_ClearBuffer(&s_u8g2);
    s_dirty = true;
}

void display_clear_region(int x, int y, int w, int h)
{
    u8g2_SetDrawColor(&s_u8g2, 0);  /* 0 = black */
    u8g2_DrawBox(&s_u8g2, x, y, w, h);
    u8g2_SetDrawColor(&s_u8g2, 1);  /* restore white */
    s_dirty = true;
}

int display_draw_text(int x, int y, const char *str, const uint8_t *font)
{
    if (!str) return x;
    u8g2_SetFont(&s_u8g2, font);
    u8g2_SetDrawColor(&s_u8g2, 1);
    int advance = u8g2_DrawStr(&s_u8g2, x, y, str);
    s_dirty = true;
    return x + advance;
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
        ESP_LOGW(TAG, "display_draw_qr: invalid QR buffer (size=%d)", size);
        return;
    }

    /* Quiet zone: the QR spec requires 4 modules; we use scale*2 which
     * gives 4 px at scale=2.  At scale=3 we skip the quiet zone entirely
     * because 128 px is too tight for a v1 QR at that scale with borders. */
    int quiet = (scale >= 3) ? 0 : (scale * 2);
    int total = size * scale + quiet * 2;

    /* Fill quiet zone with white (background). */
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawBox(&s_u8g2, x, y, total, total);

    /* Draw each dark module as a filled square of scale×scale pixels. */
    u8g2_SetDrawColor(&s_u8g2, 1);
    for (int qy = 0; qy < size; qy++) {
        for (int qx = 0; qx < size; qx++) {
            if (qrcodegen_getModule(qrcode, qx, qy)) {
                u8g2_DrawBox(&s_u8g2,
                             x + quiet + qx * scale,
                             y + quiet + qy * scale,
                             scale, scale);
            }
        }
    }
    u8g2_SetDrawColor(&s_u8g2, 1);  /* leave draw colour in known state */
    s_dirty = true;
}

/* ── Flush ──────────────────────────────────────────────────────────────*/

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
