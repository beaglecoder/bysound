/*
 * bysound v4 streaming demodulator: a C99 port of web/src/modem/receiver.ts (+ fft.ts, symbolWindow.ts, protocol.ts) with bounded
 * memory and no malloc. Same decisions as the TS: 1/46.875 s window with a smoothed edge (Tukey 0.1, the one in symbolWindow.ts),
 * 4 ms search step, per-tone energy |X(k)|^2, per-tone noise floor, preamble score (using each band's and profile's pattern),
 * candidate plateau, header chosen among the possible ones, echo, simultaneous bands and a two-sender warning.
 *
 * Audio that is kept (and why):
 *  - ring of BYS_DEMOD_RING = 4 windows (4096 samples at 48 kHz, 85 ms): once the preamble is found, symbols are read one at a time
 *    as soon as their window closes (delay < 1 step), so only one window plus the plateau-detection delay is needed
 *    (about 10 ms). The TS keeps 12 s so it can re-read the whole frame and combine passes.
 *  - history of BYS_DEMOD_HIST = 80 steps of the energies of the 96 bins of the high bank (bfloat16): the preamble score
 *    looks at 8 symbols 8 steps apart (63 steps end to end) and the plateau adds a few more steps.
 * Differences from the TS (all due to bounded memory): passes are not combined (no soft reads, no confidence-based erasures,
 * no "unseen reads"): each frame is decided with a single pass of hard decisions plus RS correction; the preamble echo is
 * measured from the history energies (step closest to each symbol) rather than from exact windows.
 */
#include "bysound.h"
#include <math.h>
#include <string.h>

#define PI_D 3.14159265358979323846
enum { S_SEARCH = 0, S_HEADER = 1, S_BODY = 2 };
#define MIN_ENERGY 1e-11f /* the TS's 1e-20 (+-1 scale) brought to the int16 scale */
#define POOL_HALF 400u
#define HBINS 320u
#define LBINS 40u
#define OVERLAP_MIN_HOPS 2
#define OVERLAP_MIN_SCORE 0.45f
#define REPEAT_SCORE_SHARE 0.7f /* a preamble past the frame's minimum length is the next repetition only with this share of its score */

/* ---------- utilities ---------- */
static int64_t fdiv(int64_t a, int64_t b) { int64_t q = a / b; if (a % b < 0) q--; return q; }
static int64_t rdiv(int64_t a, int64_t b) { return fdiv(2 * a + b, 2 * b); } /* Math.round(a / b), b > 0 */

static uint16_t f2b(float f) { uint32_t u; memcpy(&u, &f, 4); u += 0x7fffu + ((u >> 16) & 1u); return (uint16_t)(u >> 16); }
static float b2f(uint16_t b) { uint32_t u = (uint32_t)b << 16; float f; memcpy(&f, &u, 4); return f; }

static int64_t sym_start(const bys_demod_t *d, const bys_demod_reader_t *r, uint32_t k) {
  return (int64_t)(((uint64_t)k * r->sym_us * d->rate + 500000u) / 1000000u);
}
static int64_t hop_end(const bys_demod_t *d, int64_t i) { return (int64_t)d->win + i * (int64_t)d->hop; }
static unsigned symbols_for(const bys_demod_reader_t *r, unsigned bytes) { return (2u * bytes + r->groups - 1u) / r->groups; }
static float *floor_of(bys_demod_t *d, const bys_demod_reader_t *r) { return d->pool + r->pool_off; }
static float *prev_of(bys_demod_t *d, const bys_demod_reader_t *r) { return d->pool + POOL_HALF + r->pool_off; }
static float energy_at(const bys_demod_t *d, const bys_demod_reader_t *r, int64_t hop, unsigned t) {
  return b2f(d->hist[(uint32_t)(hop % BYS_DEMOD_HIST)][r->bank_off + r->offset + t]);
}
static unsigned body_bytes(const bys_demod_reader_t *r) {
  switch (r->magic) {
    case BYS_MAGIC_PAYLOAD: return (unsigned)(r->length - 1) * BYS_BLOCK_BYTES + BYS_BLOCK_HEADER + 1 + BYS_BLOCK_PARITY;
    case BYS_MAGIC_ACK: return BYS_ACK_BODY;
    case BYS_MAGIC_RISK: return BYS_RISK_BODY;
    default: return BYS_REF_DATA + BYS_REF_PARITY;
  }
}
static unsigned body_bytes_max(const bys_demod_reader_t *r) { return r->magic == BYS_MAGIC_PAYLOAD ? (unsigned)r->length * BYS_BLOCK_BYTES : body_bytes(r); }
static uint32_t first_body(const bys_demod_reader_t *r) { return BYS_PREAMBLE_SYMBOLS + r->hdr_syms; }
static int64_t symbol_end(const bys_demod_t *d, const bys_demod_reader_t *r, uint32_t k) { return r->frame_start + sym_start(d, r, k) + r->end_off; }

/* ---------- per-tone energy engine ---------- */
#if defined(BYS_DEMOD_FFT)
/* Copies the window [start, start + win) from the ring with the smoothed edge. 0 if it is no longer in the ring. */
static int win_load(bys_demod_t *d, int64_t start) {
  if (start < 0 || start + (int64_t)d->win > d->total || d->total - start > (int64_t)BYS_DEMOD_RING) return 0;
  uint32_t p = (uint32_t)(start % BYS_DEMOD_RING), n = d->win, e = d->taper_n;
  for (uint32_t i = 0; i < n; i++) {
    float x = (float)d->ring[p];
    if (++p == BYS_DEMOD_RING) p = 0;
    uint32_t dist = i < n - 1 - i ? i : n - 1 - i;
    d->re[i] = dist < e ? x * d->taper[dist] : x;
  }
  return 1;
}

static void fft_run(bys_demod_t *d) {
  const uint32_t n = d->win;
  float *re = d->re, *im = d->im;
  memset(im, 0, n * sizeof(float));
  for (uint32_t i = 1, j = 0; i < n; i++) {
    uint32_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; }
  }
  for (uint32_t len = 2; len <= n; len <<= 1) {
    const uint32_t half = len >> 1, step = n / len;
    for (uint32_t i = 0; i < n; i += len) {
      for (uint32_t k = 0; k < half; k++) {
        const float wr = d->twc[k * step], wi = d->tws[k * step];
        const uint32_t x = i + k, y = x + half;
        const float tr = re[y] * wr - im[y] * wi, ti = re[y] * wi + im[y] * wr;
        re[y] = re[x] - tr; im[y] = im[x] - ti;
        re[x] += tr; im[x] += ti;
      }
    }
  }
}
static void win_analyze(bys_demod_t *d) { if (d->fft_ok) fft_run(d); }

static void tone_energy(const bys_demod_t *d, uint32_t first_bin, uint32_t count, float *out) {
  if (d->fft_ok) {
    for (uint32_t t = 0; t < count; t++) {
      const uint32_t k = (first_bin + t) * d->stride;
      out[t] = d->re[k] * d->re[k] + d->im[k] * d->im[k];
    }
    return;
  }
  /* the grid does not fall on the bins: float32 Goertzel, like the TS */
  const uint32_t g0 = first_bin >= HBINS ? first_bin - HBINS : BYS_DEMOD_HIGH_TONES + (first_bin - LBINS);
  for (uint32_t t = 0; t < count; t++) {
    const float c = d->gz[g0 + t];
    float s1 = 0, s2 = 0;
    for (uint32_t i = 0; i < d->win; i++) { const float s0 = d->re[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    out[t] = s1 * s1 + s2 * s2 - c * s1 * s2;
  }
}
#else /* BYS_DEMOD_GOERTZEL: integers */
static int win_load(bys_demod_t *d, int64_t start) {
  if (start < 0 || start + (int64_t)d->win > d->total || d->total - start > (int64_t)BYS_DEMOD_RING) return 0;
  uint32_t p = (uint32_t)(start % BYS_DEMOD_RING), n = d->win, e = d->taper_n;
  for (uint32_t i = 0; i < n; i++) {
    int32_t x = d->ring[p];
    if (++p == BYS_DEMOD_RING) p = 0;
    uint32_t dist = i < n - 1 - i ? i : n - 1 - i;
    if (dist < e) x = (x * d->taper[dist] + 16384) >> 15;
    d->win_buf[i] = (int16_t)x;
  }
  return 1;
}
static void win_analyze(bys_demod_t *d) { (void)d; }

/* Goertzel: s = x + c*s1 - s2 with c = 2 cos(w) in Q28 and 32-bit states (64-bit products). Energy = s1^2 + s2^2 - c s1 s2. */
static uint64_t goertzel(const int16_t *x, uint32_t n, int32_t c) {
  const int64_t half = (int64_t)1 << 27;
  int32_t s1 = 0, s2 = 0;
  for (uint32_t i = 0; i < n; i++) {
    const int32_t s0 = x[i] + (int32_t)((((int64_t)c * s1) + half) >> 28) - s2;
    s2 = s1; s1 = s0;
  }
  const int64_t e = (int64_t)s1 * s1 + (int64_t)s2 * s2 - ((((int64_t)c * s1) + half) >> 28) * s2;
  return e < 0 ? 0u : (uint64_t)e;
}
static void tone_energy(const bys_demod_t *d, uint32_t first_bin, uint32_t count, float *out) {
  const uint32_t g0 = first_bin >= HBINS ? first_bin - HBINS : BYS_DEMOD_HIGH_TONES + (first_bin - LBINS);
  for (uint32_t t = 0; t < count; t++) out[t] = (float)goertzel(d->win_buf, d->win, d->gz[g0 + t]);
}
#endif

/* ---------- events and shared pool ---------- */
static void emit(bys_demod_t *d, const bys_demod_event_t *ev) { if (d->cb) d->cb(d->user, ev); }
static void clash(bys_demod_t *d) {
  d->stats.clashes++;
  bys_demod_event_t ev; memset(&ev, 0, sizeof ev);
  ev.type = BYS_EV_CLASH; ev.profile = d->profile;
  emit(d, &ev);
}
static void note_start(bys_demod_t *d, unsigned band, int64_t at, uint32_t span) {
  int overlapped = 0;
  for (unsigned i = 0; i < d->nstarts; i++) {
    const int64_t apart = at > d->starts[i].at ? at - d->starts[i].at : d->starts[i].at - at;
    const int64_t lim = (d->starts[i].at < at ? (int64_t)d->starts[i].span : (int64_t)span) - (int64_t)d->margin;
    if ((apart >= (int64_t)d->self_echo || d->starts[i].band != band) && apart < lim) overlapped = 1;
  }
  if (d->nstarts == 8) { memmove(d->starts, d->starts + 1, 7 * sizeof d->starts[0]); d->nstarts = 7; }
  d->starts[d->nstarts].at = at; d->starts[d->nstarts].span = span; d->starts[d->nstarts].band = (uint8_t)band; d->nstarts++;
  if (overlapped) clash(d);
}
/* Waits a little: the real band may read its header in the same step or right after. */
static void flush_header_fails(bys_demod_t *d) {
  const int64_t due = d->total - (int64_t)d->hdr_wait;
  unsigned kept = 0;
  for (unsigned i = 0; i < d->nhfails; i++) {
    if (d->hfails[i].noted > due) { d->hfails[kept++] = d->hfails[i]; continue; }
    int covered = 0;
    for (unsigned s = 0; s < d->nstarts; s++)
      if (d->hfails[i].at >= d->starts[s].at - (int64_t)d->hdr_near && d->hfails[i].at <= d->starts[s].at + (int64_t)d->starts[s].span) covered = 1;
    if (!covered) d->stats.failures++;
  }
  d->nhfails = (uint8_t)kept;
}

/* ---------- band reader ---------- */
static float whiten_min(bys_demod_t *d, const bys_demod_reader_t *r) {
  const float *f = floor_of(d, r);
  float s = 0;
  for (unsigned t = 0; t < r->tones; t++) s += f[t];
  const float m = s / (float)r->tones * 0.05f;
  return m > MIN_ENERGY ? m : MIN_ENERGY;
}

static void release_out(bys_demod_t *d, const bys_demod_reader_t *r) { if (d->out_owner == r->band) d->out_owner = 0xff; }

static void reset_search(bys_demod_t *d, bys_demod_reader_t *r) {
  r->state = S_SEARCH;
  r->search_from = r->frame_start + sym_start(d, r, 1);
  r->ncand = 0;
  release_out(d, r);
}
static void fail_frame(bys_demod_t *d, bys_demod_reader_t *r, int body_consumed) {
  d->stats.failures++;
  reset_search(d, r);
  if (body_consumed) r->search_from = r->frame_start + sym_start(d, r, first_body(r) + r->body_read);
}

static void update_floor(bys_demod_t *d, bys_demod_reader_t *r, int64_t index) {
  float *f = floor_of(d, r);
  if (index == 0) {
    float *tmp = d->scratch;
    for (unsigned t = 0; t < r->tones; t++) {
      const float x = energy_at(d, r, index, t);
      unsigned j = t;
      while (j > 0 && tmp[j - 1] > x) { tmp[j] = tmp[j - 1]; j--; }
      tmp[j] = x;
    }
    for (unsigned t = 0; t < r->tones; t++) f[t] = tmp[r->tones >> 1];
    return;
  }
  const int warm = (float)(index + 1) * d->hop_s < 0.3f;
  const float a = d->hop_s / (warm ? 0.08f : 0.5f);
  for (unsigned t = 0; t < r->tones; t++) {
    const float x = energy_at(d, r, index, t), v = f[t];
    const float tgt = v > 0 ? (x < 4.0f * v ? x : 4.0f * v) : x;
    f[t] = v + a * (tgt - v);
  }
}

/* Score (0..1) of the preamble ending at step `index`. 0 if there is not enough history. */
static int preamble_score(bys_demod_t *d, bys_demod_reader_t *r, int64_t index, int64_t *start_out, float *score_out) {
  const int64_t start = hop_end(d, index) - (int64_t)r->span;
  if (start < 0) return 0;
  const float minimum = whiten_min(d, r);
  const float *f = floor_of(d, r);
  float score = 0;
  for (unsigned k = 0; k < BYS_PREAMBLE_SYMBOLS; k++) {
    const int64_t end = start + sym_start(d, r, k) + (int64_t)r->end_off;
    const int64_t hop = rdiv(end - (int64_t)d->win, d->hop);
    if (hop < 0 || hop > index || index - hop >= (int64_t)BYS_DEMOD_HIST) return 0;
    const uint16_t *e = d->hist[(uint32_t)(hop % BYS_DEMOD_HIST)] + r->bank_off + r->offset;
    for (unsigned g = 0; g < r->groups; g++) {
      float sum = 0, mine = 0;
      for (unsigned v = 0; v < 16; v++) {
        const unsigned t = g * 16 + v;
        const float w = b2f(e[t]) / (f[t] > minimum ? f[t] : minimum);
        sum += w;
        if (v == r->pre[k][g]) mine = w;
      }
      if (sum > 1e-12f) score += mine / sum;
    }
  }
  *start_out = start;
  *score_out = score / (float)(BYS_PREAMBLE_SYMBOLS * r->groups);
  return 1;
}

/* Center of the candidate plateau. */
static int64_t plateau(bys_demod_t *d, bys_demod_reader_t *r) {
  float best = 0;
  for (unsigned i = 0; i < r->ncand; i++) if (r->cand_score[i] > best) best = r->cand_score[i];
  r->frame_score = best;
  int64_t sum = 0;
  unsigned n = 0;
  for (unsigned i = 0; i < r->ncand; i++)
    if (r->cand_score[i] >= best * 0.97f) { sum += hop_end(d, r->cand_idx[i]) - (int64_t)r->span; n++; }
  r->ncand = 0;
  return rdiv(sum, (int64_t)n);
}
static void push_cand(bys_demod_reader_t *r, int64_t index, float score) {
  r->cand_idx[r->ncand] = (int32_t)index;
  r->cand_score[r->ncand] = score;
  r->ncand++;
}

/* Floor-normalized energies of symbol k of the current frame, taken from the closest history step. */
static void hist_symbol(bys_demod_t *d, bys_demod_reader_t *r, uint32_t k, int64_t index, float *out) {
  const float minimum = whiten_min(d, r);
  const float *f = floor_of(d, r);
  int64_t hop = rdiv(symbol_end(d, r, k) - (int64_t)d->win, d->hop);
  if (hop > index) hop = index;
  if (hop < index - (int64_t)BYS_DEMOD_HIST + 1) hop = index - (int64_t)BYS_DEMOD_HIST + 1;
  for (unsigned t = 0; t < r->tones; t++) out[t] = energy_at(d, r, hop, t) / (f[t] > minimum ? f[t] : minimum);
}

static void measure_echo(bys_demod_t *d, bys_demod_reader_t *r, int64_t index) {
  float *last = d->scratch + 2 * BYS_DEMOD_HIGH_TONES, *cur = d->scratch + 3 * BYS_DEMOD_HIGH_TONES;
  float ratios[BYS_PREAMBLE_SYMBOLS * BYS_MAX_GROUPS];
  unsigned nr = 0;
  hist_symbol(d, r, 0, index, last);
  for (unsigned k = 1; k < BYS_PREAMBLE_SYMBOLS; k++) {
    hist_symbol(d, r, k, index, cur);
    for (unsigned g = 0; g < r->groups; g++) {
      const unsigned t = g * 16 + r->pre[k - 1][g];
      if (last[t] > 1e-12f) ratios[nr++] = cur[t] / last[t];
    }
    float *tmp = last; last = cur; cur = tmp;
  }
  for (unsigned i = 1; i < nr; i++) { const float x = ratios[i]; unsigned j = i; while (j > 0 && ratios[j - 1] > x) { ratios[j] = ratios[j - 1]; j--; } ratios[j] = x; }
  float echo = nr ? ratios[nr >> 1] : 0.0f;
  r->echo = echo < 0 ? 0 : echo > 0.8f ? 0.8f : echo;
  memcpy(prev_of(d, r), last, r->tones * sizeof(float));
}

/*
 * Reads symbol k of the current frame: hard nibbles in `nib` and, if `hdr` != NULL, the probabilities (0..255) of group g
 * in hdr[g]. The echo is subtracted as in the TS's softSymbol. 0 if the audio is no longer available.
 */
static int read_symbol(bys_demod_t *d, bys_demod_reader_t *r, uint32_t k, uint8_t *nib, uint8_t (*hdr)[16]) {
  if (!win_load(d, r->frame_start + sym_start(d, r, k) + (int64_t)r->win_start)) return 0;
  win_analyze(d);
  float *w = d->scratch + BYS_DEMOD_HIGH_TONES;
  tone_energy(d, (r->band == BYS_AUDIBLE ? LBINS : HBINS) + r->offset, r->tones, w);
  const float minimum = whiten_min(d, r), echo = r->echo;
  const float *f = floor_of(d, r);
  float *prev = prev_of(d, r);
  for (unsigned t = 0; t < r->tones; t++) w[t] /= (f[t] > minimum ? f[t] : minimum);
  for (unsigned g = 0; g < r->groups; g++) {
    float dd[16], sum = 0;
    const unsigned base = g * 16;
    for (unsigned v = 0; v < 16; v++) {
      const float x = w[base + v] - echo * prev[base + v];
      dd[v] = x > 0 ? x : 0;
      sum += dd[v];
    }
    for (unsigned v = 0; v < 16; v++) prev[base + v] = w[base + v] + echo * prev[base + v];
    unsigned best = 0;
    if (sum <= 1e-12f) {
      if (hdr) for (unsigned v = 0; v < 16; v++) hdr[g][v] = 16;
    } else {
      for (unsigned v = 1; v < 16; v++) if (dd[v] > dd[best]) best = v;
      if (hdr) for (unsigned v = 0; v < 16; v++) hdr[g][v] = (uint8_t)(dd[v] / sum * 255.0f + 0.5f);
    }
    nib[g] = (uint8_t)best;
  }
  return 1;
}

static void deliver(bys_demod_t *d, bys_demod_reader_t *r, int64_t end) {
  const int64_t pre = sym_start(d, r, BYS_PREAMBLE_SYMBOLS);
  for (unsigned i = 0; i < d->nstarts; i++)
    if (d->starts[i].band != r->band && d->starts[i].at > r->frame_start && d->starts[i].at + pre < end) { clash(d); return; }
  const bys_result_t *res = bys_rx_result(&r->rx);
  bys_demod_event_t ev; memset(&ev, 0, sizeof ev);
  ev.type = BYS_EV_FRAME; ev.band = r->band; ev.profile = d->profile; ev.result = res;
  if (res->kind == BYS_KIND_PAYLOAD) { ev.payload = d->out; ev.payload_len = res->payload_len; }
  ev.score_permille = (uint16_t)(r->frame_score * 1000.0f);
  d->stats.frames++;
  emit(d, &ev);
}

static void finish_ok(bys_demod_t *d, bys_demod_reader_t *r) {
  const int64_t end = r->frame_start + sym_start(d, r, first_body(r) + r->body_read);
  deliver(d, r, end);
  reset_search(d, r);
  r->search_from = end;
}

static void header_nibbles(unsigned band, unsigned profile, unsigned magic, unsigned length, unsigned set, uint8_t nib[8]) {
  const uint8_t version = (uint8_t)((BYS_VERSION << 4) | (band << 1) | (set & 1));
  const uint8_t crc_in[4] = { (uint8_t)magic, (uint8_t)length, version, (uint8_t)(profile == BYS_ROBUST) };
  const uint8_t h[4] = { (uint8_t)magic, (uint8_t)length, bys_crc8(crc_in, 4), version };
  for (unsigned i = 0; i < 4; i++) { nib[2 * i] = h[i] >> 4; nib[2 * i + 1] = h[i] & 15; }
}

/* Header read: picks the closest match among the possible ones (reference A and B, risk, ack, and payload of 1 to 255 blocks). */
static void decide_header(bys_demod_t *d, bys_demod_reader_t *r) {
  static const uint8_t magics[4] = { BYS_MAGIC_REFERENCE, BYS_MAGIC_REFERENCE, BYS_MAGIC_RISK, BYS_MAGIC_ACK };
  static const uint8_t lens[4] = { BYS_REF_DATA, BYS_REF_DATA, BYS_RISK_LENGTH, BYS_ACK_LENGTH };
  int best = -1;
  uint8_t choice[8] = { 0 }, magic = 0, length = 0, set = 0;
  for (unsigned c = 0; c < 4u + BYS_MAX_BLOCKS; c++) {
    unsigned m, l, s = 0;
    if (c < 4) { m = magics[c]; l = lens[c]; s = c == 1; } else { m = BYS_MAGIC_PAYLOAD; l = c - 3; }
    uint8_t nib[8];
    header_nibbles(r->band, d->profile, m, l, s, nib);
    int sc = 0;
    for (unsigned i = 0; i < 8; i++) sc += r->hdr_prob[i][nib[i]];
    if (sc > best) { best = sc; memcpy(choice, nib, 8); magic = (uint8_t)m; length = (uint8_t)l; set = (uint8_t)s; }
  }
  unsigned agree = 0;
  for (unsigned i = 0; i < 8; i++) agree += r->hdr_hard[i] == choice[i];
  /* The header's CRC-8 checks out with another band: it is another sender, not read as our own. */
  {
    const uint8_t h0 = (uint8_t)((r->hdr_hard[0] << 4) | r->hdr_hard[1]), h1 = (uint8_t)((r->hdr_hard[2] << 4) | r->hdr_hard[3]);
    const uint8_t h2 = (uint8_t)((r->hdr_hard[4] << 4) | r->hdr_hard[5]), h3 = (uint8_t)((r->hdr_hard[6] << 4) | r->hdr_hard[7]);
    const uint8_t crc_in[4] = { h0, h1, h3, (uint8_t)(d->profile == BYS_ROBUST) };
    if ((h3 >> 4) == BYS_VERSION && ((h3 >> 1) & 7) != r->band && h2 == bys_crc8(crc_in, 4)) { reset_search(d, r); return; }
  }
  if (agree < 5) {
    if (d->nhfails < 4) { d->hfails[d->nhfails].at = r->frame_start; d->hfails[d->nhfails].noted = d->total; d->nhfails++; }
    reset_search(d, r);
    return;
  }
  r->magic = magic; r->length = length; r->set = set;
  if (agree >= 7) {
    d->stats.headers++;
    bys_demod_event_t ev; memset(&ev, 0, sizeof ev);
    ev.type = BYS_EV_HEADER; ev.band = r->band; ev.profile = d->profile;
    emit(d, &ev);
    note_start(d, r->band, r->frame_start, (uint32_t)sym_start(d, r, first_body(r) + symbols_for(r, body_bytes(r))));
  }
  int ok = 1;
  uint8_t *out = NULL;
  if (magic == BYS_MAGIC_PAYLOAD) {
    /* a single output buffer: if another band is using it (two senders at once) this frame is not read; the clash was already reported */
    if (d->out && d->out_owner != 0xff) { reset_search(d, r); return; }
    if (d->out && d->out_cap >= (size_t)length * BYS_BLOCK_DATA + 4) { out = d->out; d->out_owner = r->band; } else ok = 0;
  }
  if (!ok) { fail_frame(d, r, 0); return; }
  bys_rx_init(&r->rx, (bys_band_t)r->band, (bys_profile_t)d->profile, out, out ? d->out_cap : 0);
  uint8_t pad[BYS_MAX_GROUPS * 4];
  memset(pad, 0, sizeof pad);
  memcpy(pad, choice, 8);
  for (unsigned s2 = 0; s2 < r->hdr_syms; s2++) bys_rx_push(&r->rx, pad + s2 * r->groups);
  r->state = S_BODY;
  r->body_read = 0;
  r->probe_tried = BYS_BLOCK_HEADER + BYS_BLOCK_PARITY;
  r->probe_blk = 0xff;
}

/* The last block of a payload does not say how long it is: each new length is tried as soon as its bytes arrive. 1 if the frame closed. */
static int probe_last_block(bys_demod_reader_t *r) {
  bys_rx_t *rx = &r->rx;
  if (rx->kind != BYS_KIND_PAYLOAD || rx->blk + 1 != rx->total || rx->buf_n <= BYS_BLOCK_HEADER + BYS_BLOCK_PARITY) return 0;
  if (r->probe_blk != rx->blk) { r->probe_blk = rx->blk; r->probe_tried = BYS_BLOCK_HEADER + BYS_BLOCK_PARITY; }
  for (unsigned len = r->probe_tried + 1u; len <= rx->buf_n; len++) {
    r->probe_tried = (uint16_t)len;
    /* RS alone may accept a length one byte short (last parity byte 0x00): the CRC-32 of the whole payload decides. */
    if (bys_rx_last_fits(rx, len)) return 1;
  }
  return 0;
}

/* Reads the symbols whose window has closed: header first, then the body (which goes to bys_rx). */
static void read_available(bys_demod_t *d, bys_demod_reader_t *r) {
  for (;;) {
    const uint32_t k = r->sym_next;
    if (r->state == S_SEARCH || d->total < symbol_end(d, r, k)) return;
    uint8_t nib[BYS_MAX_GROUPS];
    if (r->state == S_HEADER) {
      uint8_t prob[BYS_MAX_GROUPS][16];
      if (!read_symbol(d, r, k, nib, prob)) { fail_frame(d, r, 0); return; }
      const unsigned i0 = (k - BYS_PREAMBLE_SYMBOLS) * r->groups;
      for (unsigned g = 0; g < r->groups && i0 + g < 8; g++) { memcpy(r->hdr_prob[i0 + g], prob[g], 16); r->hdr_hard[i0 + g] = nib[g]; }
      r->sym_next++;
      if (r->sym_next == first_body(r)) decide_header(d, r);
      continue;
    }
    const unsigned count = symbols_for(r, body_bytes_max(r));
    if (r->body_read >= count) return;
    if (!read_symbol(d, r, k, nib, NULL)) { fail_frame(d, r, 0); return; }
    r->sym_next++;
    r->body_read++;
    int rc = bys_rx_push(&r->rx, nib);
    if (rc == BYS_RX_MORE && probe_last_block(r)) rc = bys_rx_finish(&r->rx);
    if (rc == BYS_RX_DONE) { finish_ok(d, r); return; }
    if (rc < 0) { fail_frame(d, r, 0); return; }
    if (r->body_read == count) {
      rc = r->magic == BYS_MAGIC_PAYLOAD ? bys_rx_finish(&r->rx) : rc;
      if (rc == BYS_RX_DONE) finish_ok(d, r); else fail_frame(d, r, 1);
      return;
    }
  }
}

static void enter_frame(bys_demod_t *d, bys_demod_reader_t *r, int64_t at, int64_t index) {
  r->frame_start = at;
  r->state = S_HEADER;
  r->sym_next = BYS_PREAMBLE_SYMBOLS;
  r->body_read = 0;
  memset(r->hdr_hard, 0, sizeof r->hdr_hard);
  memset(r->hdr_prob, 16, sizeof r->hdr_prob);
  measure_echo(d, r, index);
  read_available(d, r);
}

static void search(bys_demod_t *d, bys_demod_reader_t *r, int64_t index) {
  int64_t start;
  float score;
  if (!preamble_score(d, r, index, &start, &score) || start < r->search_from) return;
  if (score >= (float)BYS_PREAMBLE_THRESHOLD_PERMILLE / 1000.0f && r->ncand < BYS_DEMOD_CAND) { push_cand(r, index, score); return; }
  if (r->ncand) enter_frame(d, r, plateau(d, r), index);
}

/* The preamble is also searched for inside a body: on top of it is another sender, and past the minimum it is the next repetition. */
static int search_in_body(bys_demod_t *d, bys_demod_reader_t *r, int64_t index) {
  int64_t start;
  float score;
  if (!preamble_score(d, r, index, &start, &score)) return 0;
  const int64_t min_end = r->frame_start + sym_start(d, r, first_body(r) + symbols_for(r, body_bytes(r)));
  if (score >= (float)BYS_PREAMBLE_THRESHOLD_PERMILLE / 1000.0f && start >= r->frame_start + (int64_t)d->overlap_from && r->ncand < BYS_DEMOD_CAND) { push_cand(r, index, score); return 0; }
  if (!r->ncand) return 0;
  const unsigned hops = r->ncand;
  float best = 0;
  for (unsigned i = 0; i < r->ncand; i++) if (r->cand_score[i] > best) best = r->cand_score[i];
  const float own = r->frame_score;
  const int64_t at = plateau(d, r);
  r->frame_score = own; /* plateau() stores the candidate's score; the frame being read keeps its own */
  if (at + sym_start(d, r, BYS_PREAMBLE_SYMBOLS) < min_end) {
    /* Data that resembles the preamble gives an isolated peak: two senders give a high plateau. */
    if (hops >= OVERLAP_MIN_HOPS && best >= OVERLAP_MIN_SCORE) clash(d);
    return 0;
  }
  if (r->magic != BYS_MAGIC_PAYLOAD) return 0;
  /* The next repetition comes from the same sender, so its preamble scores about as well as this frame's. Body data that happens to
   * match the preamble in a few nibbles scores only that share of it (6 of 16 nibbles: 0.375 in a clean channel, over the threshold)
   * and must not cut off the read: the last block would be lost. */
  if (best < REPEAT_SCORE_SHARE * own) return 0;
  /* The next repetition arrived and the last block did not close: try once more and move on to the new frame. */
  read_available(d, r);
  if (r->state == S_BODY) fail_frame(d, r, 1);
  enter_frame(d, r, at, index);
  return 1;
}

static void reader_on_hop(bys_demod_t *d, bys_demod_reader_t *r, int64_t index) {
  if (!r->enabled) return;
  if (r->state == S_SEARCH) update_floor(d, r, index);
  {
    const float *f = floor_of(d, r);
    float mean = 0, peak = 0;
    for (unsigned t = 0; t < r->tones; t++) { const float e = energy_at(d, r, index, t); mean += f[t]; if (e > peak) peak = e; }
    mean /= (float)r->tones;
    if (mean > 0 && peak / mean > r->hold) r->hold = peak / mean;
  }
  switch (r->state) {
    case S_SEARCH: search(d, r, index); break;
    case S_HEADER: read_available(d, r); break;
    default:
      if (search_in_body(d, r, index)) break;
      read_available(d, r);
  }
}

/* ---------- API ---------- */
static void on_hop(bys_demod_t *d) {
  const int64_t index = d->hops;
  float *e = d->scratch;
  if (!win_load(d, d->total - (int64_t)d->win)) return;
  win_analyze(d);
  uint16_t *row = d->hist[(uint32_t)(index % BYS_DEMOD_HIST)];
  if (d->bank_used[0]) {
    const unsigned lo = d->bank_lo[0], n = d->bank_hi[0] - lo;
    tone_energy(d, lo, n, e);
    for (unsigned t = 0; t < n; t++) row[lo - HBINS + t] = f2b(e[t]);
  }
#if BYS_DEMOD_LOW_BANK
  if (d->bank_used[1]) {
    const unsigned lo = d->bank_lo[1], n = d->bank_hi[1] - lo;
    tone_energy(d, lo, n, e);
    for (unsigned t = 0; t < n; t++) row[BYS_DEMOD_HIGH_TONES + lo - LBINS + t] = f2b(e[t]);
  }
#endif
  for (unsigned b = 0; b < BYS_BAND_COUNT; b++) reader_on_hop(d, &d->rd[b], index);
  flush_header_fails(d);
}

void bys_demod_cfg_default(bys_demod_cfg_t *cfg) {
  memset(cfg, 0, sizeof *cfg);
  cfg->sample_rate = 48000;
  cfg->profile = BYS_NORMAL;
  cfg->band_mask = (uint8_t)((1u << BYS_TOP) | (1u << BYS_HIGH) | (1u << BYS_UPPER) | (1u << BYS_MID));
  cfg->hop_mult = 1;
}

void bys_demod_reset(bys_demod_t *d) {
  d->total = 0; d->wp = 0; d->hops = 0;
  d->next_hop_end = (int64_t)d->win;
  d->nstarts = 0; d->nhfails = 0; d->out_owner = 0xff;
  memset(d->hist, 0, sizeof d->hist);
  memset(d->pool, 0, sizeof d->pool);
  for (unsigned b = 0; b < BYS_BAND_COUNT; b++) {
    bys_demod_reader_t *r = &d->rd[b];
    r->state = S_SEARCH; r->ncand = 0; r->search_from = 0; r->frame_start = 0; r->echo = 0; r->frame_score = 0; r->hold = 0;
  }
}

int bys_demod_init(bys_demod_t *d, const bys_demod_cfg_t *cfgin) {
  bys_demod_cfg_t cfg;
  if (cfgin) cfg = *cfgin; else bys_demod_cfg_default(&cfg);
  if (!d || cfg.sample_rate < 8000 || cfg.sample_rate > BYS_DEMOD_MAX_RATE || cfg.hop_mult < 1 || cfg.hop_mult > 4 || !cfg.band_mask || (cfg.band_mask & 0x80)) return BYS_E_ARG;
  if (cfg.profile != BYS_NORMAL && cfg.profile != BYS_ROBUST) return BYS_E_ARG;
  if (cfg.profile == BYS_ROBUST && !BYS_DEMOD_ROBUST) return BYS_E_PROFILE;
  if ((cfg.band_mask >> BYS_AUDIBLE & 1) && (!BYS_DEMOD_LOW_BANK || cfg.profile == BYS_ROBUST)) return BYS_E_ARG;
  if (cfg.profile == BYS_ROBUST && (cfg.band_mask >> BYS_AUDIBLE)) return BYS_E_PROFILE;
  const uint64_t rate = cfg.sample_rate, scale = cfg.profile == BYS_ROBUST ? 2 : 1;
  memset(d, 0, sizeof *d);
  d->rate = cfg.sample_rate;
  d->profile = (uint8_t)cfg.profile;
  d->hop_mult = cfg.hop_mult;
  d->win = (uint32_t)((2 * rate * 1000 * scale + 46875) / (2 * 46875));
  d->hop = (uint32_t)((4 * scale * cfg.hop_mult * rate + 500) / 1000);
  d->hop_s = 0.004f * (float)(scale * cfg.hop_mult);
  d->out = cfg.out; d->out_cap = cfg.out_cap; d->cb = cfg.cb; d->user = cfg.user;
  if (d->win > BYS_DEMOD_WIN_MAX || d->win < 32 || d->hop < 1) return BYS_E_RATE;
  d->self_echo = (uint32_t)(rate * 3 / 10); d->margin = (uint32_t)(rate * 15 / 100);
  d->hdr_near = d->margin; d->hdr_wait = d->self_echo; d->overlap_from = d->self_echo;
  d->stride = 0;
  if (((uint64_t)d->win * 46875u) % (rate * 1000u) == 0) d->stride = (uint32_t)((uint64_t)d->win * 46875u / (rate * 1000u));
  d->fft_ok = 0;
#if defined(BYS_DEMOD_FFT)
  d->fft_ok = d->stride >= 1 && (d->win & (d->win - 1)) == 0 && (HBINS + BYS_DEMOD_HIGH_TONES) * d->stride < d->win / 2;
#endif
  /* smoothed edge: the Tukey window of symbolWindow.ts (taper 0.1) */
  {
    const double edge = 0.1 * (double)(d->win - 1) / 2.0;
    uint32_t n = 0;
    while ((double)n < edge && n < BYS_DEMOD_TAPER_MAX) {
      const double v = 0.5 - 0.5 * cos(PI_D * (double)n / edge);
#if defined(BYS_DEMOD_FFT)
      d->taper[n] = (float)v;
#else
      d->taper[n] = (int32_t)(v * 32768.0 + 0.5);
#endif
      n++;
    }
    d->taper_n = n;
  }
#if defined(BYS_DEMOD_FFT)
  if (d->fft_ok)
    for (uint32_t k = 0; k < d->win / 2; k++) { d->twc[k] = (float)cos(2 * PI_D * (double)k / (double)d->win); d->tws[k] = (float)-sin(2 * PI_D * (double)k / (double)d->win); }
#endif
  { /* Goertzel coefficients for the whole bank (the FFT engine uses them only if the grid does not fall on bins) */
    for (uint32_t t = 0; t < BYS_DEMOD_BANK_TONES; t++) {
      const uint32_t bin = t < BYS_DEMOD_HIGH_TONES ? HBINS + t : LBINS + (t - BYS_DEMOD_HIGH_TONES);
      const double c = 2.0 * cos(2.0 * PI_D * ((double)bin * 46.875) / (double)rate);
#if defined(BYS_DEMOD_FFT)
      d->gz[t] = (float)c;
#else
      d->gz[t] = (int32_t)(c * 268435456.0 + (c < 0 ? -0.5 : 0.5));
#endif
    }
  }
  unsigned pool = 0, usable = 0;
  for (unsigned b = 0; b < BYS_BAND_COUNT; b++) {
    bys_demod_reader_t *r = &d->rd[b];
    bys_band_info_t bi;
    r->band = (uint8_t)b;
    r->pool_off = (uint16_t)pool;
    if (bys_band_info((bys_band_t)b, cfg.profile, &bi) != BYS_OK) { r->enabled = 0; continue; }
    r->groups = (uint8_t)bi.groups;
    r->tones = (uint8_t)(bi.groups * 16);
    pool += r->tones;
    const int fits = ((uint64_t)bi.bin + bi.groups * 16u - 1u) * 93750u < rate * 1000u;
    r->enabled = (uint8_t)(((cfg.band_mask >> b) & 1) && fits && (b != BYS_AUDIBLE || BYS_DEMOD_LOW_BANK));
    r->bank_off = b == BYS_AUDIBLE ? BYS_DEMOD_HIGH_TONES : 0;
    r->offset = (uint8_t)(b == BYS_AUDIBLE ? bi.bin - LBINS : bi.bin - HBINS);
    r->sym_us = bi.symbol_us;
    r->win_start = (uint32_t)rdiv((int64_t)((uint64_t)bi.window_start_us * rate), 1000000);
    {
      const uint64_t num = rate * ((uint64_t)bi.window_start_us * 46875u + scale * 1000000000u), den = 46875ull * 1000000ull;
      r->end_off = (uint32_t)((2 * num + den) / (2 * den));
      if (r->end_off < r->win_start + d->win) r->end_off = r->win_start + d->win; /* the whole window must have arrived (at 44.1 kHz that is 941 samples versus 940.8) */
    }
    r->span = (uint32_t)(7 * sym_start(d, r, 1) + r->end_off);
    r->hdr_syms = (uint8_t)((2 * BYS_HEADER_BYTES + r->groups - 1) / r->groups);
    for (unsigned k = 0; k < BYS_PREAMBLE_SYMBOLS; k++) bys_preamble_symbol((bys_band_t)b, cfg.profile, k, r->pre[k]);
    if (r->enabled) {
      const unsigned bk = b == BYS_AUDIBLE, lo = (bk ? LBINS : HBINS) + r->offset, hi = lo + r->tones;
      if (!d->bank_used[bk] || lo < d->bank_lo[bk]) d->bank_lo[bk] = (uint16_t)lo;
      if (!d->bank_used[bk] || hi > d->bank_hi[bk]) d->bank_hi[bk] = (uint16_t)hi;
      usable++;
      d->bank_used[bk] = 1;
    }
  }
  if (!usable) return BYS_E_RATE;
  bys_demod_reset(d);
  return BYS_OK;
}

void bys_demod_push(bys_demod_t *d, const int16_t *pcm, size_t n) {
  while (n) {
    const int64_t room = d->next_hop_end - d->total;
    size_t take = (int64_t)n < room ? n : (size_t)room;
    const size_t n_take = take;
    const int16_t *s = pcm;
    while (take) {
      size_t k = BYS_DEMOD_RING - d->wp;
      if (k > take) k = take;
      memcpy(d->ring + d->wp, s, k * sizeof(int16_t));
      d->wp += (uint32_t)k;
      if (d->wp == BYS_DEMOD_RING) d->wp = 0;
      s += k; take -= k;
    }
    d->total += (int64_t)n_take;
    d->stats.samples += n_take;
    pcm += n_take; n -= n_take;
    if (d->total == d->next_hop_end) {
      on_hop(d);
      d->hops++;
      d->next_hop_end += d->hop;
    }
  }
}

const bys_demod_stats_t *bys_demod_stats(const bys_demod_t *d) { return &d->stats; }

int bys_demod_progress(const bys_demod_t *d) {
  const bys_demod_reader_t *act = NULL;
  for (unsigned b = 0; b < BYS_BAND_COUNT; b++) if (d->rd[b].state == S_BODY) { act = &d->rd[b]; break; }
  for (unsigned b = 0; !act && b < BYS_BAND_COUNT; b++) if (d->rd[b].state == S_HEADER) act = &d->rd[b];
  if (!act) return -1;
  const bys_demod_reader_t *r = act;
  const int64_t header_end = symbol_end(d, r, first_body(r) - 1);
  const double total = (double)d->total;
  double p;
  if (r->state == S_HEADER) p = 120.0 * (total - (double)r->frame_start) / (double)(header_end - r->frame_start);
  else {
    const int64_t end = symbol_end(d, r, first_body(r) + symbols_for(r, body_bytes(r)) - 1);
    p = 120.0 + 880.0 * (total - (double)header_end) / (double)(end - header_end);
  }
  return p < 0 ? 0 : p > 1000 ? 1000 : (int)p;
}

int bys_demod_level(const bys_demod_t *d, bys_band_t band, bys_demod_level_t *out) {
  if ((unsigned)band >= BYS_BAND_COUNT || !out || d->hops == 0) return 0;
  const bys_demod_reader_t *r = &d->rd[band];
  if (!r->enabled) return 0;
  const float *f = d->pool + r->pool_off;
  float mean = 0, peak = 0;
  for (unsigned t = 0; t < r->tones; t++) { const float e = energy_at(d, r, d->hops - 1, t); mean += f[t]; if (e > peak) peak = e; }
  mean /= (float)r->tones;
  if (mean < MIN_ENERGY) mean = MIN_ENERGY;
  if (peak < MIN_ENERGY) peak = MIN_ENERGY;
  out->peak_db = 10.0f * log10f(peak);
  out->floor_db = 10.0f * log10f(mean);
  out->snr_db = out->peak_db - out->floor_db;
  out->hold_snr_db = r->hold > 0 ? 10.0f * log10f(r->hold) : 0.0f;
  return 1;
}

void bys_demod_levels_reset(bys_demod_t *d) { for (unsigned b = 0; b < BYS_BAND_COUNT; b++) d->rd[b].hold = 0; }
