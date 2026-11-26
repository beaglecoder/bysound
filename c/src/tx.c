/*
 * bysound v4 transmitter: streaming synthesis. Per sample it is integer-only: 32-bit phase (one cycle = 2^32), sine from a
 * Q15 quarter-wave table with linear interpolation (256 segments per quarter) and a raised-cosine ramp computed as sine^2.
 * Float only in bys_tx_init_* (the level). Equivalent to render() in web/src/modem/modulator.ts.
 */
#include "bysound.h"
#include "bys_tables.h"
#include <string.h>

void bys_tx_cfg_default(bys_tx_cfg_t *cfg) {
  cfg->sample_rate = 44100;
  cfg->level = 1.0f; /* project rule: always transmit at maximum level */
  cfg->gap_ms = BYS_GAP_MS;
}

static int32_t sinq(uint32_t ph) {
  uint32_t x = ph & 0x3fffffffu;
  if (ph & 0x40000000u) x = 0x40000000u - x;
  uint32_t i = x >> 22, fr = (x >> 6) & 0xffffu;
  int32_t a = BYS_SIN_Q[i], b = BYS_SIN_Q[i + 1];
  int32_t v = a + (((b - a) * (int32_t)fr) >> 16);
  return (ph & 0x80000000u) ? -v : v;
}

static int tx_setup(bys_tx_t *tx, const bys_tx_cfg_t *cfg) {
  bys_tx_cfg_t d;
  if (!cfg) { bys_tx_cfg_default(&d); cfg = &d; }
  uint32_t rate = cfg->sample_rate;
  if (rate < 8000 || rate > 48000) return BYS_E_RATE;
  bys_band_info_t bi;
  bys_band_info((bys_band_t)tx->frame.band, (bys_profile_t)tx->frame.profile, &bi);
  /* The highest tone of the band must stay below Nyquist. */
  uint64_t mmax = (uint64_t)bi.bin + (uint64_t)bi.groups * BYS_TONES - 1;
  if (mmax * 93750u >= (uint64_t)rate * 1000u) return BYS_E_RATE;

  uint64_t den = 1000ull * rate;
  tx->den = (uint32_t)den;
  tx->ramp = (3u * rate + 500u) / 1000u;
  tx->e_step = (uint32_t)(((1ull << 32) + 2ull * tx->ramp) / (4ull * tx->ramp));
  tx->gap_n = ((uint32_t)cfg->gap_ms * rate + 500u) / 1000u;
  tx->sym_a = bi.symbol_us * rate;
  tx->acc = 500000u;
  tx->bin = bi.bin;
  tx->total = (uint32_t)(((uint64_t)tx->frame.sym_total * tx->sym_a + 500000u) / 1000000u) + tx->gap_n;

  float level = cfg->level;
  if (!(level > 0.0f)) level = 0.0f;
  if (level > 1.0f) level = 1.0f;
  float amp = (float)bi.amp_permille / 1000.0f;
  float peak = amp * (float)bi.groups; /* possible peak if all tones coincide (only wide exceeds 1) */
  if (peak > 1.0f) amp /= peak;        /* the real peak cannot be measured while streaming: scale by the bound */
  tx->amp = (uint16_t)(amp * level * 32768.0f + 0.5f);
  tx->state = 0;
  tx->n = tx->len = 0;
  return BYS_OK;
}

#define WRAP(call)                                  \
  do {                                              \
    if (!tx) return BYS_E_ARG;                      \
    memset(tx, 0, sizeof *tx);                      \
    int r_ = (call);                                \
    if (r_ < 0) return r_;                          \
    return tx_setup(tx, cfg);                       \
  } while (0)

int bys_tx_init_payload(bys_tx_t *tx, const bys_tx_cfg_t *cfg, bys_band_t band, bys_profile_t profile, const char *utf8, size_t n) {
  WRAP(bys_frame_init_payload(&tx->frame, band, profile, utf8, n));
}
int bys_tx_init_reference(bys_tx_t *tx, const bys_tx_cfg_t *cfg, bys_band_t band, bys_profile_t profile, unsigned set, const uint8_t fp[BYS_REF_BYTES]) {
  WRAP(bys_frame_init_reference(&tx->frame, band, profile, set, fp));
}
int bys_tx_init_ack(bys_tx_t *tx, const bys_tx_cfg_t *cfg, bys_band_t band, int receipt, const uint8_t hash4[4]) {
  WRAP(bys_frame_init_ack(&tx->frame, band, receipt, hash4));
}
int bys_tx_init_ack_heard(bys_tx_t *tx, const bys_tx_cfg_t *cfg, bys_band_t band, int receipt, unsigned heard, const uint8_t hash4[4]) {
  WRAP(bys_frame_init_ack_heard(&tx->frame, band, receipt, heard, hash4));
}
int bys_tx_init_risk(bys_tx_t *tx, const bys_tx_cfg_t *cfg, bys_band_t band, unsigned reason, unsigned source, unsigned hops, const uint8_t own[3], const uint8_t intruder[3]) {
  WRAP(bys_frame_init_risk(&tx->frame, band, reason, source, hops, own, intruder));
}

/* Moves on to the next symbol (or the gap, or the end). */
static void advance(bys_tx_t *tx) {
  if (tx->state == 0) {
    if (bys_frame_next(&tx->frame, tx->nib)) {
      for (unsigned g = 0; g < tx->frame.groups; g++) {
        /* exact step = round(m * 46.875 Hz / rate * 2^32): one rounding per tone and symbol, not per sample */
        uint64_t m = (uint64_t)(tx->bin + g * BYS_TONES + tx->nib[g]) * BYS_SPACING_MHZ;
        tx->step[g] = (uint32_t)(((m << 32) + tx->den / 2) / tx->den);
        tx->phase[g] = 0;
      }
      tx->acc += tx->sym_a;
      tx->len = tx->acc / 1000000u;
      tx->acc %= 1000000u;
      tx->n = 0;
      return;
    }
    tx->state = 1;
    tx->len = tx->gap_n;
    tx->n = 0;
    if (tx->len) return;
  }
  tx->state = 2;
}

static void synth(bys_tx_t *tx, int16_t *out, uint32_t cnt) {
  const unsigned groups = tx->frame.groups;
  const uint32_t ramp = tx->ramp, len = tx->len;
  for (uint32_t i = 0; i < cnt; i++) {
    uint32_t pos = tx->n + i;
    int32_t s = 0;
    for (unsigned g = 0; g < groups; g++) {
      s += sinq(tx->phase[g]);
      tx->phase[g] += tx->step[g];
    }
    int32_t y = (int32_t)((s * (int32_t)tx->amp + 16384) >> 15);
    uint32_t m = pos < ramp ? pos : (pos + ramp >= len ? len - 1 - pos : ramp);
    if (m < ramp) {
      int32_t e = sinq(m * tx->e_step);
      y = (y * ((e * e + 16384) >> 15) + 16384) >> 15;
    }
    out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
  }
}

size_t bys_tx_fill(bys_tx_t *tx, int16_t *out, size_t n) {
  size_t w = 0;
  while (w < n && tx->state != 2) {
    if (tx->n >= tx->len) {
      if (tx->state == 1) { tx->state = 2; break; }
      advance(tx);
      continue;
    }
    size_t c = n - w;
    if (c > tx->len - tx->n) c = tx->len - tx->n;
    if (tx->state == 1) memset(out + w, 0, c * sizeof *out);
    else synth(tx, out + w, (uint32_t)c);
    tx->n += (uint32_t)c;
    w += c;
  }
  return w;
}

int bys_tx_done(const bys_tx_t *tx) { return tx->state == 2; }
uint32_t bys_tx_total_samples(const bys_tx_t *tx) { return tx->total; }
