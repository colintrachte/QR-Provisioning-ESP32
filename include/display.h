#pragma once
/**
 * display.h — u8g2-backed SSD1306 OLED driver for Heltec WiFi LoRa 32 V3.
 *
 * Wraps u8g2 with a "draw into RAM, flush on demand" contract:
 *
 *   - All draw_* calls write to u8g2's full-frame buffer (1024 bytes in RAM).
 *     No I2C traffic until display_flush() is called.
 *   - display_flush() pushes the buffer to SSD1306 GDDRAM only when the
 *     dirty flag is set, then clears it.  GDDRAM holds the image with no
 *     CPU involvement until the next flush.
 *   - display_flush_force() pushes unconditionally (useful after power-on).
 *
 * Pin assignments — Heltec WiFi LoRa 32 V3
 * ──────────────────────────────────────────
 *   SDA = GPIO17   SCL = GPIO18   RST = GPIO21   Vext = GPIO36
 */

#include <stdbool.h>
#include <stdint.h>
#include "u8g2.h"
#include "u8g2_esp32_hal.h"

/* ── Hardware pin assignments ───────────────────────────────────────────*/
#define DISP_PIN_SDA   17
#define DISP_PIN_SCL   18
#define DISP_PIN_RST   21
#define DISP_PIN_VEXT  36

/** I2C address in the left-shifted form that u8g2 expects (0x3C << 1). */
#define DISP_I2C_ADDR  0x78

/* ── Panel geometry ─────────────────────────────────────────────────────*/
#define DISP_WIDTH   128
#define DISP_HEIGHT   64

/* ── Font aliases ───────────────────────────────────────────────────────
 * Any u8g2 font pointer is valid; these four cover the provisioning UI.
 * Pass them directly to display_draw_text().
 */
#define DISP_FONT_SMALL   u8g2_font_5x7_tf    /*  5 px wide, 7 px tall  */
#define DISP_FONT_NORMAL  u8g2_font_6x10_tf   /*  6 px wide, 10 px tall */
#define DISP_FONT_MEDIUM  u8g2_font_7x13_tf   /*  7 px wide, 13 px tall */
#define DISP_FONT_BOLD    u8g2_font_8x13B_tf  /*  8 px wide, 13 px tall */

/* ── Lifecycle ──────────────────────────────────────────────────────────*/

/** Power on OLED, run SSD1306 init sequence, clear screen. */
void display_init(void);

/** Turn display power on (true) or off/sleep (false). */
void display_power(bool on);

/** Set contrast 0–255 (hardware default is ~127). */
void display_contrast(uint8_t level);

/* ── Escape hatch ───────────────────────────────────────────────────────*/

/**
 * Return the raw u8g2 handle for any u8g2 call not covered by the wrappers
 * below.  The dirty flag is NOT set — call display_mark_dirty() if you
 * draw directly via this handle.
 */
u8g2_t *display_get_u8g2(void);

/* ── Dirty flag ─────────────────────────────────────────────────────────*/
void display_mark_dirty(void);
void display_clear_dirty(void);
bool display_is_dirty(void);

/* ── Draw calls — all mark dirty, none flush ────────────────────────────*/

/** Clear the entire frame buffer to black. */
void display_clear(void);

/** Fill a rectangle with black (erase a region without a full clear). */
void display_clear_region(int x, int y, int w, int h);

/**
 * Draw a string at (x, y) using the given font.
 * y is measured from the top of the glyph (FontPosTop is set in init).
 * Returns the x coordinate immediately after the last character, so
 * calls can be chained: x = display_draw_text(x, y, "Hello ", FONT_NORMAL);
 *                        x = display_draw_text(x, y, "World", FONT_BOLD);
 */
int display_draw_text(int x, int y, const char *str, const uint8_t *font);

/** Fill or clear a rectangle (on=true → white pixels, false → black). */
void display_fill_rect(int x, int y, int w, int h, bool on);

/** Draw a 1-pixel white rectangle outline. */
void display_draw_rect(int x, int y, int w, int h);

/** Draw a horizontal line of white pixels. */
void display_draw_hline(int x, int y, int len);

/** Draw a vertical line of white pixels. */
void display_draw_vline(int x, int y, int len);

/**
 * Render a QR code from a qrcodegen buffer.
 * @param x, y   Top-left corner of the QR image (including quiet zone).
 * @param scale  Pixels per module (2 = 42 px for a v1 QR, fits left panel).
 * @param qrcode Buffer produced by qrcodegen_encodeText() / qr_gen_*.
 */
void display_draw_qr(int x, int y, int scale, const uint8_t *qrcode);

/* ── Flush ──────────────────────────────────────────────────────────────*/

/** Push frame buffer to GDDRAM if dirty, then clear the dirty flag. */
void display_flush(void);

/** Push frame buffer to GDDRAM unconditionally, then clear the dirty flag. */
void display_flush_force(void);
