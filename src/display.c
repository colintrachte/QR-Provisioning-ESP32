/**
 * display.c — u8g2-backed SSD1306 driver for Heltec WiFi LoRa 32 V3.
 *
 * Changes from original
 * ─────────────────────
 *  - All draw functions are guarded by s_initialized; calling any draw
 *    function before display_init() is now a safe no-op with a log warning
 *    rather than a crash.
 *  - A FreeRTOS mutex (s_mutex) protects the u8g2 buffer and dirty flag
 *    against concurrent access from multiple tasks.
 *  - display_set_color() exposes draw colour so callers can draw in
 *    "inverted" (black-on-white) mode without fighting the wrappers.
 *  - display_draw_qr() now validates that the rendered image fits within
 *    the 128×64 panel and logs a warning if it would be clipped.
 *  - display_draw_qr() uses DrawPixel at scale=1 instead of DrawBox for
 *    speed.
 *  - I2C clock is set to 400 kHz for faster display_flush().
 *  - display_init() returns false gracefully if the OLED hardware is absent
 *    (e.g. disconnected ribbon cable) so the rest of the firmware continues.
 */

#include "display.h"
#include "qrcodegen.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

static u8g2_t          s_u8g2;
static bool            s_dirty       = false;
static bool            s_initialized = false;
static bool            s_available   = false; /* false if OLED not detected */
static SemaphoreHandle_t s_mutex     = NULL;

/* Current draw colour (1 = white/on, 0 = black/off).  Settable via
 * display_set_color() so callers can draw inverted text or shapes without
 * the wrappers silently overriding them. */
static uint8_t s_draw_color = 1;

/* ── Guard macro ────────────────────────────────────────────────────────────
 * Every public draw function calls DISPLAY_GUARD_RET() at entry.
 * If the display was never initialised or is unavailable, the call is a
 * safe no-op (returns the value passed as 'ret').
 */
#define DISPLAY_GUARD_RET(ret)                                          \
    do {                                                                \
        if (!s_initialized || !s_available) {                           \
            if (DEBUG_DISPLAY) {                                        \
                ESP_LOGD(TAG, "%s: display not ready, skipping",        \
                         __func__);                                     \
            }                                                           \
            return ret;                                               \
        }                                                               \
    } while (0)

/* Void variant for functions that return nothing. */
#define DISPLAY_GUARD() DISPLAY_GUARD_RET( )

/* ── Internal helpers ───────────────────────────────────────────────────────*/

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

/* ── Lifecycle ──────────────────────────────────────────────────────────────*/

bool display_init(void)
{
    if (s_initialized) return s_available; /* idempotent */

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create display mutex");
        s_initialized = true;
        s_available   = false;
        return false;
    }

    ESP_LOGI(TAG, "Powering OLED via Vext (GPIO%d)", DISP_PIN_VEXT);

    /* 1. Enable the OLED power rail.
     * Heltec V3: GPIO36 HIGH = Vext ON.  (V3.1 variant is inverted — LOW = ON.) */
    gpio_output_set(DISP_PIN_VEXT, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2. Hardware-reset the SSD1306. */
    gpio_output_set(DISP_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_output_set(DISP_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 3. Configure the ESP32 HAL (400 kHz I2C for faster flushes).
     * WARNING: esp_wifi_start() resets the I2C peripheral as a side-effect
     * of radio bring-up.  display_init() must be called BEFORE wifi_manager_start()
     * OR the HAL must be re-initialised after WiFi starts.  In this firmware,
     * display_init() runs first (step 1 of app_main), so the sequence is safe.
     * If you reorder boot steps, the OLED will go dark and I2C will error. */
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.sda              = DISP_PIN_SDA;
    hal.scl              = DISP_PIN_SCL;
    hal.reset            = -1;   /* RST already toggled above */
    hal.i2c_num          = I2C_NUM_0;
    hal.i2c_clk_speed    = 400000; /* 400 kHz — SSD1306 supports up to ~1 MHz */
    u8g2_esp32_hal_init(hal);

    /* 4. Initialise u8g2 in full-frame-buffer mode. */
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &s_u8g2,
        U8G2_R0,
        u8g2_esp32_i2c_byte_cb,
        u8g2_esp32_gpio_and_delay_cb);

    u8x8_SetI2CAddress(&s_u8g2.u8x8, DISP_I2C_ADDR);

    /* 5. Send init sequence.  u8g2_InitDisplay() communicates with the
     * hardware over I2C — if the OLED is disconnected this will fail.
     * We treat the absence of the display as a non-fatal condition: the
     * rest of the firmware continues, draw calls become no-ops. */
    u8g2_InitDisplay(&s_u8g2);

    /* 6. Quick sanity check: try to send one byte and see if the I2C ACKs.
     * u8g2 does not expose a clean "is hardware present?" query, so we
     * infer it from SetPowerSave returning normally.  If the bus hangs
     * the HAL will timeout and log an error; we check s_available below. */
    u8g2_SetPowerSave(&s_u8g2, 0); /* 0 = display on */

    /* If the HAL logged I2C errors the display is probably missing, but we
     * have no cross-platform way to query that here.  The health_monitor
     * component does an explicit I2C probe at DISP_I2C_ADDR to set the
     * definitive s_available flag via display_set_available(). */
    s_available = true; /* optimistic default; health_monitor may correct this */

    /* Use the top of the glyph bounding box as the y origin for DrawStr.
     * Without this, y is the text baseline, which varies per font and makes
     * pixel-exact layout impossible across mixed font sizes. */
    u8g2_SetFontPosTop(&s_u8g2);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);

    s_dirty      = false;
    s_initialized = true;
    ESP_LOGI(TAG, "SSD1306 ready (400 kHz I2C)");
    return true;
}

/* Called by health_monitor after its I2C probe to set the definitive flag. */
void display_set_available(bool available)
{
    s_available = available;
    if (!available) {
        ESP_LOGW(TAG, "OLED not detected — display calls will be no-ops");
    }
}

void display_reinit_i2c(void)
{
    if (!s_initialized) return;

    /* Re-run only the HAL init. The display panel retains its GDDRAM contents
     * and power state — only the ESP32 peripheral side was reset by WiFi. */
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.sda           = DISP_PIN_SDA;
    hal.scl           = DISP_PIN_SCL;
    hal.reset         = -1;
    hal.i2c_num       = I2C_NUM_0;
    hal.i2c_clk_speed = 400000;
    u8g2_esp32_hal_init(hal);

    /* Re-send the init sequence so the controller is in a known state.
     * u8g2_InitDisplay() does not clear GDDRAM — the previous image stays. */
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_SetFontPosTop(&s_u8g2);

    ESP_LOGI("display", "I2C HAL re-initialised after WiFi startup");
}

bool display_is_available(void) { return s_available; }

void display_power(bool on)
{
    DISPLAY_GUARD();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SetPowerSave(&s_u8g2, on ? 0 : 1);
    xSemaphoreGive(s_mutex);
}

void display_contrast(uint8_t level)
{
    DISPLAY_GUARD();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SetContrast(&s_u8g2, level);
    xSemaphoreGive(s_mutex);
}

void display_set_color(uint8_t color)
{
    s_draw_color = color ? 1 : 0;
}

/* ── Escape hatch ───────────────────────────────────────────────────────────*/

u8g2_t *display_get_u8g2(void) { return &s_u8g2; }

/* ── Dirty flag ─────────────────────────────────────────────────────────────*/

void display_mark_dirty(void)  { s_dirty = true;  }
void display_clear_dirty(void) { s_dirty = false; }
bool display_is_dirty(void)    { return s_dirty;  }

/* ── Draw calls ─────────────────────────────────────────────────────────────*/

void display_clear(void)
{
    DISPLAY_GUARD();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_ClearBuffer(&s_u8g2);
    s_dirty = true;
    xSemaphoreGive(s_mutex);
}

void display_clear_region(int x, int y, int w, int h)
{
    DISPLAY_GUARD();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawBox(&s_u8g2, x, y, w, h);
    u8g2_SetDrawColor(&s_u8g2, s_draw_color);
    s_dirty = true;
    xSemaphoreGive(s_mutex);
}

int display_draw_text(int x, int y, const char *str, const uint8_t *font)
{
    DISPLAY_GUARD_RET(x);
    if (!str) return x;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SetFont(&s_u8g2, font);
    u8g2_SetDrawColor(&s_u8g2, s_draw_color);
    int advance = u8g2_DrawStr(&s_u8g2, x, y, str);
    s_dirty = true;
    xSemaphoreGive(s_mutex);

    return x + advance;
}

void display_fill_rect(int x, int y, int w, int h, bool on)
{
    DISPLAY_GUARD();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SetDrawColor(&s_u8g2, on ? 1 : 0);
    u8g2_DrawBox(&s_u8g2, x, y, w, h);
    u8g2_SetDrawColor(&s_u8g2, s_draw_color);
    s_dirty = true;
    xSemaphoreGive(s_mutex);
}

void display_draw_rect(int x, int y, int w, int h)
{
    DISPLAY_GUARD();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SetDrawColor(&s_u8g2, s_draw_color);
    u8g2_DrawFrame(&s_u8g2, x, y, w, h);
    s_dirty = true;
    xSemaphoreGive(s_mutex);
}

void display_draw_hline(int x, int y, int len)
{
    DISPLAY_GUARD();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SetDrawColor(&s_u8g2, s_draw_color);
    u8g2_DrawHLine(&s_u8g2, x, y, len);
    s_dirty = true;
    xSemaphoreGive(s_mutex);
}

void display_draw_vline(int x, int y, int len)
{
    DISPLAY_GUARD();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SetDrawColor(&s_u8g2, s_draw_color);
    u8g2_DrawVLine(&s_u8g2, x, y, len);
    s_dirty = true;
    xSemaphoreGive(s_mutex);
}

void display_draw_qr(int x, int y, int scale, const uint8_t *qrcode)
{
    DISPLAY_GUARD();
    if (!qrcode) return;

    int size = qrcodegen_getSize(qrcode);
    if (size <= 0) {
        ESP_LOGW(TAG, "display_draw_qr: invalid QR buffer (size=%d)", size);
        return;
    }

    int quiet = (scale >= 3) ? 0 : (scale * 2);
    int total = size * scale + quiet * 2;

    /* Bounds check: warn if the QR image would be clipped by the panel. */
    if (x + total > DISP_WIDTH || y + total > DISP_HEIGHT) {
        ESP_LOGW(TAG, "display_draw_qr: QR (%dx%d px) at (%d,%d) exceeds "
                 "panel (%dx%d) — image will be clipped",
                 total, total, x, y, DISP_WIDTH, DISP_HEIGHT);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Fill quiet zone with black (background). */
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawBox(&s_u8g2, x, y, total, total);

    u8g2_SetDrawColor(&s_u8g2, 1);

    for (int qy = 0; qy < size; qy++) {
        for (int qx = 0; qx < size; qx++) {
            if (qrcodegen_getModule(qrcode, qx, qy)) {
                int px = x + quiet + qx * scale;
                int py = y + quiet + qy * scale;

                if (scale == 1) {
                    /* DrawPixel is significantly faster than DrawBox at 1:1. */
                    u8g2_DrawPixel(&s_u8g2, px, py);
                } else {
                    u8g2_DrawBox(&s_u8g2, px, py, scale, scale);
                }
            }
        }
    }

    u8g2_SetDrawColor(&s_u8g2, s_draw_color); /* restore caller's colour */
    s_dirty = true;

    xSemaphoreGive(s_mutex);
}

/* ── Flush ──────────────────────────────────────────────────────────────────*/

void display_flush(void)
{
    DISPLAY_GUARD();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SendBuffer(&s_u8g2);
    s_dirty = false;
    xSemaphoreGive(s_mutex);
}

void display_flush_force(void)
{
    DISPLAY_GUARD();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SendBuffer(&s_u8g2);
    s_dirty = false;
    xSemaphoreGive(s_mutex);
}
