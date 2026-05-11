/**
 * display.h — SSD1306 OLED abstraction for ESP8266 (Arduino).
 *
 * Wraps u8g2 in a thin layer so the rest of the firmware doesn't need
 * to know about U8G2 types. All coordinates are in pixels.
 *
 * If HAS_OLED is not defined, every call becomes a no-op and no RAM
 * is consumed by the display buffer.
 */

#pragma once
#include <Arduino.h>
#include <qrcodegen.hpp>

#define DISP_WIDTH   128
#define DISP_HEIGHT  64

/* Font references — these are u8g2 font constants. The prov_ui layer
 * uses these symbolic names so it doesn't need u8g2 headers. */
#define DISP_FONT_SMALL   0
#define DISP_FONT_NORMAL  1
#define DISP_FONT_MEDIUM  2
#define DISP_FONT_BOLD    3

bool display_init(void);
void display_power(bool on);
void display_clear(void);
void display_flush(void);
void display_flush_force(void);

void display_set_color(uint8_t color);
void display_draw_text(int x, int y, const char *str,int font_id = DISP_FONT_SMALL);
void display_draw_hline(int x, int y, int len);
void display_draw_vline(int x, int y, int len);
void display_draw_rect(int x, int y, int w, int h);
void display_fill_rect(int x, int y, int w, int h, bool on);
void display_clear_region(int x, int y, int w, int h);

/**
 * Draw a QR code at (x,y) with pixel scale.
 * qrcode must be a buffer produced by qr_gen_wifi() / qr_gen_url().
 */
void display_draw_qr(int x, int y, int scale, const uint8_t *qrcode);

bool display_is_available(void);
void display_set_available(bool available);
