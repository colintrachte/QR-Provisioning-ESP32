/**
 * display.c — u8g2-backed SSD1306 driver for Heltec WiFi LoRa 32 V3.
 *
 * I2C driver migration (ESP-IDF 5.x)
 * ────────────────────────────────────
 * The HAL (u8g2_esp32_hal.c) now uses driver/i2c_master.h exclusively.
 * As a result:
 *
 *  - display_init() no longer installs a legacy i2c driver.  The HAL's
 *    U8X8_MSG_BYTE_INIT handler calls i2c_new_master_bus() + add_device()
 *    internally on the first u8g2_InitDisplay() call.
 *
 *  - display_reinit_i2c() now returns esp_err_t (was void).  It calls
 *    u8g2_esp32_hal_reinit_bus() to tear down and recreate the bus/device
 *    handles, then re-sends the SSD1306 init sequence.  i_sensors_init()
 *    calls this after creating its own bus so the display HAL is consistent
 *    with the freshly installed peripheral state.
 *
 * Everything else (mutex, dirty flag, draw wrappers, DISPLAY_GUARD) is
 * unchanged from the previous version.
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

static u8g2_t            s_u8g2;
static bool              s_dirty       = false;
static bool              s_initialized = false;
static bool              s_available   = false;
static SemaphoreHandle_t s_mutex       = NULL;
static uint8_t           s_draw_color  = 1;

/* ── Guard macros ───────────────────────────────────────────────────────────*/

#define DISPLAY_GUARD_RET(ret)                                          \
    do {                                                                \
        if (!s_initialized || !s_available) {                           \
            if (DEBUG_DISPLAY)                                          \
                ESP_LOGD(TAG, "%s: display not ready, skipping",        \
                         __func__);                                     \
            return ret;                                                 \
        }                                                               \
    } while (0)

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
    if (s_initialized) return s_available;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create display mutex");
        s_initialized = true;
        s_available   = false;
        return false;
    }

    /* 1. Power rail — only where a GPIO switch exists (e.g. Heltec Vext).
     *    TTGO and other boards with always-on PMIC rails set DISP_PIN_VEXT
     *    to -1 in their board header to skip this step. */
#if DISP_PIN_VEXT >= 0
    ESP_LOGI(TAG, "Powering OLED via Vext (GPIO%d)", DISP_PIN_VEXT);
    gpio_output_set(DISP_PIN_VEXT, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
#else
    ESP_LOGI(TAG, "OLED power via PMIC (no Vext GPIO)");
#endif

    /* 2. Hardware reset. */
    gpio_output_set(DISP_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_output_set(DISP_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 3. Configure the HAL (stores parameters; does NOT install driver yet).
     *    The driver is created inside U8X8_MSG_BYTE_INIT on the first
     *    u8g2_InitDisplay() call below. */
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.sda           = DISP_PIN_SDA;
    hal.scl           = DISP_PIN_SCL;
    hal.reset         = -1;         /* RST toggled above; HAL need not touch it */
    hal.i2c_port      = I2C_NUM_0;
    hal.i2c_clk_speed = 400000;
    u8g2_esp32_hal_init(hal);

    /* 4. Init u8g2 in full-frame-buffer mode. */
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &s_u8g2,
        U8G2_R0,
        u8g2_esp32_i2c_byte_cb,
        u8g2_esp32_gpio_and_delay_cb);

    u8x8_SetI2CAddress(&s_u8g2.u8x8, DISP_I2C_ADDR);

    /* 5. Send init sequence — this triggers U8X8_MSG_BYTE_INIT in the HAL,
     *    which creates the i2c_master bus + device handles.
     *    If the OLED is absent the HAL will log an error; we set s_available
     *    optimistically here and let i_sensors' bus scan correct it. */
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);

    s_available = true;   /* corrected by display_set_available() after scan */

    u8g2_SetFontPosTop(&s_u8g2);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);

    s_dirty       = false;
    s_initialized = true;
    ESP_LOGI(TAG, "SSD1306 ready (400 kHz I2C)");
    return true;
}

void display_set_available(bool available)
{
    s_available = available;
    if (!available)
        ESP_LOGW(TAG, "OLED not detected — display calls will be no-ops");
}

/**
 * Re-synchronise the u8g2 HAL after WiFi resets the I2C peripheral.
 *
 * Called by i_sensors_init() after it has created a fresh i2c_master bus.
 * Delegates the handle teardown/rebuild to u8g2_esp32_hal_reinit_bus(),
 * then re-sends the SSD1306 init sequence so the controller is in a known
 * state (GDDRAM contents are preserved — the panel stayed powered).
 *
 * Returns ESP_OK on success, or the error from u8g2_esp32_hal_reinit_bus().
 */
esp_err_t display_reinit_i2c(i2c_master_bus_handle_t new_bus)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = u8g2_esp32_hal_reinit_bus(new_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HAL reinit failed: %s", esp_err_to_name(err));
        return err;
    }

    // DO NOT call u8g2_InitDisplay() here — it sends the full SSD1306
    // init sequence which clears GDDRAM and blanks the panel.
    // The controller stayed powered through the WiFi disruption;
    // only the ESP32-side bus handles were stale. The HAL reinit
    // above replaced them. A flush will work immediately.

    // Only restore font position setting (no hardware command):
    u8g2_SetFontPosTop(&s_u8g2);
    s_available = true;          /* reinit succeeded — display is reachable */
    display_flush_force();   /* push current buffer to freshly reinited controller */

    ESP_LOGI(TAG, "I2C HAL re-initialised after WiFi startup");
    return ESP_OK;
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

u8g2_t *display_get_u8g2(void) { return &s_u8g2; }

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

    int quiet = 0;//(scale >= 3) ? 0 : (scale * 2);//we don't have room for this.
    int total = size * scale + quiet * 2;

    if (x + total > DISP_WIDTH || y + total > DISP_HEIGHT) {
        ESP_LOGW(TAG, "display_draw_qr: QR (%dx%d px) at (%d,%d) exceeds "
                 "panel (%dx%d) — image will be clipped",
                 total, total, x, y, DISP_WIDTH, DISP_HEIGHT);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawBox(&s_u8g2, x, y, total, total);
    u8g2_SetDrawColor(&s_u8g2, 1);

    for (int qy = 0; qy < size; qy++) {
        for (int qx = 0; qx < size; qx++) {
            if (qrcodegen_getModule(qrcode, qx, qy)) {
                int px = x + quiet + qx * scale;
                int py = y + quiet + qy * scale;
                if (scale == 1)
                    u8g2_DrawPixel(&s_u8g2, px, py);
                else
                    u8g2_DrawBox(&s_u8g2, px, py, scale, scale);
            }
        }
    }

    u8g2_SetDrawColor(&s_u8g2, s_draw_color);
    s_dirty = true;

    xSemaphoreGive(s_mutex);
}

/* ── Flush ──────────────────────────────────────────────────────────────────*/

void display_flush(void)
{
    DISPLAY_GUARD();
    if (!s_dirty) return;          /* skip if nothing changed */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SendBuffer(&s_u8g2);
    s_dirty = false;
    xSemaphoreGive(s_mutex);
}

void display_flush_force(void)
{
    DISPLAY_GUARD();               /* always sends, ignores dirty flag */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    u8g2_SendBuffer(&s_u8g2);
    s_dirty = false;
    xSemaphoreGive(s_mutex);
}
