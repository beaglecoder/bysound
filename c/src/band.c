/* Bands, preamble and CRCs of bysound v4. */
#include "bysound.h"

static const bys_band_info_t BANDS[BYS_BAND_COUNT] = {
  /* bin, groups, amp, symbol_us, window_start_us */
  { 384, 2, 300, 32000, 4000 }, /* top */
  { 368, 3, 300, 32000, 4000 }, /* high */
  { 352, 3, 300, 32000, 4000 }, /* upper */
  { 320, 3, 300, 32000, 4000 }, /* mid */
  {  40, 4, 200, 40000, 5000 }, /* audible */
  { 352, 4, 250, 32000, 4000 }, /* xhigh */
  { 320, 6, 200, 32000, 4000 }, /* wide */
};

static const uint8_t PREAMBLE[8][4] = {
  { 0, 8, 4, 12 }, { 15, 7, 11, 3 }, { 2, 10, 6, 14 }, { 13, 5, 9, 1 },
  { 4, 12, 0, 8 }, { 11, 3, 15, 7 }, { 6, 14, 2, 10 }, { 9, 1, 13, 5 },
};
/* Key [column shift, nibble offset] per band. */
static const uint8_t KEY_NORMAL[BYS_BAND_COUNT][2] = { { 0, 3 }, { 3, 9 }, { 2, 14 }, { 1, 13 }, { 0, 0 }, { 1, 5 }, { 3, 14 } };
static const uint8_t KEY_ROBUST[4][2] = { { 1, 13 }, { 3, 11 }, { 0, 8 }, { 2, 9 } }; /* top, high, upper, mid */

int bys_band_info(bys_band_t band, bys_profile_t profile, bys_band_info_t *out) {
  if ((unsigned)band >= BYS_BAND_COUNT || !out) return BYS_E_ARG;
  if (profile != BYS_NORMAL && profile != BYS_ROBUST) return BYS_E_ARG;
  *out = BANDS[band];
  if (profile == BYS_ROBUST) {
    if (band > BYS_MID) return BYS_E_PROFILE;
    out->symbol_us *= 2;
    out->window_start_us *= 2;
  }
  return BYS_OK;
}

int bys_preamble_symbol(bys_band_t band, bys_profile_t profile, unsigned k, uint8_t *nib) {
  if ((unsigned)band >= BYS_BAND_COUNT || k >= BYS_PREAMBLE_SYMBOLS || !nib) return BYS_E_ARG;
  if (profile == BYS_ROBUST && band > BYS_MID) return BYS_E_PROFILE;
  const uint8_t *key = profile == BYS_ROBUST ? KEY_ROBUST[band] : KEY_NORMAL[band];
  for (unsigned g = 0; g < BANDS[band].groups; g++) nib[g] = (uint8_t)((PREAMBLE[k][(g + key[0]) % 4] + key[1]) & 15);
  return BYS_OK;
}

uint8_t bys_crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0xff;
  while (n--) {
    c ^= *d++;
    for (int i = 0; i < 8; i++) c = (uint8_t)((c & 0x80) ? ((c << 1) ^ 0x2f) : (c << 1));
  }
  return c;
}

uint16_t bys_crc16_update(uint16_t crc, uint8_t b) {
  crc ^= (uint16_t)(b << 8);
  for (int i = 0; i < 8; i++) crc = (uint16_t)((crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1));
  return crc;
}
uint16_t bys_crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xffff;
  while (n--) c = bys_crc16_update(c, *d++);
  return c;
}

uint32_t bys_crc32_update(uint32_t c, uint8_t b) {
  c ^= b;
  for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
  return c;
}
uint32_t bys_crc32(const uint8_t *d, size_t n) {
  uint32_t c = 0xffffffffu;
  while (n--) c = bys_crc32_update(c, *d++);
  return c ^ 0xffffffffu;
}
