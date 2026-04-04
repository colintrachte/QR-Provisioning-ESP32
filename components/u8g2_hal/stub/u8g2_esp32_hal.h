#pragma once
/* Stub — clone u8g2 and hal submodules first. See component CMakeLists.txt. */
#include <stdint.h>
#include <stddef.h>

typedef struct { uint8_t _[1024]; } u8g2_t;
typedef uint8_t (*u8x8_msg_cb)(void*, uint8_t, uint8_t, void*);

static inline void u8g2_Setup_ssd1306_i2c_128x64_noname_f(
    u8g2_t *u, int r, u8x8_msg_cb b, u8x8_msg_cb d)
{ (void)u;(void)r;(void)b;(void)d; }
static inline void u8g2_InitDisplay(u8g2_t *u)        { (void)u; }
static inline void u8g2_SetPowerSave(u8g2_t *u, int v){ (void)u;(void)v; }
static inline void u8g2_ClearBuffer(u8g2_t *u)        { (void)u; }
static inline void u8g2_SendBuffer(u8g2_t *u)         { (void)u; }
static inline void u8g2_SetFont(u8g2_t *u, const void *f){ (void)u;(void)f; }
static inline void u8g2_DrawStr(u8g2_t *u, int x, int y, const char *s){ (void)u;(void)x;(void)y;(void)s; }
static inline void u8g2_DrawPixel(u8g2_t *u, int x, int y){ (void)u;(void)x;(void)y; }
static inline void u8g2_DrawBox(u8g2_t *u, int x, int y, int w, int h){ (void)u;(void)x;(void)y;(void)w;(void)h; }
static inline void u8g2_DrawFrame(u8g2_t *u, int x, int y, int w, int h){ (void)u;(void)x;(void)y;(void)w;(void)h; }
static inline void u8g2_DrawHLine(u8g2_t *u, int x, int y, int w){ (void)u;(void)x;(void)y;(void)w; }
static inline void u8g2_DrawVLine(u8g2_t *u, int x, int y, int h){ (void)u;(void)x;(void)y;(void)h; }
static inline void u8g2_SetDrawColor(u8g2_t *u, int c){ (void)u;(void)c; }
static inline void u8x8_SetI2CAddress(void *u, int a) { (void)u;(void)a; }
static inline int  u8g2_GetDisplayWidth(u8g2_t *u)    { (void)u; return 128; }
static inline int  u8g2_GetDisplayHeight(u8g2_t *u)   { (void)u; return 64; }
static inline int  u8g2_GetStrWidth(u8g2_t *u, const char *s){ (void)u;(void)s; return 0; }

/* Font stubs */
static const uint8_t u8g2_font_6x10_tf[1]  = {0};
static const uint8_t u8g2_font_7x13_tf[1]  = {0};
static const uint8_t u8g2_font_8x13B_tf[1] = {0};
static const uint8_t u8g2_font_5x7_tf[1]   = {0};

/* HAL stubs */
typedef struct {
    struct { struct { int sda; int scl; } i2c; } bus;
    int reset;
} u8g2_esp32_hal_t;
#define U8G2_ESP32_HAL_DEFAULT  {{{{-1,-1}}},-1}
#define U8G2_R0 0

static inline void u8g2_esp32_hal_init(u8g2_esp32_hal_t h){ (void)h; }
static inline uint8_t u8g2_esp32_i2c_byte_cb(void*a,uint8_t b,uint8_t c,void*d){
    (void)a;(void)b;(void)c;(void)d; return 0; }
static inline uint8_t u8g2_esp32_gpio_and_delay_cb(void*a,uint8_t b,uint8_t c,void*d){
    (void)a;(void)b;(void)c;(void)d; return 0; }
