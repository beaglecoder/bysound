/*
 * Demodulator tests (bys_demod_*). Built once per engine (FFT and Goertzel, see the Makefile); each binary writes a
 * transcript (argument 1) and `make test` compares the two: they must make exactly the same decisions.
 *   test_demod <transcript> [--sweep] [--bench]
 */
#define _POSIX_C_SOURCE 200112L
#include "bysound.h"
#include "golden_cases.h"
#include "vectors.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef GOLDEN_DIR
#define GOLDEN_DIR "test/golden"
#endif
#ifndef VECTORS_DIR
#define VECTORS_DIR "../test-vectors"
#endif
#ifndef EVAL_DIR
#define EVAL_DIR "test/recordings"
#endif

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if defined(BYS_DEMOD_FFT)
#define ENGINE "float32 FFT"
#else
#define ENGINE "integer Goertzel"
#endif

static FILE *tr; /* transcript */

/* ---------- randomness ---------- */
static uint32_t rs = 987654321u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static double unif(void) { return (rnd() >> 8) / 16777216.0 + 1e-9; }
static double gauss(void) { return sqrt(-2.0 * log(unif())) * cos(6.283185307179586 * unif()); }
static void seed(uint32_t s) { rs = s * 2654435761u + 12345u; if (!rs) rs = 1; for (int i = 0; i < 8; i++) rnd(); }

/* ---------- test frames ---------- */
#define SHORT_TXT "{\"m\":\"Almac\xc3\xa9n\",\"a\":1250.5}"
#define LONG_TXT "00020101021226470017com.example.pagos01220000003100099988877766520454115303032540812500.505802AR5918ALMACEN LA ESQUINA6007CORDOBA62250108F-0001230509PEDIDO-776304F4AF \xc3\xb1" "and\xc3\xba \xf0\x9f\x99\x82"
enum { K_PAYLOAD, K_REF, K_ACK, K_RISK };
typedef struct { int kind, band, profile, set, receipt; const char *text; uint8_t fp[12]; uint8_t h4[4]; unsigned reason, source, hops; uint8_t own[3], intr[3]; } tcase_t;

static const uint8_t FP[12] = { 7, 36, 65, 94, 123, 152, 181, 210, 239, 12, 41, 70 };

static tcase_t mk_payload(int band, int profile, const char *t) { tcase_t c; memset(&c, 0, sizeof c); c.kind = K_PAYLOAD; c.band = band; c.profile = profile; c.text = t; return c; }
static tcase_t mk_ref(int band, int profile, int set) { tcase_t c; memset(&c, 0, sizeof c); c.kind = K_REF; c.band = band; c.profile = profile; c.set = set; memcpy(c.fp, FP, 12); return c; }
static tcase_t mk_ack(int band, int receipt) { tcase_t c; memset(&c, 0, sizeof c); c.kind = K_ACK; c.band = band; c.receipt = receipt; c.h4[0] = 0xde; c.h4[1] = 0xad; c.h4[2] = 0xbe; c.h4[3] = 0xef; return c; }
static tcase_t mk_risk(int band) { tcase_t c; memset(&c, 0, sizeof c); c.kind = K_RISK; c.band = band; c.reason = 3; c.source = 1; c.hops = 1; c.own[0] = 1; c.own[1] = 2; c.own[2] = 3; c.intr[0] = 9; c.intr[1] = 8; c.intr[2] = 7; return c; }

static int tx_of(bys_tx_t *tx, const bys_tx_cfg_t *cfg, const tcase_t *c) {
  switch (c->kind) {
    case K_PAYLOAD: return bys_tx_init_payload(tx, cfg, (bys_band_t)c->band, (bys_profile_t)c->profile, c->text, strlen(c->text));
    case K_REF: return bys_tx_init_reference(tx, cfg, (bys_band_t)c->band, (bys_profile_t)c->profile, (unsigned)c->set, c->fp);
    case K_ACK: return bys_tx_init_ack(tx, cfg, (bys_band_t)c->band, c->receipt, c->h4);
    default: return bys_tx_init_risk(tx, cfg, (bys_band_t)c->band, c->reason, c->source, c->hops, c->own, c->intr);
  }
}

#define MAXS 1500000
static int16_t SIG[MAXS], MIX[MAXS + 200000], LAP[MAXS];
static size_t lap_len;

/* One repetition (frame + gap) into LAP, at the sender's amplitude `level`. */
static void render_lap(const tcase_t *c, unsigned rate, float level) {
  bys_tx_cfg_t cfg;
  bys_tx_cfg_default(&cfg);
  cfg.sample_rate = rate;
  cfg.level = level;
  bys_tx_t tx;
  if (tx_of(&tx, &cfg, c) != BYS_OK) { lap_len = 0; return; }
  lap_len = 0;
  while (!bys_tx_done(&tx) && lap_len + 4096 <= MAXS) lap_len += bys_tx_fill(&tx, LAP + lap_len, 4096);
}

/* Linear resampling: the output advances `ratio` input samples per output sample (a clock different from the sender's). */
static size_t resample(const int16_t *in, size_t n, double ratio, int16_t *out) {
  size_t m = 0;
  for (double p = 0; p + 1 < (double)n; p += ratio) { size_t i = (size_t)p; double f = p - (double)i; out[m++] = (int16_t)lrint(in[i] * (1 - f) + in[i + 1] * f); }
  return m;
}

/* MIX = silence + repetitions + silence, with Gaussian white noise of `sigma` (fraction of full scale). */
static size_t build(unsigned rate, size_t lead, int laps, double sigma, double ppm) {
  size_t n = 0;
  memset(MIX, 0, lead * sizeof(int16_t));
  n = lead;
  for (int l = 0; l < laps; l++) {
    if (ppm != 0) { size_t m = resample(LAP, lap_len, 1.0 + ppm * 1e-6, SIG); memcpy(MIX + n, SIG, m * sizeof(int16_t)); n += m; }
    else { memcpy(MIX + n, LAP, lap_len * sizeof(int16_t)); n += lap_len; }
  }
  size_t tail = (size_t)(0.3 * rate);
  memset(MIX + n, 0, tail * sizeof(int16_t));
  n += tail;
  if (sigma > 0)
    for (size_t i = 0; i < n; i++) { double v = MIX[i] + gauss() * sigma * 32768.0; MIX[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }
  return n;
}

static void add_noise(size_t n, double sigma) {
  for (size_t i = 0; i < n; i++) { double v = MIX[i] + gauss() * sigma * 32768.0; MIX[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }
}

/* ---------- listening ---------- */
#define MAXEV 64
typedef struct { int kind, band, profile; int64_t at; uint8_t fp[12], h4[4]; int receipt; unsigned reason, hops; char text[700]; } ev_t;
typedef struct {
  bys_demod_t *d;
  int n, clashes, headers;
  ev_t ev[MAXEV];
} listen_t;

static void on_event(void *user, const bys_demod_event_t *e) {
  listen_t *L = user;
  if (e->type == BYS_EV_CLASH) { L->clashes++; return; }
  if (e->type == BYS_EV_HEADER) { L->headers++; return; }
  if (L->n >= MAXEV) return;
  ev_t *x = &L->ev[L->n++];
  memset(x, 0, sizeof *x);
  x->kind = e->result->kind; x->band = e->band; x->profile = e->profile; x->at = L->d->total;
  memcpy(x->fp, e->result->fingerprint, 12); memcpy(x->h4, e->result->hash4, 4);
  x->receipt = e->result->receipt; x->reason = e->result->reason; x->hops = e->result->hops;
  if (e->payload) { int n = bys_unpack(e->payload, e->payload_len, x->text, sizeof x->text); if (n < 0) strcpy(x->text, "<unpack failed>"); }
}

static bys_demod_t D;
static uint8_t OUTBUF[255 * 48 + 8];

/* chunks: 0 = 480 at a time; otherwise random sizes from 1 to `chunks`. */
static void run(listen_t *L, unsigned rate, unsigned profile, unsigned mask, const int16_t *x, size_t n, unsigned chunks, unsigned hop_mult) {
  bys_demod_cfg_t cfg;
  bys_demod_cfg_default(&cfg);
  cfg.sample_rate = rate; cfg.profile = (bys_profile_t)profile; cfg.band_mask = (uint8_t)mask; cfg.hop_mult = (uint8_t)hop_mult;
  cfg.out = OUTBUF; cfg.out_cap = sizeof OUTBUF; cfg.cb = on_event; cfg.user = L;
  memset(L, 0, sizeof *L);
  L->d = &D;
  int r = bys_demod_init(&D, &cfg);
  if (r != BYS_OK) { printf("  init failed %d (rate %u)\n", r, rate); CHECK(r == BYS_OK); return; }
  size_t i = 0;
  while (i < n) {
    size_t c = chunks ? 1 + rnd() % chunks : 480;
    if (c > n - i) c = n - i;
    bys_demod_push(&D, x + i, c);
    i += c;
  }
}

static int matches(const listen_t *L, int idx, const tcase_t *c) {
  const ev_t *e = &L->ev[idx];
  if (e->band != c->band || e->profile != c->profile) return 0;
  switch (c->kind) {
    case K_PAYLOAD: return e->kind == BYS_KIND_PAYLOAD && !strcmp(e->text, c->text);
    case K_REF: return e->kind == BYS_KIND_REFERENCE && !memcmp(e->fp, c->fp, 12);
    case K_ACK: return e->kind == BYS_KIND_ACK && !memcmp(e->h4, c->h4, 4) && e->receipt == c->receipt;
    default: return e->kind == BYS_KIND_RISK && e->reason == c->reason && e->hops == c->hops;
  }
}
static int count_ok(const listen_t *L, const tcase_t *c) { int k = 0; for (int i = 0; i < L->n; i++) k += matches(L, i, c); return k; }
static uint32_t fnv(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }

static unsigned all_normal(unsigned rate) {
  unsigned m = 0;
  for (unsigned b = 0; b < BYS_BAND_COUNT; b++) {
    bys_band_info_t bi;
    bys_band_info((bys_band_t)b, BYS_NORMAL, &bi);
    if (((uint64_t)bi.bin + bi.groups * 16u - 1u) * 93750u < (uint64_t)rate * 1000u && (b != BYS_AUDIBLE || BYS_DEMOD_LOW_BANK)) m |= 1u << b;
  }
  return m;
}
static const char *BN[] = { "top", "high", "upper", "mid", "audible", "xhigh", "wide" };

static void log_listen(const char *name, const listen_t *L) {
  fprintf(tr, "%s: events=%d clashes=%d headers=%d", name, L->n, L->clashes, L->headers);
  for (int i = 0; i < L->n; i++) fprintf(tr, " [k%d b%d p%d f%08x @%lld]", L->ev[i].kind, L->ev[i].band, L->ev[i].profile, fnv(L->ev[i].text) ^ (uint32_t)L->ev[i].fp[0] ^ ((uint32_t)L->ev[i].h4[0] << 8), (long long)L->ev[i].at);
  fprintf(tr, "\n");
}

/* ---------- 1. the bys_tx golden files ---------- */
static int16_t GOLD[200000];
static void test_golden(void) {
  printf("1. bys_tx golden audio, decoded (%s)\n", ENGINE);
  for (size_t i = 0; i < N_GOLDEN; i++) {
    const golden_case_t *g = &GOLDEN[i];
    char path[256];
    snprintf(path, sizeof path, "%s/%s.i16", GOLDEN_DIR, g->id);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) continue;
    size_t ng = fread(GOLD, 2, sizeof GOLD / 2, f);
    fclose(f);
    tcase_t c;
    if (g->is_ack) { const vec_ack_t *v = &V_ACKS[g->index]; c = mk_ack(v->band, v->receipt); uint8_t b[32]; for (int k = 0; k < 5; k++) { unsigned hi = v->body[2 * k], lo = v->body[2 * k + 1]; hi = hi <= '9' ? hi - '0' : hi - 'a' + 10; lo = lo <= '9' ? lo - '0' : lo - 'a' + 10; b[k] = (uint8_t)(hi << 4 | lo); } memcpy(c.h4, b + 1, 4); }
    else {
      const vec_frame_t *v = &V_FRAMES[g->index];
      if (v->payload) c = mk_payload(v->band, v->robust, v->text);
      else { c = mk_ref(v->band, v->robust, v->set); for (int k = 0; k < 12; k++) { unsigned hi = v->body[2 * k], lo = v->body[2 * k + 1]; hi = hi <= '9' ? hi - '0' : hi - 'a' + 10; lo = lo <= '9' ? lo - '0' : lo - 'a' + 10; c.fp[k] = (uint8_t)(hi << 4 | lo); } }
    }
    size_t lead = g->rate / 4;
    memset(MIX, 0, lead * 2);
    memcpy(MIX + lead, GOLD, ng * 2);
    size_t n = lead + ng;
    memset(MIX + n, 0, g->rate / 4 * 2);
    n += g->rate / 4;
    listen_t L;
    run(&L, g->rate, c.profile, c.profile ? 0x0f : all_normal(g->rate), MIX, n, 0, 1);
    int ok = count_ok(&L, &c);
    printf("  %-22s %6zu samples: %d frame(s) ok, %d in total, clashes %d\n", g->id, n, ok, L.n, L.clashes);
    CHECK(ok == 1 && L.n == 1 && L.clashes == 0);
    char nm[64]; snprintf(nm, sizeof nm, "golden %s", g->id);
    log_listen(nm, &L);
  }
}

/* ---------- 2. frames generated in the test ---------- */
static void test_matrix(void) {
  printf("2. round trip: frames, bands, rates, noise, offset, amplitude and drift (%s)\n", ENGINE);
  static const unsigned rates[] = { 48000, 44100 };
  int total = 0, good = 0;
  for (unsigned ri = 0; ri < 2; ri++) {
    unsigned rate = rates[ri];
    for (unsigned b = 0; b < BYS_BAND_COUNT; b++) {
      if (!(all_normal(rate) >> b & 1)) continue;
      tcase_t cs[6] = { mk_payload((int)b, 0, SHORT_TXT), mk_payload((int)b, 0, LONG_TXT), mk_ref((int)b, 0, 0), mk_ref((int)b, 0, 1), mk_ack((int)b, (int)(b & 1)), mk_risk((int)b) };
      for (int k = 0; k < 6; k++) {
        seed(b * 100 + (unsigned)k + ri * 7);
        render_lap(&cs[k], rate, 1.0f);
        double sigma = 0.15;
        size_t lead = (size_t)(0.1 * rate + rnd() % (rate / 2));
        size_t n = build(rate, lead, 2, sigma, 0);
        listen_t L;
        run(&L, rate, 0, all_normal(rate), MIX, n, 0, 1);
        int ok = count_ok(&L, &cs[k]);
        total++; good += ok > 0;
        if (!(ok > 0 && L.n == ok && L.clashes == 0)) printf("  %u Hz %-7s case %d (lead %zu): %d ok of %d, clashes %d\n", rate, BN[b], k, lead, ok, L.n, L.clashes);
        CHECK(ok > 0 && L.n == ok && L.clashes == 0);
        char nm[80]; snprintf(nm, sizeof nm, "matrix %u %s %d", rate, BN[b], k);
        log_listen(nm, &L);
      }
    }
  }
  printf("  matrix with noise 0.15 of full scale: %d of %d ok\n", good, total);

  /* robust (the robust demodulator is separate state, with a double window) */
#if BYS_DEMOD_ROBUST
  {
    int rt = 0, rg = 0;
    for (unsigned ri = 0; ri < 2; ri++) for (int b = 0; b < 4; b++) {
      tcase_t cs[3] = { mk_payload(b, 1, SHORT_TXT), mk_ref(b, 1, 0), mk_ref(b, 1, 1) };
      for (int k = 0; k < 3; k++) {
        unsigned rate = rates[ri];
        seed(500 + (unsigned)(b * 3 + k));
        render_lap(&cs[k], rate, 1.0f);
        size_t n = build(rate, rate / 3, 2, 0.2, 0);
        listen_t L;
        run(&L, rate, 1, 0x0f, MIX, n, 0, 1);
        int ok = count_ok(&L, &cs[k]);
        rt++; rg += ok > 0;
        if (!(ok > 0 && L.n == ok && L.clashes == 0)) printf("  robust %u Hz %s case %d: %d ok of %d\n", rate, BN[b], k, ok, L.n);
        CHECK(ok > 0 && L.n == ok && L.clashes == 0);
        char nm[80]; snprintf(nm, sizeof nm, "robust %u %s %d", rate, BN[b], k);
        log_listen(nm, &L);
        /* a normal demodulator does not hear a robust frame */
        if (k == 1) { run(&L, rate, 0, 0x0f, MIX, n, 0, 1); CHECK(L.n == 0); }
      }
    }
    printf("  robust with noise 0.2: %d of %d ok\n", rg, rt);
    /* and a normal frame does not come out as robust */
    tcase_t c = mk_ref(0, 0, 0);
    render_lap(&c, 48000, 1.0f);
    size_t n = build(48000, 12000, 1, 0.05, 0);
    listen_t L;
    run(&L, 48000, 1, 0x0f, MIX, n, 0, 1);
    CHECK(L.n == 0);
    run(&L, 48000, 0, 0x0f, MIX, n, 0, 1);
    CHECK(L.n == 1 && L.ev[0].profile == 0);
  }
#endif

  /* amplitude 0.05..1 with proportional noise (fixed ratio) and a minimum floor */
  {
    int t = 0, g = 0;
    static const float amps[] = { 0.05f, 0.1f, 0.25f, 0.5f, 1.0f };
    for (int ai = 0; ai < 5; ai++) for (int b = 0; b < 4; b++) {
      tcase_t c = mk_payload(b, 0, SHORT_TXT);
      render_lap(&c, 48000, amps[ai]);
      seed(900 + (unsigned)(ai * 4 + b));
      size_t n = build(48000, 20000, 2, 0.1 * amps[ai] + 0.0005, 0);
      listen_t L;
      run(&L, 48000, 0, 0x0f, MIX, n, 0, 1);
      int ok = count_ok(&L, &c);
      t++; g += ok > 0;
      if (!(ok > 0)) printf("  amplitude %.2f %s: no frame\n", amps[ai], BN[b]);
      CHECK(ok > 0 && L.n == ok);
      char nm[80]; snprintf(nm, sizeof nm, "amplitude %.2f %s", amps[ai], BN[b]);
      log_listen(nm, &L);
    }
    printf("  amplitude 0.05..1 at a fixed signal-to-noise ratio: %d of %d ok\n", g, t);
  }

  /* clock drift between sender and receiver: the TS does not compensate for it (only the preamble fixes the timing) */
  {
    static const double ppms[] = { -400, -200, -100, 100, 200, 400 };
    printf("  clock drift (short and long payload, top and mid, 48 kHz):");
    for (int p = 0; p < 6; p++) {
      int ok1 = 0, n1 = 0;
      for (int b = 0; b < 4; b += 3) for (int k = 0; k < 2; k++) {
        tcase_t c = mk_payload(b, 0, k ? LONG_TXT : SHORT_TXT);
        render_lap(&c, 48000, 1.0f);
        seed(1300 + (unsigned)p * 8 + (unsigned)(b + k));
        size_t n = build(48000, 15000, 2, 0.05, ppms[p]);
        listen_t L;
        run(&L, 48000, 0, 0x0f, MIX, n, 0, 1);
        n1++; ok1 += count_ok(&L, &c) > 0;
        char nm[80]; snprintf(nm, sizeof nm, "drift %+.0f %s %d", ppms[p], BN[b], k);
        log_listen(nm, &L);
        if (fabs(ppms[p]) <= 200) CHECK(count_ok(&L, &c) > 0);
      }
      printf(" %+.0fppm=%d/%d", ppms[p], ok1, n1);
    }
    printf("\n");
  }
}

/* ---------- 3. other properties ---------- */
static void test_misc(void) {
  printf("3. streaming, silence, clashes, hop_mult (%s)\n", ENGINE);
  tcase_t c = mk_payload(BYS_MID, 0, LONG_TXT);
  render_lap(&c, 48000, 1.0f);
  seed(77);
  size_t n = build(48000, 24000, 2, 0.1, 0);
  listen_t A, B;
  run(&A, 48000, 0, 0x0f, MIX, n, 0, 1);
  run(&B, 48000, 0, 0x0f, MIX, n, 1777, 1);
  CHECK(A.n == B.n && A.n >= 1 && !memcmp(&A.ev[0].text, &B.ev[0].text, 100) && A.ev[0].at == B.ev[0].at);
  printf("  the feed chunk size changes nothing: %d = %d frames, same instant %s\n", A.n, B.n, A.ev[0].at == B.ev[0].at ? "yes" : "no");
  CHECK(sizeof D < 64 * 1024 || BYS_DEMOD_ROBUST || BYS_DEMOD_LOW_BANK);

  /* half the cost: 8 ms step */
  listen_t H;
  run(&H, 48000, 0, 0x0f, MIX, n, 0, 2);
  printf("  hop_mult 2 (8 ms step): %d frames ok\n", count_ok(&H, &c));
  CHECK(count_ok(&H, &c) >= 1);
  log_listen("hop_mult 2", &H);

  /* silence and noise alone: nothing, and zero failures */
  {
    size_t m = 48000 * 4;
    memset(MIX, 0, m * 2);
    listen_t L;
    run(&L, 48000, 0, all_normal(48000), MIX, m, 0, 1);
    CHECK(L.n == 0 && L.headers == 0 && bys_demod_stats(&D)->failures == 0);
    seed(5);
    for (size_t i = 0; i < m; i++) { double v = gauss() * 0.3 * 32768.0; MIX[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }
    run(&L, 48000, 0, all_normal(48000), MIX, m, 0, 1);
    printf("  white noise of 0.3 for 4 s: %d frames, %d headers, %u failures\n", L.n, L.headers, bys_demod_stats(&D)->failures);
    CHECK(L.n == 0 && L.headers == 0 && bys_demod_stats(&D)->failures == 0);
    log_listen("white noise", &L);
  }

  /* two senders on the same band (a preamble on top of the body) and on different bands */
  {
    tcase_t a = mk_payload(BYS_UPPER, 0, LONG_TXT), b = mk_payload(BYS_UPPER, 0, SHORT_TXT " otro");
    render_lap(&a, 48000, 1.0f);
    size_t la = lap_len;
    memcpy(SIG, LAP, la * 2);
    render_lap(&b, 48000, 1.0f);
    size_t off = 48000;
    memset(MIX, 0, (la + 100000) * 2);
    for (size_t i = 0; i < la; i++) MIX[i] = (int16_t)(SIG[i] / 2);
    for (size_t i = 0; i < lap_len; i++) { int v = MIX[i + off] + LAP[i] / 2; MIX[i + off] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    listen_t L;
    seed(31);
    add_noise(la + 100000, 0.05);
    run(&L, 48000, 0, 0x0f, MIX, la + 100000, 0, 1);
    printf("  two senders on upper: %d clashes, %d frames\n", L.clashes, L.n);
    CHECK(L.clashes > 0);
    log_listen("same-band clash", &L);

    tcase_t t = mk_payload(BYS_TOP, 0, LONG_TXT), u = mk_payload(BYS_MID, 0, LONG_TXT);
    render_lap(&t, 48000, 1.0f); la = lap_len; memcpy(SIG, LAP, la * 2);
    render_lap(&u, 48000, 1.0f);
    off = (size_t)(0.6 * 48000);
    memset(MIX, 0, (la + 100000) * 2);
    memcpy(MIX, SIG, la * 2);
    for (size_t i = 0; i < lap_len; i++) { int v = MIX[i + off] + LAP[i] / 2; MIX[i + off] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    add_noise(la + 100000, 0.05);
    run(&L, 48000, 0, 0x0f, MIX, la + 100000, 0, 1);
    printf("  two senders on top and mid: %d clashes, %d frames, %d headers\n", L.clashes, L.n, L.headers);
    CHECK(L.clashes > 0);
    log_listen("cross-band clash", &L);
  }

  /* reference that hops bands and starts on A and B */
  {
    tcase_t laps[4] = { mk_ref(BYS_TOP, 0, 0), mk_ref(BYS_TOP, 0, 1), mk_ref(BYS_TOP, 0, 0), mk_ref(BYS_HIGH, 0, 0) };
    size_t at = 24000;
    memset(MIX, 0, 2400000);
    for (int i = 0; i < 4; i++) { render_lap(&laps[i], 48000, 1.0f); memcpy(MIX + at, LAP, lap_len * 2); at += lap_len; }
    at += 20000;
    seed(21);
    for (size_t i = 0; i < at; i++) { double v = MIX[i] + gauss() * 0.2 * 32768.0; MIX[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }
    listen_t L;
    run(&L, 48000, 0, 0x0f, MIX, at, 0, 1);
    int ok = L.n == 4;
    for (int i = 0; i < L.n && i < 4; i++) ok &= matches(&L, i, &laps[i]);
    printf("  reference hopping bands (top A, top B, top A, high A): %d frames, %s\n", L.n, ok ? "all ok and in order" : "NO");
    CHECK(ok && L.clashes == 0);
    log_listen("band hop", &L);
  }

  /* per-band levels */
  {
    tcase_t t = mk_ref(BYS_HIGH, 0, 0);
    render_lap(&t, 48000, 1.0f);
    size_t m = build(48000, 24000, 1, 0.01, 0);
    listen_t L;
    run(&L, 48000, 0, 0x0f, MIX, m, 0, 1);
    bys_demod_level_t lv_hi, lv_mid;
    int a = bys_demod_level(&D, BYS_HIGH, &lv_hi), b = bys_demod_level(&D, BYS_MID, &lv_mid);
    printf("  level with emission on high: held SNR high %.1f dB, mid %.1f dB\n", lv_hi.hold_snr_db, lv_mid.hold_snr_db);
    CHECK(a && b && lv_hi.hold_snr_db > lv_mid.hold_snr_db + 3.0f);
    CHECK(bys_demod_level(&D, BYS_AUDIBLE, &lv_hi) == 0 || BYS_DEMOD_LOW_BANK);
  }
}

/* ---------- 4. recordings ---------- */
static void test_recordings(void) {
  printf("4. recordings (evaluation WAV files)\n");
  static const char *wavs[] = { "top-v4", "mid-v4", "upper-v4", "v4", "cobro-v4", "falso-v4" };
  for (unsigned i = 0; i < sizeof wavs / sizeof *wavs; i++) {
    char p[300];
    snprintf(p, sizeof p, "%s/%s.wav", EVAL_DIR, wavs[i]);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("  %s.wav: no file (skipped)\n", wavs[i]); continue; }
    uint8_t hdr[44];
    size_t got = fread(hdr, 1, 44, f);
    size_t ns = got == 44 ? fread(MIX, 2, MAXS, f) : 0;
    fclose(f);
    listen_t L;
    run(&L, 48000, 0, all_normal(48000), MIX, ns, 0, 1);
    {
      int want_band = !strcmp(wavs[i], "mid-v4") ? BYS_MID : !strcmp(wavs[i], "upper-v4") ? BYS_UPPER : BYS_TOP;
      int is_payload = !strcmp(wavs[i], "falso-v4");
      CHECK(L.n == 1 && L.ev[0].band == want_band && L.ev[0].kind == (is_payload ? BYS_KIND_PAYLOAD : BYS_KIND_REFERENCE) && L.clashes == 0);
      if (is_payload) CHECK(!strncmp(L.ev[0].text, "000201010212264700", 18));
    }
    printf("  %-14s %7zu samples: %d frame(s)", wavs[i], ns, L.n);
    for (int e = 0; e < L.n && e < 3; e++) printf(" [%s type %d \"%.28s\"]", BN[L.ev[e].band], L.ev[e].kind, L.ev[e].text);
    printf(", clashes %d\n", L.clashes);
    log_listen(wavs[i], &L);
  }
}

/* ---------- 5. noise sweep ---------- */
static void test_sweep(int full) {
  static const double sig[] = { 0.3, 0.7, 1.0, 1.2, 1.3, 1.4, 1.5, 1.6 }; /* from 1.0 up the noise already clips in int16 */
  const int trials = full ? 10 : 2, nsig = full ? 8 : 2;
  printf("5. white noise sweep (%s), short payload in 2 repetitions, %d trials per cell\n", ENGINE, trials);
  printf("   sigma (full scale) and per-tone SNR in its bin; cell = trials with the frame ok in either of the 2 repetitions / in the first\n");
  printf("   %-7s", "band");
  for (int s = 0; s < nsig; s++) {
    double sg = full ? sig[s] : (s ? 0.3 : 0.1);
    double a = 0.3, snr = 10.0 * log10((a * a * 1024.0 / 4.0) / (sg * sg * 0.95 * 1.0));
    printf(" %4.1f/%+5.1fdB", sg, snr);
  }
  printf("\n");
  for (int b = 0; b < 4; b++) {
    printf("   %-7s", BN[b]);
    for (int s = 0; s < nsig; s++) {
      double sg = full ? sig[s] : (s ? 0.3 : 0.1);
      int any = 0, first = 0;
      for (int t = 0; t < trials; t++) {
        tcase_t c = mk_payload(b, 0, SHORT_TXT);
        render_lap(&c, 48000, 1.0f);
        seed(4000 + (unsigned)(b * 1000 + s * 50 + t));
        size_t n = build(48000, 4000 + rnd() % 30000, 2, sg, 0);
        listen_t L;
        run(&L, 48000, 0, 0x0f, MIX, n, 0, 1);
        int ok = count_ok(&L, &c);
        any += ok > 0;
        first += ok > 0 && L.ev[0].at < (int64_t)(n * 0.62);
        if (!full && s == 1) CHECK(ok > 0);
      }
      printf("   %2d/%2d     ", any, first);
      fprintf(tr, "sweep %s %.1f: %d %d\n", BN[b], sg, any, first);
    }
    printf("\n");
  }
}

/* ---------- 6. CPU cost ---------- */
static void bench(void) {
  static const unsigned masks[] = { 0x0f, 0x01 };
  tcase_t c = mk_payload(BYS_MID, 0, LONG_TXT);
  render_lap(&c, 48000, 1.0f);
  seed(3);
  size_t n = build(48000, 24000, 3, 0.1, 0);
  for (int hm = 1; hm <= 2; hm++) for (int m = 0; m < 2; m++) {
    listen_t L;
    bys_demod_cfg_t cfg;
    bys_demod_cfg_default(&cfg);
    cfg.band_mask = (uint8_t)masks[m]; cfg.hop_mult = (uint8_t)hm; cfg.out = OUTBUF; cfg.out_cap = sizeof OUTBUF; cfg.cb = on_event; cfg.user = &L;
    memset(&L, 0, sizeof L); L.d = &D;
    bys_demod_init(&D, &cfg);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < n; i += 480) bys_demod_push(&D, MIX + i, i + 480 <= n ? 480 : n - i);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("   band %-9s hop %d ms: %.2f Msamples/s on the host (%.0fx real time at 48 kHz), %d frame(s)\n", masks[m] == 1 ? "top only" : "4 bands", 4 * hm, (double)n / s / 1e6, (double)n / s / 48000.0, L.n);
  }
}

int main(int argc, char **argv) {
  int sweep = 0, do_bench = 0;
  const char *trfile = "/dev/null";
  for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--sweep")) sweep = 1; else if (!strcmp(argv[i], "--bench")) do_bench = 1; else trfile = argv[i]; }
  tr = fopen(trfile, "w");
  if (!tr) tr = stdout;
  printf("bysound demod: engine %s, sizeof(bys_demod_t) = %zu B (robust %d, audible %d, max rate %u)\n", ENGINE, sizeof(bys_demod_t), BYS_DEMOD_ROBUST, BYS_DEMOD_LOW_BANK, (unsigned)BYS_DEMOD_MAX_RATE);
  if (do_bench) { bench(); return 0; }
  if (sweep) { test_sweep(1); return 0; }
  test_golden();
  test_matrix();
  test_misc();
  test_recordings();
  test_sweep(0);
  if (tr != stdout) fclose(tr);
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
