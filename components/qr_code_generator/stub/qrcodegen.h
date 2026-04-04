#pragma once
/* Stub — replace by cloning nayuki/QR-Code-generator into
   components/qr_code_generator/nayuki */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum { qrcodegen_Ecc_LOW=0, qrcodegen_Ecc_MEDIUM, qrcodegen_Ecc_QUARTILE, qrcodegen_Ecc_HIGH } qrcodegen_Ecc;
typedef enum { qrcodegen_Mask_AUTO=-1 } qrcodegen_Mask;

#define qrcodegen_VERSION_MIN 1
#define qrcodegen_VERSION_MAX 40
#define qrcodegen_BUFFER_LEN_MAX 3918

static inline bool qrcodegen_encodeText(const char *text, uint8_t *tempBuffer, uint8_t *qrcode,
    qrcodegen_Ecc ecl, int minVersion, int maxVersion, qrcodegen_Mask mask, bool boostEcl)
{ (void)text;(void)tempBuffer;(void)ecl;(void)minVersion;(void)maxVersion;(void)mask;(void)boostEcl;
  qrcode[0]=0; return false; }
static inline int  qrcodegen_getSize(const uint8_t *qrcode) { (void)qrcode; return 0; }
static inline bool qrcodegen_getModule(const uint8_t *qrcode, int x, int y) { (void)qrcode;(void)x;(void)y; return false; }
