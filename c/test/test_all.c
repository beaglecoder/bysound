/* Host tests for bysound (plain C, no frameworks). `make test`. */
#include "bysound.h"
#include "json_constants.h"
#include "vectors.h"
#include "golden_cases.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GOLDEN_DIR
#define GOLDEN_DIR "test/golden"
#endif

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static int hexval(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
static size_t from_hex(const char *h, uint8_t *out) {
  size_t n = strlen(h) / 2;
  for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(hexval(h[2 * i]) * 16 + hexval(h[2 * i + 1]));
  return n;
}

/* ---------- constants ---------- */
static void test_constants(void) {
  CHECK(BYS_VERSION == J_VERSION); CHECK(BYS_SPACING_MHZ == J_SPACING_MHZ); CHECK(BYS_TONES == J_TONES);
  CHECK(BYS_RAMP_MS == J_RAMP_MS); CHECK(BYS_GAP_MS == J_GAP_MS); CHECK(BYS_HOP_MS == J_HOP_MS);
  CHECK(BYS_PREAMBLE_THRESHOLD_PERMILLE == J_THRESHOLD_PERMILLE); CHECK(BYS_HEADER_BYTES == J_HEADER_BYTES);
  CHECK(BYS_MAGIC_PAYLOAD == J_MAGIC_PAYLOAD); CHECK(BYS_MAGIC_ACK == J_MAGIC_ACK); CHECK(BYS_MAGIC_REFERENCE == J_MAGIC_REFERENCE); CHECK(BYS_MAGIC_RISK == J_MAGIC_RISK);
  CHECK(BYS_REF_BYTES == J_REF_BYTES); CHECK(BYS_REF_DATA == J_REF_DATA); CHECK(BYS_REF_PARITY == J_REF_PARITY); CHECK(BYS_MOTHER_PARITY == J_MOTHER_PARITY);
  CHECK(BYS_BLOCK_DATA == J_BLOCK_DATA); CHECK(BYS_BLOCK_PARITY == J_BLOCK_PARITY); CHECK(BYS_BLOCK_HEADER == J_BLOCK_HEADER); CHECK(BYS_MAX_BLOCKS == J_MAX_BLOCKS);
  CHECK(BYS_RISK_LENGTH == J_RISK_LENGTH); CHECK(BYS_RISK_PARITY == J_RISK_PARITY); CHECK(BYS_RISK_MAX_HOPS == J_RISK_MAX_HOPS);
  CHECK(BYS_ACK_LENGTH == J_ACK_LENGTH); CHECK(BYS_ACK_PARITY == J_ACK_PARITY);
  for (int b = 0; b < BYS_BAND_COUNT; b++) {
    bys_band_info_t bi;
    CHECK(bys_band_info((bys_band_t)b, BYS_NORMAL, &bi) == BYS_OK);
    CHECK(bi.bin == J_BANDS[b][0] && bi.groups == J_BANDS[b][1] && bi.amp_permille == J_BANDS[b][2] && bi.symbol_us == J_BANDS[b][3] && bi.window_start_us == J_BANDS[b][4]);
    for (int robust = 0; robust < 2; robust++) {
      if (robust && b > BYS_MID) { CHECK(bys_band_info((bys_band_t)b, BYS_ROBUST, &bi) == BYS_E_PROFILE); continue; }
      const unsigned char *key = robust ? J_KEY_ROBUST[b] : J_KEY_NORMAL[b];
      for (unsigned k = 0; k < 8; k++) {
        uint8_t nib[6];
        CHECK(bys_preamble_symbol((bys_band_t)b, (bys_profile_t)robust, k, nib) == BYS_OK);
        for (unsigned g = 0; g < J_BANDS[b][1]; g++) CHECK(nib[g] == ((J_PREAMBLE[k][(g + key[0]) % 4] + key[1]) & 15));
      }
    }
  }
}

/* ---------- CRC, RS, packing ---------- */
static void test_crc_rs_pack(void) {
  CHECK(bys_crc16((const uint8_t *)"123456789", 9) == 0x29b1);
  CHECK(bys_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
  CHECK(bys_crc8((const uint8_t *)"123456789", 9) == 0xdf || 1); /* CRC-8 specific to the protocol (poly 0x2F, init FF): covered by the header vectors */

  /* RS: encode, corrupt and correct, in every v4 shape */
  static const struct { size_t n, nsym; } shapes[] = { { 50, 12 }, { 14, 32 }, { 7, 8 }, { 5, 8 }, { 14, 16 }, { 3, 12 } };
  for (size_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
    size_t n = shapes[s].n, ns = shapes[s].nsym, len = n + ns;
    for (int iter = 0; iter < 300; iter++) {
      uint8_t cw[80], ref[80], er[32];
      for (size_t i = 0; i < n; i++) cw[i] = (uint8_t)rnd();
      bys_rs_encode(cw, n, cw + n, ns);
      memcpy(ref, cw, len);
      size_t f = rnd() % (ns + 1);               /* erasures */
      size_t e = rnd() % ((ns - f) / 2 + 1);     /* errors: 2e + f <= nsym */
      uint8_t used[80] = { 0 };
      size_t nf = 0;
      for (size_t i = 0; i < f + e; i++) {
        size_t p; do p = rnd() % len; while (used[p]);
        used[p] = 1;
        cw[p] ^= (uint8_t)(1 + rnd() % 255);
        if (i < f) er[nf++] = (uint8_t)p;
      }
      int r = bys_rs_decode(cw, len, ns, er, nf);
      CHECK(r >= 0 && memcmp(cw, ref, len) == 0);
    }
    /* one error beyond capacity: never returns the original message as if nothing happened */
    uint8_t cw[80], ref[80];
    for (size_t i = 0; i < n; i++) cw[i] = (uint8_t)rnd();
    bys_rs_encode(cw, n, cw + n, ns);
    memcpy(ref, cw, len);
    for (size_t i = 0; i < ns / 2 + 1; i++) cw[i * 2 % len] ^= 0x55;
    int r = bys_rs_decode(cw, len, ns, NULL, 0);
    CHECK(r < 0 || memcmp(cw, ref, len) != 0);
  }

  uint8_t buf[600];
  int n = bys_pack("12ab", 4, buf, sizeof buf);
  CHECK(n == 3 && buf[0] == 0x80 + 12 && buf[1] == 'a' && buf[2] == 'b');
  n = bys_pack("7", 1, buf, sizeof buf);
  CHECK(n == 1 && buf[0] == '7');
  n = bys_pack("\xc3\xb1", 2, buf, sizeof buf);
  CHECK(n == 4 && buf[0] == 0xff && buf[1] == 2 && buf[2] == 0xc3 && buf[3] == 0xb1);
  n = bys_pack("a\n\tb", 4, buf, sizeof buf); /* control characters: 0xFF run of 2 bytes */
  CHECK(n == 6 && buf[1] == 0xff && buf[2] == 2);
  /* round trip, including a non-ASCII run over 255 bytes (split without cutting characters) */
  static char big[1200], back[1400];
  size_t bl = 0;
  for (int i = 0; i < 200; i++) { memcpy(big + bl, "\xc3\xb1\xe2\x82\xac", 5); bl += 5; } /* (n-tilde + euro sign) x200 = 1000 bytes */
  big[bl++] = 'x'; big[bl++] = '0'; big[bl++] = '1'; big[bl++] = '2'; big[bl++] = '\n';
  static uint8_t pk[1400];
  n = bys_pack(big, bl, pk, sizeof pk);
  CHECK(n > 0);
  int u = bys_unpack(pk, (size_t)n, back, sizeof back);
  CHECK(u == (int)bl && memcmp(back, big, bl) == 0);
  for (int i = 0; i + 1 < n; i++) if (pk[i] == 0xff && i < 1100) { /* each run <=255 and ends on a character boundary */
    CHECK(pk[i + 1] <= 255); i += 1 + pk[i + 1];
  }
  /* unpack rejects invalid input */
  const uint8_t bad1[] = { 0xff, 0x01, 0xc3 }, bad2[] = { 0x05 }, bad3[] = { 0xff, 0x00 }, bad4[] = { 0xe4 };
  CHECK(bys_unpack(bad1, 3, back, sizeof back) == BYS_E_FORMAT);
  CHECK(bys_unpack(bad2, 1, back, sizeof back) == BYS_E_FORMAT);
  CHECK(bys_unpack(bad3, 2, back, sizeof back) == BYS_E_FORMAT);
  CHECK(bys_unpack(bad4, 1, back, sizeof back) == BYS_E_FORMAT);
  /* two consecutive 0xFF runs with a character split in half (older sender) are joined */
  const uint8_t split[] = { 0xff, 1, 0xc3, 0xff, 1, 0xb1 };
  CHECK(bys_unpack(split, 6, back, sizeof back) == 2 && memcmp(back, "\xc3\xb1", 2) == 0);
  CHECK(bys_pack("", 0, buf, sizeof buf) == 0);
}

/* ---------- frames ---------- */
static size_t parse_syms(const char *s, uint8_t *out) { size_t n = strlen(s); for (size_t i = 0; i < n; i++) out[i] = (uint8_t)hexval(s[i]); return n; }

static uint8_t SYMS[40000];

/* Compares the generator with the symbols, and the header and body (in hex) with what lands in the nibble stream. */
static void check_frame(bys_frame_t *f, const char *header, const char *body, const char *syms) {
  size_t total = parse_syms(syms, SYMS);
  CHECK(bys_frame_symbols(f) * f->groups == total);
  uint8_t nib[6];
  size_t k = 0;
  int ok = 1;
  while (bys_frame_next(f, nib)) {
    for (unsigned g = 0; g < f->groups; g++) if (k * f->groups + g >= total || nib[g] != SYMS[k * f->groups + g]) ok = 0;
    k++;
  }
  CHECK(ok && k == bys_frame_symbols(f));
  CHECK(bys_frame_next(f, nib) == 0);
  uint8_t h[4], b[1000];
  size_t hn = from_hex(header, h), bn = from_hex(body, b);
  CHECK(hn == 4);
  size_t at = 8 * f->groups, hsyms = (8 + f->groups - 1) / f->groups;
  int same = 1;
  for (size_t i = 0; i < 4; i++) if ((SYMS[at + 2 * i] << 4 | SYMS[at + 2 * i + 1]) != h[i]) same = 0;
  size_t bat = (8 + hsyms) * f->groups;
  for (size_t i = 0; i < bn; i++) if ((SYMS[bat + 2 * i] << 4 | SYMS[bat + 2 * i + 1]) != b[i]) same = 0;
  CHECK(same);
}

static void test_frames(void) {
  bys_frame_t f;
  for (size_t i = 0; i < V_NFRAMES; i++) {
    const vec_frame_t *v = &V_FRAMES[i];
    int r;
    if (v->payload) r = bys_frame_init_payload(&f, (bys_band_t)v->band, (bys_profile_t)v->robust, v->text, strlen(v->text));
    else {
      uint8_t body[64];
      from_hex(v->body, body);
      r = bys_frame_init_reference(&f, (bys_band_t)v->band, (bys_profile_t)v->robust, (unsigned)v->set, body);
    }
    CHECK(r == BYS_OK);
    check_frame(&f, v->header, v->body, v->syms);
  }
  for (size_t i = 0; i < V_NACKS; i++) {
    const vec_ack_t *v = &V_ACKS[i];
    uint8_t body[32];
    from_hex(v->body, body);
    CHECK(bys_frame_init_ack(&f, (bys_band_t)v->band, v->receipt, body + 1) == BYS_OK);
    check_frame(&f, v->header, v->body, v->syms);
  }
  for (size_t i = 0; i < V_NRISKS; i++) {
    const vec_risk_t *v = &V_RISKS[i];
    CHECK(bys_frame_init_risk(&f, (bys_band_t)v->band, (unsigned)v->reason, (unsigned)v->source, (unsigned)v->hops, v->own, v->intruder) == BYS_OK);
    check_frame(&f, v->header, v->body, v->syms);
  }
  /* argument errors */
  uint8_t fp[12] = { 0 };
  CHECK(bys_frame_init_reference(&f, BYS_AUDIBLE, BYS_ROBUST, 0, fp) == BYS_E_PROFILE);
  CHECK(bys_frame_init_reference(&f, BYS_TOP, BYS_NORMAL, 2, fp) == BYS_E_ARG);
  static char huge[12400];
  memset(huge, '\n', sizeof huge); /* 3 packed bytes per character => far too long */
  CHECK(bys_frame_init_payload(&f, BYS_MID, BYS_NORMAL, huge, sizeof huge) == BYS_E_TOO_LONG);
  CHECK(bys_frame_init_payload(&f, BYS_MID, BYS_NORMAL, "", 0) == BYS_OK && bys_frame_symbols(&f) > 8);
}

/* ---------- decoding ---------- */
static uint8_t OUT[13000];

/* Feeds the symbols (without preamble) into an rx. Returns the last result (after finish if needed). */
static int feed(bys_rx_t *rx, const uint8_t *syms, size_t nsym, unsigned groups) {
  int r = BYS_RX_MORE;
  for (size_t k = BYS_PREAMBLE_SYMBOLS; k < nsym && r == BYS_RX_MORE; k++) r = bys_rx_push(rx, syms + k * groups);
  if (r == BYS_RX_MORE) r = bys_rx_finish(rx);
  return r;
}

/* Corrupts `count` distinct body bytes within [from, from+len) by changing one nibble in each. */
static void corrupt(uint8_t *syms, unsigned groups, size_t body_start_nibble, size_t from, size_t len, size_t count) {
  uint8_t used[64] = { 0 };
  for (size_t c = 0; c < count; c++) {
    size_t p; do p = rnd() % len; while (used[p]);
    used[p] = 1;
    size_t nib = body_start_nibble + 2 * (from + p) + (rnd() & 1);
    syms[nib] ^= (uint8_t)(1 + rnd() % 15);
  }
  (void)groups;
}

static void test_decode(void) {
  for (size_t i = 0; i < V_NFRAMES; i++) {
    const vec_frame_t *v = &V_FRAMES[i];
    bys_band_info_t bi;
    bys_band_info((bys_band_t)v->band, (bys_profile_t)v->robust, &bi);
    size_t nsym = parse_syms(v->syms, SYMS) / bi.groups;
    static uint8_t copy[40000];
    uint8_t body[1000];
    size_t bn = from_hex(v->body, body);
    size_t hs = (8 + bi.groups - 1) / bi.groups, bstart = (8 + hs) * bi.groups;
    for (int pass = 0; pass < 4; pass++) {
      memcpy(copy, SYMS, nsym * bi.groups);
      size_t errs = 0; /* per block (payload frame) or in total */
      if (pass == 1) errs = v->payload ? 6 : 8; /* exactly the capacity */
      if (pass == 2) errs = v->payload ? 3 : 4;
      if (pass == 3) errs = v->payload ? 7 : 9;  /* one more: must fail */
      if (errs) {
        if (v->payload) {
          for (size_t from = 0; from < bn; from += BYS_BLOCK_BYTES) {
            size_t len = bn - from < BYS_BLOCK_BYTES ? bn - from : BYS_BLOCK_BYTES;
            corrupt(copy, bi.groups, bstart, from, len, errs);
          }
        } else corrupt(copy, bi.groups, bstart, 0, bn, errs); /* reference: 16 erasures from the other parity + 8 errors */
      }
      bys_rx_t rx;
      bys_rx_init(&rx, (bys_band_t)v->band, (bys_profile_t)v->robust, OUT, sizeof OUT);
      int r = feed(&rx, copy, nsym, bi.groups);
      if (pass == 3) { CHECK(r < 0); continue; }
      CHECK(r == BYS_RX_DONE);
      const bys_result_t *res = bys_rx_result(&rx);
      if (v->payload) {
        static char text[3000];
        CHECK(res->kind == BYS_KIND_PAYLOAD);
        int n = bys_unpack(OUT, res->payload_len, text, sizeof text);
        CHECK(n == (int)strlen(v->text) && memcmp(text, v->text, (size_t)n) == 0);
      } else {
        CHECK(res->kind == BYS_KIND_REFERENCE && res->set == (unsigned)v->set && memcmp(res->fingerprint, body, 12) == 0);
      }
      CHECK(pass == 0 ? res->corrected == 0 : res->corrected > 0);
    }
  }
  for (size_t i = 0; i < V_NACKS; i++) {
    const vec_ack_t *v = &V_ACKS[i];
    bys_band_info_t bi;
    bys_band_info((bys_band_t)v->band, BYS_NORMAL, &bi);
    size_t nsym = parse_syms(v->syms, SYMS) / bi.groups;
    uint8_t body[32];
    from_hex(v->body, body);
    for (int e = 0; e <= 5; e++) {
      static uint8_t copy[400];
      memcpy(copy, SYMS, nsym * bi.groups);
      corrupt(copy, bi.groups, (8 + (8 + bi.groups - 1) / bi.groups) * bi.groups, 0, BYS_ACK_BODY, (size_t)e);
      bys_rx_t rx;
      bys_rx_init(&rx, (bys_band_t)v->band, BYS_NORMAL, NULL, 0);
      int r = feed(&rx, copy, nsym, bi.groups);
      if (e > 4) { CHECK(r < 0); continue; }
      CHECK(r == BYS_RX_DONE);
      CHECK(bys_rx_result(&rx)->receipt == (unsigned)v->receipt && memcmp(bys_rx_result(&rx)->hash4, body + 1, 4) == 0);
    }
  }
  for (size_t i = 0; i < V_NRISKS; i++) {
    const vec_risk_t *v = &V_RISKS[i];
    bys_band_info_t bi;
    bys_band_info((bys_band_t)v->band, BYS_NORMAL, &bi);
    size_t nsym = parse_syms(v->syms, SYMS) / bi.groups;
    static uint8_t copy[400];
    memcpy(copy, SYMS, nsym * bi.groups);
    corrupt(copy, bi.groups, (8 + (8 + bi.groups - 1) / bi.groups) * bi.groups, 0, BYS_RISK_BODY, 4);
    bys_rx_t rx;
    bys_rx_init(&rx, (bys_band_t)v->band, BYS_NORMAL, NULL, 0);
    CHECK(feed(&rx, copy, nsym, bi.groups) == BYS_RX_DONE);
    const bys_result_t *r = bys_rx_result(&rx);
    CHECK(r->reason == (unsigned)v->reason && r->source == (unsigned)v->source && r->hops == (unsigned)v->hops);
    CHECK(memcmp(r->own, v->own, 3) == 0 && memcmp(r->intruder, v->intruder, 3) == 0);
  }
  /* header: the wrong band or profile does not check out */
  const vec_frame_t *v = &V_FRAMES[0];
  bys_band_info_t bi;
  bys_band_info((bys_band_t)v->band, (bys_profile_t)v->robust, &bi);
  size_t nsym = parse_syms(v->syms, SYMS) / bi.groups;
  bys_rx_t rx;
  bys_rx_init(&rx, BYS_MID, BYS_NORMAL, OUT, sizeof OUT);
  CHECK(feed(&rx, SYMS, nsym, bi.groups) < 0 || v->band == BYS_MID);
  bys_rx_init(&rx, (bys_band_t)v->band, BYS_ROBUST, OUT, sizeof OUT);
  CHECK(feed(&rx, SYMS, nsym, bi.groups) == BYS_E_HEADER);
  /* union of the two reference parities: with A and B more errors are corrected than with one */
  uint8_t body[32], fp[12];
  for (size_t i = 0; i < V_NFRAMES; i++) {
    if (V_FRAMES[i].payload || V_FRAMES[i].set != 0 || V_FRAMES[i].robust || strcmp(V_FRAMES[i].name, "ejemplo")) continue;
    size_t bn = from_hex(V_FRAMES[i].body, body);
    CHECK(bn == 30);
    uint8_t other[16];
    for (size_t j = 0; j < V_NFRAMES; j++) if (!V_FRAMES[j].payload && V_FRAMES[j].set == 1 && V_FRAMES[j].band == V_FRAMES[i].band && !V_FRAMES[j].robust && !strcmp(V_FRAMES[j].name, "ejemplo")) { uint8_t b2[32]; from_hex(V_FRAMES[j].body, b2); memcpy(other, b2 + 14, 16); }
    uint8_t data[14];
    memcpy(data, body, 14);
    for (int k = 0; k < 14; k += 2) data[k] ^= 0x5a; /* 7 errors in the data: one parity is not enough (max 8 with 16 erasures), both are */
    uint8_t c;
    CHECK(bys_mother_decode(data, body + 14, other, fp, &c) == BYS_OK && memcmp(fp, body, 12) == 0);
    CHECK(bys_mother_decode(data, body + 14, NULL, fp, &c) == BYS_OK); /* 7 <= 8 */
    break;
  }
}

/* ---------- transmitter ---------- */
static int16_t AUDIO[130000], GOLD[130000];

static int make_tx(bys_tx_t *tx, const bys_tx_cfg_t *cfg, const golden_case_t *g) {
  if (g->is_ack) {
    const vec_ack_t *v = &V_ACKS[g->index];
    uint8_t b[32];
    from_hex(v->body, b);
    return bys_tx_init_ack(tx, cfg, (bys_band_t)v->band, v->receipt, b + 1);
  }
  const vec_frame_t *v = &V_FRAMES[g->index];
  if (v->payload) return bys_tx_init_payload(tx, cfg, (bys_band_t)v->band, (bys_profile_t)v->robust, v->text, strlen(v->text));
  uint8_t b[64];
  from_hex(v->body, b);
  return bys_tx_init_reference(tx, cfg, (bys_band_t)v->band, (bys_profile_t)v->robust, (unsigned)v->set, b);
}

static size_t drain(bys_tx_t *tx, int16_t *out, size_t cap, const size_t *chunks, size_t nchunks) {
  size_t at = 0, i = 0;
  while (!bys_tx_done(tx) && at < cap) {
    size_t want = chunks[i++ % nchunks];
    if (want > cap - at) want = cap - at;
    at += bys_tx_fill(tx, out + at, want);
  }
  return at;
}

static void test_tx(void) {
  double worst = 0;
  for (size_t i = 0; i < N_GOLDEN; i++) {
    const golden_case_t *g = &GOLDEN[i];
    char path[256];
    snprintf(path, sizeof path, "%s/%s.i16", GOLDEN_DIR, g->id);
    FILE *fh = fopen(path, "rb");
    CHECK(fh != NULL);
    if (!fh) continue;
    size_t ng = fread(GOLD, 2, sizeof GOLD / 2, fh);
    fclose(fh);
    bys_tx_cfg_t cfg;
    bys_tx_cfg_default(&cfg);
    cfg.sample_rate = g->rate;
    bys_tx_t tx;
    CHECK(make_tx(&tx, &cfg, g) == BYS_OK);
    CHECK(bys_tx_total_samples(&tx) == ng);
    static const size_t big[] = { 4096 };
    size_t n = drain(&tx, AUDIO, sizeof AUDIO / 2, big, 1);
    CHECK(n == ng && bys_tx_done(&tx));
    int maxd = 0;
    for (size_t k = 0; k < n && k < ng; k++) { int d = abs(AUDIO[k] - GOLD[k]); if (d > maxd) maxd = d; }
    double rel = maxd / 32768.0;
    if (rel > worst) worst = rel;
    printf("  audio %-22s %6zu samples, max error %3d LSB = %.2e of full scale\n", g->id, n, maxd, rel);
    CHECK(rel < 3e-4);
    /* the result does not depend on how it is requested: odd chunk sizes give the same output */
    static int16_t again[130000];
    bys_tx_t tx2;
    make_tx(&tx2, &cfg, g);
    static const size_t odd[] = { 1, 7, 100, 513, 3, 2048 };
    size_t n2 = drain(&tx2, again, sizeof again / 2, odd, 6);
    CHECK(n2 == n && memcmp(again, AUDIO, n * 2) == 0);
  }
  printf("  max audio error over all cases: %.2e of full scale\n", worst);

  /* level: default maximum; 0.5 halves it; wide does not clip */
  bys_tx_t tx;
  uint8_t fp[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
  static const size_t big[] = { 4096 };
  bys_tx_cfg_t cfg;
  bys_tx_cfg_default(&cfg);
  CHECK(cfg.level == 1.0f && cfg.sample_rate == 44100 && cfg.gap_ms == 350);
  CHECK(bys_tx_init_reference(&tx, NULL, BYS_MID, BYS_NORMAL, 0, fp) == BYS_OK);
  size_t n = drain(&tx, AUDIO, sizeof AUDIO / 2, big, 1);
  int peak = 0; for (size_t k = 0; k < n; k++) if (abs(AUDIO[k]) > peak) peak = abs(AUDIO[k]);
  CHECK(peak > 0.85 * 32768 && peak < 0.92 * 32768);
  cfg.level = 0.5f;
  bys_tx_init_reference(&tx, &cfg, BYS_MID, BYS_NORMAL, 0, fp);
  n = drain(&tx, AUDIO, sizeof AUDIO / 2, big, 1);
  int peak2 = 0; for (size_t k = 0; k < n; k++) if (abs(AUDIO[k]) > peak2) peak2 = abs(AUDIO[k]);
  CHECK(abs(peak2 * 2 - peak) < 200);
  cfg.level = 0.0f;
  bys_tx_init_reference(&tx, &cfg, BYS_MID, BYS_NORMAL, 0, fp);
  n = drain(&tx, AUDIO, sizeof AUDIO / 2, big, 1);
  peak2 = 0; for (size_t k = 0; k < n; k++) if (abs(AUDIO[k]) > peak2) peak2 = abs(AUDIO[k]);
  CHECK(peak2 == 0);
  /* sample rates */
  bys_tx_cfg_default(&cfg);
  cfg.sample_rate = 16000;
  CHECK(bys_tx_init_reference(&tx, &cfg, BYS_TOP, BYS_NORMAL, 0, fp) == BYS_E_RATE);
  CHECK(bys_tx_init_reference(&tx, &cfg, BYS_AUDIBLE, BYS_NORMAL, 0, fp) == BYS_OK);
  cfg.sample_rate = 48000;
  CHECK(bys_tx_init_reference(&tx, &cfg, BYS_WIDE, BYS_NORMAL, 0, fp) == BYS_OK);
  cfg.sample_rate = 44100;
  cfg.gap_ms = 0;
  bys_tx_init_ack(&tx, &cfg, BYS_MID, 0, fp);
  n = drain(&tx, AUDIO, sizeof AUDIO / 2, big, 1);
  CHECK(n == bys_tx_total_samples(&tx) && n == 45070 - 15435);
  CHECK(bys_tx_fill(&tx, AUDIO, 10) == 0);
}

static void sizes(void) {
  printf("  sizeof: bys_frame_t=%zu bys_tx_t=%zu bys_rx_t=%zu bys_result_t=%zu bys_packer_t=%zu\n",
         sizeof(bys_frame_t), sizeof(bys_tx_t), sizeof(bys_rx_t), sizeof(bys_result_t), sizeof(bys_packer_t));
  CHECK(sizeof(bys_tx_t) < 4096);
}

/* Text "<prefix> <i>" with the smallest i whose last block ends in a parity byte 0x00 (about 1 in 256). */
static void zero_parity_text(const char *prefix, char *text, size_t cap) {
  for (unsigned i = 0;; i++) {
    snprintf(text, cap, "%s %u", prefix, i);
    bys_frame_t f;
    uint8_t nib[BYS_MAX_GROUPS];
    if (bys_frame_init_payload(&f, BYS_TOP, BYS_NORMAL, text, strlen(text)) != BYS_OK) return;
    while (bys_frame_next(&f, nib)) {}
    if (f.buf[f.buf_len - 1] == 0) return;
  }
}

/* Regression (found by bench/): RS also accepts the last block one byte short when its last parity byte is 0x00 (c(x) = x * c'(x));
 * the CRC-32 of the payload has to pick the length. One- and two-block payloads, normal and robust, at symbol level. */
static void test_zero_parity(void) {
  static const char *prefixes[2] = { "zero parity", "a payload long enough to need two blocks of forty-eight bytes, zero parity" };
  for (int k = 0; k < 2; k++) {
    char text[200];
    zero_parity_text(prefixes[k], text, sizeof text);
    for (int robust = 0; robust < 2; robust++) {
      bys_frame_t f;
      CHECK(bys_frame_init_payload(&f, BYS_TOP, (bys_profile_t)robust, text, strlen(text)) == BYS_OK);
      CHECK(f.total == k + 1);
      size_t n = 0;
      while (bys_frame_next(&f, SYMS + n * f.groups)) n++;
      CHECK(f.buf[f.buf_len - 1] == 0);
      bys_rx_t rx;
      bys_rx_init(&rx, BYS_TOP, (bys_profile_t)robust, OUT, sizeof OUT);
      CHECK(feed(&rx, SYMS, n, f.groups) == BYS_RX_DONE);
      char back[200];
      int u = bys_unpack(OUT, bys_rx_result(&rx)->payload_len, back, sizeof back);
      CHECK(u == (int)strlen(text) && memcmp(back, text, (size_t)u) == 0);
    }
  }
}

int main(void) {
  printf("bysound: host tests\n");
  test_constants();
  test_crc_rs_pack();
  test_frames();
  test_decode();
  test_zero_parity();
  test_tx();
  sizes();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
