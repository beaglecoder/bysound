/* Symbols to bytes: header, body, RS correction and CRC verification. Meant to be fed by a streaming demodulator. */
#include "bysound.h"
#include <string.h>

enum { ST_HEADER = 0, ST_BODY = 1, ST_DONE = 2, ST_ERROR = 3 };

int bys_header_parse(const uint8_t h[BYS_HEADER_BYTES], bys_band_t band, bys_profile_t profile, uint8_t *magic, uint8_t *length, uint8_t *set) {
  uint8_t v = h[3];
  uint8_t crc = bys_crc8((uint8_t[]){ h[0], h[1], v, (uint8_t)(profile == BYS_ROBUST) }, 4);
  if (crc != h[2] || (v >> 4) != BYS_VERSION || ((v >> 1) & 7) != (unsigned)band) return BYS_E_HEADER;
  if (h[0] != BYS_MAGIC_PAYLOAD && h[0] != BYS_MAGIC_REFERENCE && h[0] != BYS_MAGIC_ACK && h[0] != BYS_MAGIC_RISK) return BYS_E_HEADER;
  *magic = h[0];
  *length = h[1];
  *set = v & 1;
  return BYS_OK;
}

int bys_block_decode(uint8_t *block, size_t len, const uint8_t *erasures, size_t nerr, uint8_t *index, uint8_t *total, size_t *chunk_len) {
  if (len <= BYS_BLOCK_HEADER + BYS_BLOCK_PARITY || len > BYS_BLOCK_BYTES) return BYS_E_FORMAT;
  int r = bys_rs_decode(block, len, BYS_BLOCK_PARITY, erasures, nerr);
  if (r < 0) return r;
  if (block[1] < 1 || block[0] >= block[1]) return BYS_E_FORMAT;
  *index = block[0];
  *total = block[1];
  *chunk_len = len - BYS_BLOCK_HEADER - BYS_BLOCK_PARITY;
  return r;
}

int bys_mother_decode(const uint8_t data14[BYS_REF_DATA], const uint8_t *pa, const uint8_t *pb, uint8_t *fp_out, uint8_t *corrected) {
  uint8_t w[BYS_MOTHER_BYTES], er[BYS_MOTHER_PARITY];
  size_t ne = 0;
  memcpy(w, data14, BYS_REF_DATA);
  const uint8_t *ps[2] = { pa, pb };
  for (unsigned s = 0; s < 2; s++) {
    uint8_t *dst = w + BYS_REF_DATA + s * BYS_REF_PARITY;
    if (ps[s]) memcpy(dst, ps[s], BYS_REF_PARITY);
    else { memset(dst, 0, BYS_REF_PARITY); for (unsigned i = 0; i < BYS_REF_PARITY; i++) er[ne++] = (uint8_t)(BYS_REF_DATA + s * BYS_REF_PARITY + i); }
  }
  uint8_t before[BYS_MOTHER_BYTES];
  memcpy(before, w, sizeof w);
  int r = bys_rs_decode(w, BYS_MOTHER_BYTES, BYS_MOTHER_PARITY, er, ne);
  if (r < 0) return r;
  /* actually corrected: bytes that arrived and changed (filled-in erasures do not count) */
  r = 0;
  for (size_t i = 0; i < BYS_MOTHER_BYTES; i++) {
    int erased = 0;
    for (size_t e = 0; e < ne; e++) erased |= er[e] == i;
    r += !erased && w[i] != before[i];
  }
  if (bys_crc16(w, BYS_REF_BYTES) != (uint16_t)((w[12] << 8) | w[13])) return BYS_E_CRC;
  memcpy(fp_out, w, BYS_REF_BYTES);
  if (corrected) *corrected = (uint8_t)r;
  return BYS_OK;
}

void bys_rx_init(bys_rx_t *rx, bys_band_t band, bys_profile_t profile, uint8_t *out, size_t out_cap) {
  memset(rx, 0, sizeof *rx);
  rx->band = (uint8_t)band;
  rx->profile = (uint8_t)profile;
  rx->out = out;
  rx->out_cap = (uint32_t)out_cap;
  bys_band_info_t bi;
  rx->groups = bys_band_info(band, profile, &bi) == BYS_OK ? bi.groups : 0;
}

static void note(bys_rx_t *rx, int fixed) {
  unsigned c = rx->res.corrected + (unsigned)fixed;
  rx->res.corrected = (uint8_t)(c > 255 ? 255 : c);
}

static int fail(bys_rx_t *rx, int e) { rx->state = ST_ERROR; return e; }

static int finish_small(bys_rx_t *rx) { /* fingerprint, ack and risk */
  bys_result_t *r = &rx->res;
  uint8_t *b = rx->buf;
  r->kind = rx->kind; r->band = rx->band; r->set = rx->set;
  if (rx->kind == BYS_KIND_REFERENCE) {
    uint8_t c = 0;
    int e = bys_mother_decode(b, rx->set ? NULL : b + BYS_REF_DATA, rx->set ? b + BYS_REF_DATA : NULL, r->fingerprint, &c);
    if (e < 0) return fail(rx, e);
    note(rx, c);
  } else {
    unsigned dl = rx->kind == BYS_KIND_ACK ? 5 : 8, nsym = rx->kind == BYS_KIND_ACK ? BYS_ACK_PARITY : BYS_RISK_PARITY;
    int e = bys_rs_decode(b, dl + 2 + nsym, nsym, NULL, 0);
    if (e < 0) return fail(rx, e);
    note(rx, e);
    if (bys_crc16(b, dl) != (uint16_t)((b[dl] << 8) | b[dl + 1])) return fail(rx, BYS_E_CRC);
    if (rx->kind == BYS_KIND_ACK) {
      if (b[0] != 2 && b[0] != 3) return fail(rx, BYS_E_FORMAT);
      r->receipt = b[0] == 3;
      memcpy(r->hash4, b + 1, 4);
    } else {
      r->reason = b[0]; r->hops = b[1] >> 4; r->source = b[1] & 15;
      memcpy(r->own, b + 2, 3);
      memcpy(r->intruder, b + 5, 3);
    }
  }
  rx->state = ST_DONE;
  return BYS_RX_DONE;
}

static int finish_payload(bys_rx_t *rx) {
  uint32_t n = rx->out_n;
  if (n <= 4) return fail(rx, BYS_E_FORMAT);
  uint32_t want = ((uint32_t)rx->out[n - 4] << 24) | ((uint32_t)rx->out[n - 3] << 16) | ((uint32_t)rx->out[n - 2] << 8) | rx->out[n - 1];
  if (bys_crc32(rx->out, n - 4) != want) return fail(rx, BYS_E_CRC);
  rx->res.kind = BYS_KIND_PAYLOAD; rx->res.band = rx->band;
  rx->res.payload_len = (uint16_t)(n - 4);
  rx->state = ST_DONE;
  return BYS_RX_DONE;
}

/* Closes the block in buf[0..len). */
static int close_block(bys_rx_t *rx, size_t len) {
  uint8_t index, total;
  size_t chunk;
  int e = bys_block_decode(rx->buf, len, NULL, 0, &index, &total, &chunk);
  if (e < 0) return e;
  if (index != rx->blk || total != rx->total) return BYS_E_FORMAT;
  if (rx->blk + 1 < rx->total && chunk != BYS_BLOCK_DATA) return BYS_E_FORMAT;
  if (rx->out_n + chunk > rx->out_cap || !rx->out) return BYS_E_SPACE;
  memcpy(rx->out + rx->out_n, rx->buf + BYS_BLOCK_HEADER, chunk);
  rx->out_n += (uint32_t)chunk;
  note(rx, e);
  rx->blk++;
  rx->buf_n = 0;
  return BYS_OK;
}

/* CRC-32 of the packed bytes a[0..na) followed by b[0..nb): the last 4 bytes of the whole are the CRC of the rest. */
static int crc_ok(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
  const size_t n = na + nb;
  if (n <= 4) return 0;
  uint32_t c = 0xffffffffu, want = 0;
  for (size_t i = 0; i < n; i++) {
    const uint8_t x = i < na ? a[i] : b[i - na];
    if (i < n - 4) c = bys_crc32_update(c, x); else want = (want << 8) | x;
  }
  return (c ^ 0xffffffffu) == want;
}

/* The last block closed at `len`: 1 if RS accepts it with the expected index and total and the payload's CRC-32 checks out. RS alone
 * cannot tell the length: if the last parity byte is 0x00, the codeword without it is also a codeword (c(x) = x * c'(x)). */
static int last_fits(const bys_rx_t *rx, size_t len, uint8_t *tmp, size_t *chunk, int *fixed, int *rs_ok) {
  uint8_t index, total;
  memcpy(tmp, rx->buf, len);
  const int e = bys_block_decode(tmp, len, NULL, 0, &index, &total, chunk);
  if (e < 0 || index != rx->blk || total != rx->total) return 0;
  *fixed = e;
  if (!rx->out || rx->out_n + *chunk > rx->out_cap) { *rs_ok = 2; return 0; }
  if (*rs_ok < 1) *rs_ok = 1;
  return crc_ok(rx->out, rx->out_n, tmp + BYS_BLOCK_HEADER, *chunk);
}

int bys_rx_last_fits(const bys_rx_t *rx, size_t len) {
  uint8_t tmp[BYS_BLOCK_BYTES];
  size_t chunk;
  int fixed, rs_ok = 0;
  if (rx->state != ST_BODY || rx->kind != BYS_KIND_PAYLOAD || rx->blk + 1 != rx->total || len > rx->buf_n) return 0;
  return last_fits(rx, len, tmp, &chunk, &fixed, &rs_ok);
}

/* Closes the last block with the first length, from shortest to longest, that RS accepts and the CRC-32 confirms. (Going downwards
 * does not work: appending zeros to a codeword with roots alpha^0.. gives another valid codeword, and the padding of the last symbol
 * is zeros.) */
static int close_last(bys_rx_t *rx, size_t avail) {
  uint8_t tmp[BYS_BLOCK_BYTES];
  int rs_ok = 0;
  for (size_t len = BYS_BLOCK_HEADER + BYS_BLOCK_PARITY + 1; len <= avail; len++) {
    size_t chunk;
    int fixed;
    if (!last_fits(rx, len, tmp, &chunk, &fixed, &rs_ok)) continue;
    memcpy(rx->out + rx->out_n, tmp + BYS_BLOCK_HEADER, chunk);
    rx->out_n += (uint32_t)chunk;
    note(rx, fixed);
    rx->blk++;
    rx->buf_n = 0;
    return finish_payload(rx);
  }
  return fail(rx, rs_ok == 2 ? BYS_E_SPACE : rs_ok ? BYS_E_CRC : BYS_E_RS);
}

static int body_byte(bys_rx_t *rx, uint8_t byte) {
  rx->buf[rx->buf_n++] = byte;
  if (rx->kind != BYS_KIND_PAYLOAD) {
    unsigned need = rx->kind == BYS_KIND_REFERENCE ? BYS_REF_DATA + BYS_REF_PARITY : rx->kind == BYS_KIND_ACK ? BYS_ACK_BODY : BYS_RISK_BODY;
    return rx->buf_n == need ? finish_small(rx) : BYS_RX_MORE;
  }
  if (rx->buf_n < BYS_BLOCK_BYTES) return BYS_RX_MORE;
  if (rx->blk + 1 == rx->total) return close_last(rx, BYS_BLOCK_BYTES);
  int e = close_block(rx, BYS_BLOCK_BYTES);
  if (e < 0) return fail(rx, e);
  return rx->blk == rx->total ? finish_payload(rx) : BYS_RX_MORE;
}

int bys_rx_push(bys_rx_t *rx, const uint8_t *nib) {
  if (!rx->groups) return BYS_E_ARG;
  if (rx->state == ST_DONE) return BYS_RX_DONE;
  if (rx->state == ST_ERROR) return BYS_E_FORMAT;
  if (rx->state == ST_HEADER) {
    for (unsigned c = 0; c < rx->groups && rx->hdr_n < 2 * BYS_HEADER_BYTES; c++, rx->hdr_n++)
      rx->hdr[rx->hdr_n >> 1] = (uint8_t)((rx->hdr_n & 1) ? (rx->hdr[rx->hdr_n >> 1] | (nib[c] & 15)) : ((nib[c] & 15) << 4));
    if (rx->hdr_n < 2 * BYS_HEADER_BYTES) return BYS_RX_MORE;
    int e = bys_header_parse(rx->hdr, (bys_band_t)rx->band, (bys_profile_t)rx->profile, &rx->kind, &rx->length, &rx->set);
    if (e < 0) return fail(rx, e);
    if ((rx->kind == BYS_KIND_REFERENCE && rx->length != BYS_REF_DATA) || (rx->kind == BYS_KIND_ACK && rx->length != BYS_ACK_LENGTH) ||
        (rx->kind == BYS_KIND_RISK && rx->length != BYS_RISK_LENGTH) || (rx->kind == BYS_KIND_PAYLOAD && rx->length < 1))
      return fail(rx, BYS_E_FORMAT);
    if (rx->kind == BYS_KIND_PAYLOAD) rx->total = rx->length;
    rx->state = ST_BODY;
    return BYS_RX_MORE;
  }
  for (unsigned c = 0; c < rx->groups; c++) {
    if (!rx->half) { rx->cur = (uint8_t)((nib[c] & 15) << 4); rx->half = 1; continue; }
    rx->half = 0;
    int r = body_byte(rx, (uint8_t)(rx->cur | (nib[c] & 15)));
    if (r != BYS_RX_MORE) return r; /* finished (the rest of the symbol is padding) or error */
  }
  return BYS_RX_MORE;
}

int bys_rx_finish(bys_rx_t *rx) {
  if (rx->state == ST_DONE) return BYS_RX_DONE;
  if (rx->state != ST_BODY || rx->kind != BYS_KIND_PAYLOAD) return BYS_E_INCOMPLETE;
  /* The last block does not say how long it is (see close_last). */
  if (rx->blk + 1 != rx->total) return fail(rx, BYS_E_INCOMPLETE);
  return close_last(rx, rx->buf_n);
}

const bys_result_t *bys_rx_result(const bys_rx_t *rx) { return &rx->res; }
int bys_rx_header_ready(const bys_rx_t *rx) { return rx->state >= ST_BODY ? rx->kind : 0; }
