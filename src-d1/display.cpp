/**
 * display.cpp — SSD1306 OLED driver for ESP8266 via u8g2.
 *
 * Uses hardware I2C on D1 (SCL) / D2 (SDA).
 * The u8g2 full-buffer mode (F) allocates 1 KB statically for the
 * framebuffer — acceptable on ESP8266.
 */
#include <qrcodegen.hpp>
#include "display.h"

#ifdef HAS_OLED
  #include <U8g2lib.h>
  #include <Wire.h>

  /* U8G2 full-framebuffer constructor for SSD1306 128x64 I2C */
  static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_u8g2(
      U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

  static bool    s_initialized = false;
  static bool    s_available   = false;
  static uint8_t s_draw_color  = 1;

  /* Font table mapping our IDs to u8g2 font pointers */
  static const uint8_t* font_for_id(int id)
  {
      switch (id) {
          case DISP_FONT_SMALL:  return u8g2_font_6x10_tf;
          case DISP_FONT_NORMAL: return u8g2_font_7x13_tf;
          case DISP_FONT_MEDIUM: return u8g2_font_9x15_tf;
          case DISP_FONT_BOLD:   return u8g2_font_9x15B_tf;
          default:               return u8g2_font_6x10_tf;
      }
  }

  bool display_init(void)
  {
      if (s_initialized) return s_available;

      Wire.begin(D2, D1);           /* SDA, SCL — D2=GPIO4, D1=GPIO5 */
      s_u8g2.setBusClock(400000);   /* 400 kHz */

      if (!s_u8g2.begin()) {
          s_initialized = true;
          s_available   = false;
          Serial.println("[display] OLED not detected");
          return false;
      }

      s_u8g2.setFontPosTop();
      s_u8g2.setFont(font_for_id(DISP_FONT_SMALL));
      s_u8g2.clearBuffer();
      s_u8g2.sendBuffer();

      s_available   = true;
      s_initialized = true;
      Serial.println("[display] SSD1306 ready (128x64, 400 kHz I2C)");
      return true;
  }

  void display_set_available(bool available) { s_available = available; }
  bool display_is_available(void) { return s_available; }

  void display_power(bool on)
  {
      if (!s_available) return;
      s_u8g2.setPowerSave(on ? 0 : 1);
  }

  void display_clear(void)
  {
      if (!s_available) return;
      s_u8g2.clearBuffer();
  }

  void display_flush(void)
  {
      if (!s_available) return;
      s_u8g2.sendBuffer();
  }

  void display_flush_force(void)
  {
      if (!s_available) return;
      s_u8g2.sendBuffer();
  }

  void display_set_color(uint8_t color) { s_draw_color = color ? 1 : 0; }

  void display_draw_text(int x, int y, const char *str, int font_id)
  {
      if (!s_available || !str) return;
      s_u8g2.setFont(font_for_id(font_id));
      s_u8g2.setDrawColor(s_draw_color);
      s_u8g2.drawStr(x, y, str);
  }

  void display_draw_hline(int x, int y, int len)
  {
      if (!s_available) return;
      s_u8g2.setDrawColor(s_draw_color);
      s_u8g2.drawHLine(x, y, len);
  }

  void display_draw_vline(int x, int y, int len)
  {
      if (!s_available) return;
      s_u8g2.setDrawColor(s_draw_color);
      s_u8g2.drawVLine(x, y, len);
  }

  void display_draw_rect(int x, int y, int w, int h)
  {
      if (!s_available) return;
      s_u8g2.setDrawColor(s_draw_color);
      s_u8g2.drawFrame(x, y, w, h);
  }

  void display_fill_rect(int x, int y, int w, int h, bool on)
  {
      if (!s_available) return;
      s_u8g2.setDrawColor(on ? 1 : 0);
      s_u8g2.drawBox(x, y, w, h);
      s_u8g2.setDrawColor(s_draw_color);
  }

  void display_clear_region(int x, int y, int w, int h)
  {
      if (!s_available) return;
      s_u8g2.setDrawColor(0);
      s_u8g2.drawBox(x, y, w, h);
      s_u8g2.setDrawColor(s_draw_color);
  }

    void display_draw_qr(int x, int y, int scale, const qrcodegen::QrCode &qr)
    {
        if (!s_available) return;

        int size = qr.getSize();
        if (size <= 0) return;

        int total = size * scale;

        /* Clear background */
        s_u8g2.setDrawColor(0);
        s_u8g2.drawBox(x, y, total, total);
        s_u8g2.setDrawColor(1);

        for (int qy = 0; qy < size; qy++) {
            for (int qx = 0; qx < size; qx++) {
                if (qr.getModule(qx, qy)) {
                    int px = x + qx * scale;
                    int py = y + qy * scale;
                    if (scale == 1)
                        s_u8g2.drawPixel(px, py);
                    else
                        s_u8g2.drawBox(px, py, scale, scale);
                }
            }
        }
        s_u8g2.setDrawColor(s_draw_color);
    }

#else /* !HAS_OLED */

  bool display_init(void) { return false; }
  void display_set_available(bool) {}
  bool display_is_available(void) { return false; }
  void display_power(bool) {}
  void display_clear(void) {}
  void display_flush(void) {}
  void display_flush_force(void) {}
  void display_set_color(uint8_t) {}
  void display_draw_text(int, int, const char *, int) {}
  void display_draw_hline(int, int, int) {}
  void display_draw_vline(int, int, int) {}
  void display_draw_rect(int, int, int, int) {}
  void display_fill_rect(int, int, int, int, bool) {}
  void display_clear_region(int, int, int, int) {}
  void display_draw_qr(int, int, int, const qrcodegen::QrCode &) {}

#endif /* HAS_OLED */
