/* Text packing from v4-reference.mjs (digit pairs, ASCII, 0xFF runs of UTF-8). */
#include "bysound.h"

#define IS_DIG(c) ((c) >= 0x30 && (c) <= 0x39)
#define IS_ASC(c) ((c) >= 0x20 && (c) <= 0x7e)

void bys_packer_init(bys_packer_t *p, const char *utf8, size_t n) {
  p->s = (const uint8_t *)utf8;
  p->n = (uint32_t)n;
  p->i = 0;
  p->run = 0;
  p->stage = 0;
}

/* Byte length of the UTF-8 character starting at s[j] (lead byte + the continuation bytes actually present). */
static uint32_t char_len(const uint8_t *s, uint32_t j, uint32_t n) {
  uint32_t l = 1;
  if (s[j] >= 0xc0) while (j + l < n && (s[j + l] & 0xc0) == 0x80 && l < 4) l++;
  return l;
}

int bys_packer_next(bys_packer_t *p) {
  if (p->stage == 2) { /* run bytes */
    int b = p->s[p->i++];
    if (--p->run == 0) p->stage = 0;
    return b;
  }
  if (p->stage == 1) { p->stage = 2; return p->run; } /* run length */
  if (p->i >= p->n) return -1;
  uint8_t c = p->s[p->i];
  if (IS_DIG(c) && p->i + 1 < p->n && IS_DIG(p->s[p->i + 1])) {
    int b = 0x80 + (c - 0x30) * 10 + (p->s[p->i + 1] - 0x30);
    p->i += 2;
    return b;
  }
  if (IS_ASC(c)) { p->i++; return c; }
  /* Run of non-ASCII characters up to 255 bytes, never splitting a character. */
  uint32_t len = 0, j = p->i;
  while (j < p->n && !IS_ASC(p->s[j])) {
    uint32_t cl = char_len(p->s, j, p->n);
    if (len + cl > 255) break;
    len += cl;
    j += cl;
  }
  p->run = (uint16_t)len;
  p->stage = 1;
  return 0xff;
}

int bys_pack(const char *utf8, size_t n, uint8_t *out, size_t cap) {
  bys_packer_t p;
  bys_packer_init(&p, utf8, n);
  size_t o = 0;
  int b;
  while ((b = bys_packer_next(&p)) >= 0) {
    if (o >= cap) return BYS_E_SPACE;
    out[o++] = (uint8_t)b;
  }
  return (int)o;
}

/* Strict UTF-8 (no overlong forms, surrogates or values above U+10FFFF), like a fatal TextDecoder. */
static int utf8_ok(const uint8_t *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint8_t c = s[i];
    if (c < 0x80) { i++; continue; }
    size_t l; uint32_t cp, min;
    if ((c & 0xe0) == 0xc0) { l = 2; cp = c & 0x1f; min = 0x80; }
    else if ((c & 0xf0) == 0xe0) { l = 3; cp = c & 0x0f; min = 0x800; }
    else if ((c & 0xf8) == 0xf0) { l = 4; cp = c & 0x07; min = 0x10000; }
    else return 0;
    if (i + l > n) return 0;
    for (size_t k = 1; k < l; k++) {
      if ((s[i + k] & 0xc0) != 0x80) return 0;
      cp = (cp << 6) | (s[i + k] & 0x3f);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    i += l;
  }
  return 1;
}

int bys_unpack(const uint8_t *d, size_t n, char *out, size_t cap) {
  size_t i = 0, o = 0;
  while (i < n) {
    uint8_t b = d[i];
    if (b >= 0x20 && b <= 0x7e) {
      if (o + 1 > cap) return BYS_E_SPACE;
      out[o++] = (char)b; i++;
    } else if (b >= 0x80 && b <= 0xe3) {
      if (o + 2 > cap) return BYS_E_SPACE;
      out[o++] = (char)('0' + (b - 0x80) / 10);
      out[o++] = (char)('0' + (b - 0x80) % 10);
      i++;
    } else if (b == 0xff) {
      /* Consecutive 0xFF runs are a single text (older senders cut every 255 bytes even mid-character). */
      size_t start = o;
      while (i < n && d[i] == 0xff) {
        if (i + 1 >= n) return BYS_E_FORMAT;
        size_t len = d[i + 1];
        if (len == 0 || i + 2 + len > n) return BYS_E_FORMAT;
        if (o + len > cap) return BYS_E_SPACE;
        for (size_t k = 0; k < len; k++) out[o++] = (char)d[i + 2 + k];
        i += 2 + len;
      }
      if (!utf8_ok((const uint8_t *)out + start, o - start)) return BYS_E_FORMAT;
    } else return BYS_E_FORMAT;
  }
  if (o < cap) out[o] = 0;
  return (int)o;
}
