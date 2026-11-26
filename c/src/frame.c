/* v4 frame: header, body with parity, and symbols, generated one at a time. */
#include "bysound.h"
#include <string.h>

static int setup(bys_frame_t *f, bys_band_t band, bys_profile_t profile, bys_kind_t kind, unsigned length, unsigned set, unsigned body_bytes) {
  bys_band_info_t bi;
  int r = bys_band_info(band, profile, &bi);
  if (r < 0) return r;
  memset(f, 0, sizeof *f);
  uint8_t version = (uint8_t)((BYS_VERSION << 4) | ((unsigned)band << 1) | (set & 1));
  f->hdr[0] = (uint8_t)kind;
  f->hdr[1] = (uint8_t)length;
  f->hdr[2] = bys_crc8((uint8_t[]){ (uint8_t)kind, (uint8_t)length, version, (uint8_t)(profile == BYS_ROBUST) }, 4);
  f->hdr[3] = version;
  f->band = (uint8_t)band;
  f->profile = (uint8_t)profile;
  f->kind = (uint8_t)kind;
  f->groups = bi.groups;
  f->hdr_syms = (uint8_t)((2 * BYS_HEADER_BYTES + bi.groups - 1) / bi.groups);
  f->body_left = (uint16_t)body_bytes;
  f->sym_total = (uint16_t)(BYS_PREAMBLE_SYMBOLS + f->hdr_syms + (2 * body_bytes + bi.groups - 1) / bi.groups);
  return BYS_OK;
}

/* Symbols (preamble + header + body) of a payload frame with `packed` packed bytes. */
static uint32_t payload_symbols(unsigned groups, uint32_t packed) {
  uint32_t len = packed + 4, total = (len + BYS_BLOCK_DATA - 1) / BYS_BLOCK_DATA;
  uint32_t body = len + total * (BYS_BLOCK_HEADER + BYS_BLOCK_PARITY);
  return BYS_PREAMBLE_SYMBOLS + (2 * BYS_HEADER_BYTES + groups - 1) / groups + (2 * body + groups - 1) / groups;
}

/* Whole transmission (frame + default gap) within the cap? Exact, in microseconds. */
static int fits(unsigned groups, uint32_t symbol_us, uint32_t packed) {
  uint64_t us = (uint64_t)payload_symbols(groups, packed) * symbol_us + (uint64_t)BYS_GAP_MS * 1000u;
  return us <= (uint64_t)BYS_MAX_FRAME_SECONDS * 1000000u;
}

int bys_max_payload_bytes(bys_band_t band, bys_profile_t profile) {
  bys_band_info_t bi;
  int r = bys_band_info(band, profile, &bi);
  if (r < 0) return r;
  uint32_t lo = 0, hi = 48u * BYS_MAX_BLOCKS - 4; /* binary search: fits() is monotonic */
  if (!fits(bi.groups, bi.symbol_us, 0)) return 0;
  if (fits(bi.groups, bi.symbol_us, hi)) return (int)hi;
  while (hi - lo > 1) {
    uint32_t mid = (lo + hi) / 2;
    if (fits(bi.groups, bi.symbol_us, mid)) lo = mid; else hi = mid;
  }
  return (int)lo;
}

int bys_frame_init_payload(bys_frame_t *f, bys_band_t band, bys_profile_t profile, const char *utf8, size_t n) {
  if (!f || (!utf8 && n)) return BYS_E_ARG;
  /* First pass: packed length and CRC-32, storing nothing. */
  bys_packer_t p;
  bys_packer_init(&p, utf8, n);
  uint32_t crc = 0xffffffffu, len = 4;
  int b;
  while ((b = bys_packer_next(&p)) >= 0) { crc = bys_crc32_update(crc, (uint8_t)b); len++; }
  crc ^= 0xffffffffu;
  uint32_t total = (len + BYS_BLOCK_DATA - 1) / BYS_BLOCK_DATA;
  if (total > BYS_MAX_BLOCKS) return BYS_E_TOO_LONG;
  { /* hard cap on the duration of one transmission */
    bys_band_info_t bi;
    int rb = bys_band_info(band, profile, &bi);
    if (rb < 0) return rb;
    if (!fits(bi.groups, bi.symbol_us, len - 4)) return BYS_E_TOO_LONG;
  }
  int r = setup(f, band, profile, BYS_KIND_PAYLOAD, total, 0, len + total * (BYS_BLOCK_HEADER + BYS_BLOCK_PARITY));
  if (r < 0) return r;
  f->total = (uint8_t)total;
  f->crc = crc;
  bys_packer_init(&f->pk, utf8, n);
  return BYS_OK;
}

int bys_frame_init_reference(bys_frame_t *f, bys_band_t band, bys_profile_t profile, unsigned set, const uint8_t fp[BYS_REF_BYTES]) {
  if (!f || !fp || set > 1) return BYS_E_ARG;
  int r = setup(f, band, profile, BYS_KIND_REFERENCE, BYS_REF_DATA, set, BYS_REF_DATA + BYS_REF_PARITY);
  if (r < 0) return r;
  uint8_t par[BYS_MOTHER_PARITY];
  memcpy(f->buf, fp, BYS_REF_BYTES);
  uint16_t c = bys_crc16(fp, BYS_REF_BYTES);
  f->buf[12] = (uint8_t)(c >> 8);
  f->buf[13] = (uint8_t)c;
  bys_rs_encode(f->buf, BYS_REF_DATA, par, BYS_MOTHER_PARITY);
  memcpy(f->buf + BYS_REF_DATA, par + set * BYS_REF_PARITY, BYS_REF_PARITY);
  f->buf_len = BYS_REF_DATA + BYS_REF_PARITY;
  return BYS_OK;
}

static void with_crc_and_parity(bys_frame_t *f, unsigned data_len, unsigned nsym) {
  uint16_t c = bys_crc16(f->buf, data_len);
  f->buf[data_len] = (uint8_t)(c >> 8);
  f->buf[data_len + 1] = (uint8_t)c;
  bys_rs_encode(f->buf, data_len + 2, f->buf + data_len + 2, nsym);
  f->buf_len = (uint8_t)(data_len + 2 + nsym);
}

int bys_frame_init_ack_heard(bys_frame_t *f, bys_band_t band, int receipt, unsigned heard, const uint8_t hash4[4]) {
  if (!f || !hash4 || heard > 15 || (heard && (heard & 7) == 0)) return BYS_E_ARG; /* 0x8 (robust, channel -1) is invalid */
  int r = setup(f, band, BYS_NORMAL, BYS_KIND_ACK, BYS_ACK_LENGTH, 0, BYS_ACK_BODY);
  if (r < 0) return r;
  f->buf[0] = (uint8_t)((heard << 4) | (receipt ? 3 : 2));
  memcpy(f->buf + 1, hash4, 4);
  with_crc_and_parity(f, 5, BYS_ACK_PARITY);
  return BYS_OK;
}

int bys_frame_init_ack(bys_frame_t *f, bys_band_t band, int receipt, const uint8_t hash4[4]) {
  return bys_frame_init_ack_heard(f, band, receipt, 0, hash4);
}

int bys_frame_init_risk(bys_frame_t *f, bys_band_t band, unsigned reason, unsigned source, unsigned hops, const uint8_t own[3], const uint8_t intruder[3]) {
  if (!f || !own || !intruder || reason > 255 || source > 15 || hops > 15) return BYS_E_ARG;
  int r = setup(f, band, BYS_NORMAL, BYS_KIND_RISK, BYS_RISK_LENGTH, 0, BYS_RISK_BODY);
  if (r < 0) return r;
  f->buf[0] = (uint8_t)reason;
  f->buf[1] = (uint8_t)((hops << 4) | source);
  memcpy(f->buf + 2, own, 3);
  memcpy(f->buf + 5, intruder, 3);
  with_crc_and_parity(f, 8, BYS_RISK_PARITY);
  return BYS_OK;
}

int bys_frame_duration_us(bys_kind_t kind, bys_band_t band, bys_profile_t profile, uint32_t packed_len, uint32_t *us) {
  bys_band_info_t bi;
  int r = bys_band_info(band, profile, &bi);
  if (r < 0) return r;
  if (!us) return BYS_E_ARG;
  uint32_t body, sym;
  switch (kind) {
    case BYS_KIND_PAYLOAD: sym = payload_symbols(bi.groups, packed_len); break;
    case BYS_KIND_REFERENCE: body = BYS_REF_DATA + BYS_REF_PARITY; goto small;
    case BYS_KIND_ACK: body = BYS_ACK_BODY; goto small;
    case BYS_KIND_RISK: body = BYS_RISK_BODY; goto small;
    default: return BYS_E_ARG;
  }
  *us = sym * bi.symbol_us;
  return BYS_OK;
small:
  *us = (BYS_PREAMBLE_SYMBOLS + (2 * BYS_HEADER_BYTES + bi.groups - 1) / bi.groups + (2 * body + bi.groups - 1) / bi.groups) * bi.symbol_us;
  return BYS_OK;
}

size_t bys_frame_symbols(const bys_frame_t *f) { return f->sym_total; }

static int data_next(bys_frame_t *f) { /* packed bytes + CRC-32 */
  int c = bys_packer_next(&f->pk);
  if (c >= 0) return c;
  if (f->crc_i < 4) return (int)((f->crc >> (24 - 8 * f->crc_i++)) & 255);
  return -1;
}

static void next_block(bys_frame_t *f) {
  unsigned n = 0;
  f->buf[0] = f->blk;
  f->buf[1] = f->total;
  while (n < BYS_BLOCK_DATA) {
    int c = data_next(f);
    if (c < 0) break;
    f->buf[BYS_BLOCK_HEADER + n++] = (uint8_t)c;
  }
  bys_rs_encode(f->buf, BYS_BLOCK_HEADER + n, f->buf + BYS_BLOCK_HEADER + n, BYS_BLOCK_PARITY);
  f->buf_len = (uint8_t)(BYS_BLOCK_HEADER + n + BYS_BLOCK_PARITY);
  f->buf_pos = 0;
  f->blk++;
}

static uint8_t body_nibble(bys_frame_t *f) {
  if (f->half) { f->half = 0; return f->cur & 15; }
  if (!f->body_left) return 0; /* padding up to the end of the symbol */
  if (f->buf_pos >= f->buf_len && f->kind == BYS_KIND_PAYLOAD) next_block(f);
  f->cur = f->buf[f->buf_pos++];
  f->body_left--;
  f->half = 1;
  return f->cur >> 4;
}

int bys_frame_next(bys_frame_t *f, uint8_t *nib) {
  unsigned k = f->sym_k;
  if (k >= f->sym_total) return 0;
  if (k < BYS_PREAMBLE_SYMBOLS) bys_preamble_symbol((bys_band_t)f->band, (bys_profile_t)f->profile, k, nib);
  else if (k < BYS_PREAMBLE_SYMBOLS + f->hdr_syms) {
    for (unsigned c = 0; c < f->groups; c++) {
      unsigned idx = (k - BYS_PREAMBLE_SYMBOLS) * f->groups + c;
      nib[c] = idx < 2 * BYS_HEADER_BYTES ? (uint8_t)((f->hdr[idx >> 1] >> ((idx & 1) ? 0 : 4)) & 15) : 0;
    }
  } else for (unsigned c = 0; c < f->groups; c++) nib[c] = body_nibble(f);
  f->sym_k++;
  return 1;
}
