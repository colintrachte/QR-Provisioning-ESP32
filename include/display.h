#pragma once
/**
 * display.h — u8g2-backed SSD1306 OLED driver for Heltec WiFi LoRa 32 V3.
 *
 * "Draw into RAM, flush on demand" contract
 * ─────────────────────────────────────────
 *  - All draw_* calls write to u8g2's full-frame buffer (1024 bytes in RAM).
 *    No I2C traffic until display_flush() is called.
 *  - display_flush() pushes the buffer to SSD1306 GDDRAM only when the
 *    dirty flag is set, then clears it.
 *  - display_flush_force() pushes unconditionally.
 *
 * Thread safety
 * ─────────────
 *  All public functions are protected by an internal FreeRTOS mutex.
 *  It is safe to call draw_* and flush from different tasks.
 *
 * Graceful degradation
 * ────────────────────
 *  display_init() returns false if it cannot communicate with the SSD1306
 *  (e.g. disconnected cable).  All draw calls become silent no-ops in that
 *  state — the rest of the firmware keeps running normally.
 *  Call display_is_available() to query whether the OLED is present.
 *  i_sensors_init() calls display_set_available() after its I2C scan
 *  to set the definitive hardware-detected flag.
 *
 * I2C driver
 * ──────────
 *  Uses driver/i2c_master.h (ESP-IDF 5.x new API) exclusively.
 *  The legacy driver/i2c.h is not used anywhere in this module.
 *
 * Pin assignments — Heltec WiFi LoRa 32 V3
 * ──────────────────────────────────────────
 *   SDA = GPIO17   SCL = GPIO18   RST = GPIO21   Vext = GPIO36
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "u8g2.h"
#include "u8g2_esp32_hal.h"

/* ── Font aliases ───────────────────────────────────────────────────────────*/
#define DISP_FONT_SMALL   u8g2_font_5x7_tf
#define DISP_FONT_NORMAL  u8g2_font_6x10_tf
#define DISP_FONT_MEDIUM  u8g2_font_7x13_tf
#define DISP_FONT_BOLD    u8g2_font_8x13B_tf

/* ── Lifecycle ──────────────────────────────────────────────────────────────*/

/**
 * Power on OLED, run SSD1306 init sequence, clear screen.
 * @return true  if the display initialised and appears to be responding.
 *         false if hardware communication failed (display absent/faulty).
 *         All draw calls are safe no-ops when this returns false.
 */
bool display_init(void);

/** Turn display on (true) or sleep (false). No-op if display unavailable. */
void display_power(bool on);

/** Set contrast 0–255. No-op if display unavailable. */
void display_contrast(uint8_t level);

/**
 * Set the draw colour used by subsequent draw calls.
 * @param color  1 = white (lit pixels), 0 = black (dark pixels / erase).
 * Default: 1 (white).
 */
void display_set_color(uint8_t color);

/* ── Availability ───────────────────────────────────────────────────────────*/

/** Returns true if the OLED was detected and initialised successfully. */
bool display_is_available(void);

/**
 * Called by i_sensors_init() after its I2C bus scan.
 * Sets the definitive hardware-detected available flag.
 * If available=false, all subsequent draw calls become no-ops.
 */
void display_set_available(bool available);

/**
 * Re-synchronise the u8g2 HAL with a freshly created I2C master bus.
 *
 * Call after i_sensors_init() has called i2c_new_master_bus() post-WiFi.
 * Internally calls u8g2_esp32_hal_reinit_bus() to rebuild bus/device handles,
 * then re-sends the SSD1306 init sequence (GDDRAM contents preserved).
 *
 * @return ESP_OK on success.
 *         ESP_ERR_INVALID_STATE if display_init() has not run yet.
 *         Propagated esp_err_t from u8g2_esp32_hal_reinit_bus() on failure.
 */
esp_err_t display_reinit_i2c(i2c_master_bus_handle_t new_bus);

/* ── Escape hatch ───────────────────────────────────────────────────────────*/

/**
 * Return the raw u8g2 handle for calls not covered by the wrappers below.
 * Call display_mark_dirty() after drawing directly via this handle.
 * The mutex is NOT held while you use this handle — use with care.
 */
u8g2_t *display_get_u8g2(void);

/* ── Dirty flag ─────────────────────────────────────────────────────────────*/
void display_mark_dirty(void);
void display_clear_dirty(void);
bool display_is_dirty(void);

/* ── Draw calls — all mark dirty, none flush ────────────────────────────────
 * All functions are no-ops if the display is not available.
 */

/** Clear the entire frame buffer to black. */
void display_clear(void);

/** Fill a rectangle with black (erase without a full clear). */
void display_clear_region(int x, int y, int w, int h);

/**
 * Draw a string at (x, y) using the given font.
 * y is from the top of the glyph (FontPosTop is set in init).
 * Returns x + pixel advance so calls can be chained.
 */
int display_draw_text(int x, int y, const char *str, const uint8_t *font);

/** Fill or clear a rectangle (on=true → white, false → black). */
void display_fill_rect(int x, int y, int w, int h, bool on);

/** Draw a 1-pixel rectangle outline in the current draw colour. */
void display_draw_rect(int x, int y, int w, int h);

/** Draw a horizontal line of pixels in the current draw colour. */
void display_draw_hline(int x, int y, int len);

/** Draw a vertical line of pixels in the current draw colour. */
void display_draw_vline(int x, int y, int len);

/**
 * Render a QR code from a qrcodegen buffer.
 * @param x, y   Top-left corner including quiet zone.
 * @param scale  Pixels per module (2 → 42 px for v1 QR, fits left panel).
 * @param qrcode Buffer produced by qrcodegen_encodeText() / qr_gen_*.
 */
void display_draw_qr(int x, int y, int scale, const uint8_t *qrcode);

/* ── Flush ──────────────────────────────────────────────────────────────────*/

/** Push frame buffer to GDDRAM if dirty; clear dirty flag. No-op if unavailable. */
void display_flush(void);

/** Push frame buffer unconditionally; clear dirty flag. No-op if unavailable. */
void display_flush_force(void);
