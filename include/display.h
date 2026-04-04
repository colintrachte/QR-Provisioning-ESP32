#pragma once
/**
 * display.h — u8g2-backed OLED driver for Heltec WiFi LoRa 32 V3
 *
 * Thin wrapper around u8g2 that preserves the "flush only when dirty"
 * contract from the previous custom driver:
 *
 *   - All draw_* calls write into u8g2's internal full-frame buffer (RAM).
 *     Zero I2C traffic until you call display_flush().
 *   - display_flush() calls u8g2_SendBuffer() only if the dirty flag is set,
 *     then clears it.  The SSD1306 GDDRAM holds the image indefinitely.
 *   - display_mark_dirty() / display_flush() let you batch multiple draw
 *     calls and push once.
 *
 * u8g2 replaces: the custom SSD1306 init sequence, the shadow framebuffer,
 * the 6x8/8x8 font tables, and the I2C write path.
 *
 * Pin assignments (Heltec V3):
 *   SDA=GPIO17  SCL=GPIO18  RST=GPIO21  Vext=GPIO36
 */

#include <stdbool.h>
#include <stdint.h>
#include "u8g2.h"
#include "u8g2_esp32_hal.h"

/* Pins */
#define DISP_PIN_SDA   17
#define DISP_PIN_SCL   18
#define DISP_PIN_RST   21
#define DISP_PIN_VEXT  36

/* I2C address shifted left (u8g2 convention) */
#define DISP_I2C_ADDR  0x78

/* Geometry */
#define DISP_WIDTH   128
#define DISP_HEIGHT   64

/* Recommended fonts - any u8g2 font pointer works */
#define DISP_FONT_SMALL   u8g2_font_5x7_tf
#define DISP_FONT_NORMAL  u8g2_font_6x10_tf
#define DISP_FONT_MEDIUM  u8g2_font_7x13_tf
#define DISP_FONT_BOLD    u8g2_font_8x13B_tf

/* Lifecycle */
void display_init(void);
void display_power(bool on);
void display_contrast(uint8_t level);

/* Raw u8g2 handle - use for any u8g2 call not wrapped below */
u8g2_t *display_u8g2(void);

/* Dirty flag */
void display_mark_dirty(void);
void display_clear_dirty(void);
bool display_is_dirty(void);

/* Drawing wrappers - all mark dirty, none flush */
void display_clear(void);
void display_clear_region(int x, int y, int w, int h);
int  display_draw_text(int x, int y, const char *str, const uint8_t *font);
void display_fill_rect(int x, int y, int w, int h, bool on);
void display_draw_rect(int x, int y, int w, int h);
void display_draw_hline(int x, int y, int len);
void display_draw_vline(int x, int y, int len);
void display_draw_qr(int x, int y, int scale, const uint8_t *qrcode);

/* Flush */
void display_flush(void);        /* flush if dirty, then clear flag   */
void display_flush_force(void);  /* flush unconditionally             */
