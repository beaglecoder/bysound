/*
 * Session layer tests (docs/SESSION.md): vectors (acks with the heard nibble, risk, security levels), the state machines driven by
 * synthetic demodulator events, and multi-device scenarios on simulated air: audio rendered with bys_tx, mixed per pair with a gain,
 * and fed to real bys_demod instances. `make test` runs it (ASan + UBSan, FFT engine).
 */
#include "bysound.h"
#include "bysound_session.h"
#include "json_constants.h"
#include "session_vectors.h"
#include "vectors.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int hexval(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
static size_t from_hex(const char *h, uint8_t *out) {
  size_t n = strlen(h) / 2;
  for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(hexval(h[2 * i]) * 16 + hexval(h[2 * i + 1]));
  return n;
}

/* ======================================================================================================================== */
/* Vectors                                                                                                                  */
/* ======================================================================================================================== */
static uint8_t SYMS[40000];
static bys_tx_cfg_t TXC; /* 48 kHz */
static void txc_init(void) { bys_tx_cfg_default(&TXC); TXC.sample_rate = 48000; }
static size_t parse_syms(const char *s, uint8_t *out) { size_t n = strlen(s); for (size_t i = 0; i < n; i++) out[i] = (uint8_t)hexval(s[i]); return n; }

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
  uint8_t h[4], b[64];
  CHECK(from_hex(header, h) == 4);
  size_t bn = from_hex(body, b);
  size_t at = 8 * f->groups, hsyms = (8 + f->groups - 1) / f->groups, bat = (8 + hsyms) * f->groups;
  int same = 1;
  for (size_t i = 0; i < 4; i++) if ((SYMS[at + 2 * i] << 4 | SYMS[at + 2 * i + 1]) != h[i]) same = 0;
  for (size_t i = 0; i < bn; i++) if ((SYMS[bat + 2 * i] << 4 | SYMS[bat + 2 * i + 1]) != b[i]) same = 0;
  CHECK(same);
}

static int feed_rx(bys_rx_t *rx, const uint8_t *syms, size_t nsym, unsigned groups) {
  int r = BYS_RX_MORE;
  for (size_t k = BYS_PREAMBLE_SYMBOLS; k < nsym && r == BYS_RX_MORE; k++) r = bys_rx_push(rx, syms + k * groups);
  return r == BYS_RX_MORE ? bys_rx_finish(rx) : r;
}

static void test_vectors(void) {
  printf("1. vectors: session acks (heard nibble), plain acks, risk, ids, security levels\n");
  /* session-acks.json */
  for (size_t i = 0; i < V_NSACKS; i++) {
    const vec_sack_t *v = &V_SACKS[i];
    uint8_t data[8], hash[32];
    from_hex(v->data, data);
    bys_sha256((const uint8_t *)v->text, strlen(v->text), hash);
    CHECK(!memcmp(data + 1, hash, 4)); /* ack id = first 4 bytes of SHA-256(text) */
    const unsigned nib = v->heard_band < 0 ? 0u : bys_heard_nibble((bys_band_t)v->heard_band, (bys_profile_t)v->heard_robust);
    CHECK(data[0] == (uint8_t)((nib << 4) | (v->receipt ? 3 : 2)));
    bys_frame_t f;
    CHECK(bys_frame_init_ack_heard(&f, (bys_band_t)v->band, v->receipt, nib, hash) == BYS_OK);
    check_frame(&f, v->header, v->body, v->syms);
    /* decode back */
    bys_band_info_t bi;
    bys_band_info((bys_band_t)v->band, BYS_NORMAL, &bi);
    size_t nsym = parse_syms(v->syms, SYMS) / bi.groups;
    bys_rx_t rx;
    bys_rx_init(&rx, (bys_band_t)v->band, BYS_NORMAL, NULL, 0);
    CHECK(feed_rx(&rx, SYMS, nsym, bi.groups) == BYS_RX_DONE);
    const bys_result_t *r = bys_rx_result(&rx);
    CHECK(r->kind == BYS_KIND_ACK && r->receipt == (unsigned)v->receipt && r->heard == nib && !memcmp(r->hash4, hash, 4));
    bys_band_t hb = BYS_TOP;
    bys_profile_t hp = BYS_NORMAL;
    const int st = bys_heard_parse(r->heard, &hb, &hp);
    CHECK(v->heard_band < 0 ? st == 0 : (st == 1 && (int)hb == v->heard_band && (int)hp == v->heard_robust));
  }
  /* v4-acks.json: plain acks still decode with heard 0, and a session decoder accepts both */
  for (size_t i = 0; i < V_NACKS; i++) {
    const vec_ack_t *v = &V_ACKS[i];
    bys_band_info_t bi;
    bys_band_info((bys_band_t)v->band, BYS_NORMAL, &bi);
    size_t nsym = parse_syms(v->syms, SYMS) / bi.groups;
    bys_rx_t rx;
    bys_rx_init(&rx, (bys_band_t)v->band, BYS_NORMAL, NULL, 0);
    CHECK(feed_rx(&rx, SYMS, nsym, bi.groups) == BYS_RX_DONE && bys_rx_result(&rx)->heard == 0);
  }
  /* invalid heard nibbles */
  CHECK(bys_heard_parse(8, NULL, NULL) == -1 && bys_heard_parse(0, NULL, NULL) == 0 && bys_heard_parse(15, NULL, NULL) == 1);
  { bys_frame_t f; const uint8_t h4[4] = { 1, 2, 3, 4 }; CHECK(bys_frame_init_ack_heard(&f, BYS_MID, 0, 8, h4) == BYS_E_ARG); CHECK(bys_frame_init_ack_heard(&f, BYS_MID, 0, 16, h4) == BYS_E_ARG); }
  /* bys_frame_duration_us against SESSION.md section 10 */
  { uint32_t us;
    CHECK(bys_frame_duration_us(BYS_KIND_ACK, BYS_TOP, BYS_NORMAL, 0, &us) == BYS_OK && us == 864000);
    CHECK(bys_frame_duration_us(BYS_KIND_ACK, BYS_MID, BYS_NORMAL, 0, &us) == BYS_OK && us == 672000);
    CHECK(bys_frame_duration_us(BYS_KIND_RISK, BYS_MID, BYS_NORMAL, 0, &us) == BYS_OK && us == 736000);
    CHECK(bys_frame_duration_us(BYS_KIND_RISK, BYS_TOP, BYS_NORMAL, 0, &us) == BYS_OK && us == 960000);
    CHECK(bys_frame_duration_us(BYS_KIND_REFERENCE, BYS_TOP, BYS_NORMAL, 0, &us) == BYS_OK && us == 1344000);
    CHECK(bys_frame_duration_us(BYS_KIND_REFERENCE, BYS_XHIGH, BYS_NORMAL, 0, &us) == BYS_OK && us == 800000);
    CHECK(bys_frame_duration_us(BYS_KIND_ACK, BYS_WIDE, BYS_NORMAL, 0, &us) == BYS_OK && us == 480000);
    { uint8_t pk[64]; const int pn = bys_pack("pay 12.50 to shop 7", 19, pk, sizeof pk); /* digit pairs pack 2:1 */
      CHECK(bys_frame_duration_us(BYS_KIND_PAYLOAD, BYS_TOP, BYS_NORMAL, (uint32_t)pn, &us) == BYS_OK && us == 1504000); /* SESSION.md section 10 */
      CHECK(bys_frame_duration_us(BYS_KIND_PAYLOAD, BYS_HIGH, BYS_NORMAL, (uint32_t)pn, &us) == BYS_OK && us == 1120000); }
    bys_frame_t f;
    const char *t = "pay 12.50 to shop 7";
    bys_frame_init_payload(&f, BYS_TOP, BYS_ROBUST, t, strlen(t));
    bys_band_info_t bi; bys_band_info(BYS_TOP, BYS_ROBUST, &bi);
    uint8_t pk[64]; const int pn = bys_pack(t, strlen(t), pk, sizeof pk);
    CHECK(bys_frame_duration_us(BYS_KIND_PAYLOAD, BYS_TOP, BYS_ROBUST, (uint32_t)pn, &us) == BYS_OK && us == (uint32_t)bys_frame_symbols(&f) * bi.symbol_us);
  }
  /* SHA-256 */
  { uint8_t h[32]; bys_sha256((const uint8_t *)"abc", 3, h); CHECK(h[0] == 0xba && h[1] == 0x78 && h[2] == 0x16 && h[3] == 0xbf && h[31] == 0xad);
    uint8_t big[1000]; memset(big, 'a', sizeof big); bys_sha256(big, 1000, h); CHECK(h[0] == 0x41 && h[1] == 0xed && h[2] == 0xec); /* 1000 x 'a' */
    bys_ids_t ids; bys_text_ids("abc", 3, &ids); CHECK(ids.ack[0] == 0xba && ids.short_id[2] == 0x16 && ids.fingerprint[11] == 0xde); }
  /* v4-risk.json frames through the session descriptor */
  for (size_t i = 0; i < V_NRISKS; i++) {
    const vec_risk_t *v = &V_RISKS[i];
    bys_session_frame_t sf;
    memset(&sf, 0, sizeof sf);
    sf.kind = BYS_KIND_RISK; sf.band = (uint8_t)v->band; sf.reason = (uint8_t)v->reason; sf.source = (uint8_t)v->source; sf.hops = (uint8_t)v->hops; sf.gap_ms = BYS_CONTROL_GAP_MS;
    memcpy(sf.own, v->own, 3); memcpy(sf.intruder, v->intruder, 3);
    bys_tx_t a, b;
    CHECK(bys_session_tx_init(&a, &TXC, &sf) == BYS_OK && bys_tx_init_risk(&b, &TXC, (bys_band_t)v->band, (unsigned)v->reason, (unsigned)v->source, (unsigned)v->hops, v->own, v->intruder) == BYS_OK);
    CHECK(bys_tx_total_samples(&a) == bys_tx_total_samples(&b) - 48000u * BYS_GAP_MS / 1000u + 48000u * BYS_CONTROL_GAP_MS / 1000u);
  }
  /* security levels equal the JSON table */
  static const bys_security_level_t LV[3] = { BYS_SEC_BASIC, BYS_SEC_STANDARD, BYS_SEC_MAXIMUM };
  for (int i = 0; i < 3; i++) {
    bys_security_t s;
    bys_security_resolve(&s, LV[i]);
    const vec_sec_t *v = &V_SEC[i];
    CHECK(s.detect_multiple_emitters == v->detect && s.listen_while_sending == v->listen_while_sending && s.confirm_watch == v->confirm_watch);
    CHECK(s.risk.emit_auto == v->emit_auto && s.risk.listen == v->listen && s.risk.honor == v->honor && s.risk.listen_until_confirm == v->until_confirm);
    CHECK(s.listen_before_ms == v->listen_before_ms && s.scan_jitter_ms == v->jitter_ms && s.confirm_watch_ms == v->confirm_watch_ms);
    CHECK(s.risk.duration_ms == v->duration_ms && s.risk.detector_duration_ms == v->detector_ms && s.risk.quarantine_ms == v->quarantine_ms);
  }
  { bys_sender_cfg_t c; bys_sender_cfg_default(&c, "x", 1); CHECK(c.security.listen_before_ms == 3000 && c.security.risk.quarantine_ms == 60000); /* maximum is the default */ }
  /* constants */
  CHECK(BYS_RISK_MAX_HOPS == 2 && BYS_CONTROL_GAP_MS == 150 && BYS_LISTEN_WINDOW_MS == 2000);
  /* risk bands (SESSION.md 3.2) */
  { bys_band_t b[2]; unsigned n;
    n = bys_risk_bands(BYS_TOP, -1, b); CHECK(n == 2 && b[0] == BYS_MID && b[1] == BYS_TOP);
    n = bys_risk_bands(BYS_MID, -1, b); CHECK(n == 1 && b[0] == BYS_MID);
    n = bys_risk_bands(BYS_AUDIBLE, -1, b); CHECK(n == 1 && b[0] == BYS_MID);
    n = bys_risk_bands(BYS_TOP, BYS_MID, b); CHECK(n == 1 && b[0] == BYS_TOP);
    n = bys_risk_bands(BYS_TOP, BYS_TOP, b); CHECK(n == 1 && b[0] == BYS_MID);
    n = bys_risk_bands(BYS_TOP, BYS_AUDIBLE, b); CHECK(n == 2);
    n = bys_risk_bands(BYS_UPPER, BYS_MID, b); CHECK(n == 1 && b[0] == BYS_TOP); /* neither mid nor upper is clear of mid: the spare list gives top */
    n = bys_risk_bands(BYS_HIGH, BYS_UPPER, b); CHECK(n == 2); /* nothing is clear of upper, nor are top/mid: the base list stays */
    n = bys_risk_bands(BYS_HIGH, BYS_XHIGH, b); CHECK(n == 2 || n == 1);
    CHECK(!bys_bands_overlap(BYS_MID, BYS_HIGH) && !bys_bands_overlap(BYS_MID, BYS_TOP) && bys_bands_overlap(BYS_TOP, BYS_HIGH) && bys_bands_overlap(BYS_UPPER, BYS_MID)); }
}

/* ======================================================================================================================== */
/* Mock device: logs what a session does                                                                                    */
/* ======================================================================================================================== */
#define NPLAY 400
typedef struct dev tdev_t;
struct dev {
  const char *name;
  /* what the sessions did */
  int nplay, nstop; bys_session_frame_t plays[NPLAY];
  int nrep; struct { uint32_t i; int band, profile; } reps[64];
  int nfail; bys_session_error_t err; int fail_has_alert; bys_alert_t fail_alert;
  int ndeliv; bys_delivered_t dl;
  int nrecv; char rtext[512]; int rband, rvia;
  int nreceipt, receipt;
  int nalert; bys_alert_t alerts[16];
  int nquar; uint32_t qms; int qreason, qsource;
  int nfin, nstate, last_state;
  int namb; bys_ambient_t amb; uint8_t route[8]; unsigned nroute;
  int have_cal, nsave, nclear; bys_calibration_t cal;
  uint32_t rnd;
  int play_rc;
  void (*on_play)(tdev_t *, const bys_session_frame_t *);
  void (*on_stop)(tdev_t *);
  bys_session_io io;
  bys_sender_t snd; bys_receiver_t rcv; int has_snd, has_rcv;
  char rbuf[600], scratch[600];
  bys_risk_memory_t mem;
};

static int m_play(void *u, const bys_session_frame_t *f) {
  tdev_t *d = u;
  if (d->nplay < NPLAY) d->plays[d->nplay] = *f;
  d->nplay++;
  if (d->play_rc >= 0 && d->on_play) d->on_play(d, f);
  return d->play_rc;
}
static void m_stop(void *u) { tdev_t *d = u; d->nstop++; if (d->on_stop) d->on_stop(d); }
static uint32_t m_random(void *u) { return ((tdev_t *)u)->rnd; }
static int m_cal_load(void *u, bys_calibration_t *c) { tdev_t *d = u; if (!d->have_cal) return 0; *c = d->cal; return 1; }
static void m_cal_save(void *u, const bys_calibration_t *c) { tdev_t *d = u; d->nsave++; if (c) { d->cal = *c; d->have_cal = 1; } else { d->have_cal = 0; d->nclear++; } }
static void m_alert(void *u, const bys_alert_t *a) { tdev_t *d = u; if (d->nalert < 16) d->alerts[d->nalert] = *a; d->nalert++; }
static void m_quar(void *u, uint32_t ms, uint8_t r, uint8_t s) { tdev_t *d = u; d->nquar++; d->qms = ms; d->qreason = r; d->qsource = s; }
static void m_state(void *u, int s) { tdev_t *d = u; d->nstate++; d->last_state = s; }
static void m_rep(void *u, uint32_t i, bys_band_t b, bys_profile_t p) { tdev_t *d = u; if (d->nrep < 64) { d->reps[d->nrep].i = i; d->reps[d->nrep].band = b; d->reps[d->nrep].profile = p; } d->nrep++; }
static void m_amb(void *u, const bys_ambient_t *a, const uint8_t *route, unsigned n) { tdev_t *d = u; d->namb++; d->amb = *a; memcpy(d->route, route, n); d->nroute = n; }
static void m_deliv(void *u, const bys_delivered_t *x) { tdev_t *d = u; d->ndeliv++; d->dl = *x; }
static void m_recv(void *u, const char *t, size_t n, bys_band_t b, int via) { tdev_t *d = u; d->nrecv++; memcpy(d->rtext, t, n + 1); d->rband = b; d->rvia = via; }
static void m_receipt(void *u, int h) { tdev_t *d = u; d->nreceipt++; d->receipt = h; }
static void m_failed(void *u, bys_session_error_t e, const bys_alert_t *a) { tdev_t *d = u; d->nfail++; d->err = e; d->fail_has_alert = a != NULL; if (a) d->fail_alert = *a; }
static void m_fin(void *u) { ((tdev_t *)u)->nfin++; }

static void dev_init(tdev_t *d, const char *name) {
  memset(d, 0, sizeof *d);
  d->name = name;
  d->io = (bys_session_io){ .user = d, .play = m_play, .stop = m_stop, .random = m_random, .cal_load = m_cal_load, .cal_save = m_cal_save, .on_alert = m_alert, .on_quarantine = m_quar,
    .on_state = m_state, .on_repetition = m_rep, .on_ambient = m_amb, .on_delivered = m_deliv, .on_received = m_recv, .on_receipt = m_receipt, .on_failed = m_failed, .on_finished = m_fin };
  bys_risk_memory_init(&d->mem);
}
static void dev_clear_log(tdev_t *d) {
  tdev_t keep = *d; /* keep calibration and memory */
  dev_init(d, keep.name);
  d->have_cal = keep.have_cal; d->cal = keep.cal; d->mem = keep.mem; d->on_play = keep.on_play; d->on_stop = keep.on_stop;
}

static bys_sender_cfg_t scfg(tdev_t *d, const char *text, bys_security_level_t lv) {
  bys_sender_cfg_t c;
  bys_sender_cfg_default(&c, text, strlen(text));
  bys_security_resolve(&c.security, lv);
  c.memory = &d->mem; c.scratch = d->scratch; c.scratch_cap = sizeof d->scratch;
  return c;
}
static bys_receiver_cfg_t rcfg(tdev_t *d, bys_security_level_t lv) {
  bys_receiver_cfg_t c;
  bys_receiver_cfg_default(&c, d->rbuf, sizeof d->rbuf);
  bys_security_resolve(&c.security, lv);
  c.memory = &d->mem; c.scratch = d->scratch; c.scratch_cap = sizeof d->scratch;
  return c;
}

/* ---- synthetic demodulator events ---- */
typedef struct { bys_result_t res; bys_demod_event_t ev; uint8_t packed[600]; } evbuf_t;
static const bys_demod_event_t *ev_payload(evbuf_t *e, const char *text, int band, int profile) {
  memset(e, 0, sizeof *e);
  int n = bys_pack(text, strlen(text), e->packed, sizeof e->packed);
  e->res.kind = BYS_KIND_PAYLOAD; e->res.band = (uint8_t)band; e->res.payload_len = (uint16_t)n;
  e->ev = (bys_demod_event_t){ .type = BYS_EV_FRAME, .band = (uint8_t)band, .profile = (uint8_t)profile, .result = &e->res, .payload = e->packed, .payload_len = (size_t)n };
  return &e->ev;
}
static const bys_demod_event_t *ev_ref(evbuf_t *e, const char *text, int band) {
  memset(e, 0, sizeof *e);
  bys_ids_t ids; bys_text_ids(text, strlen(text), &ids);
  e->res.kind = BYS_KIND_REFERENCE; e->res.band = (uint8_t)band; memcpy(e->res.fingerprint, ids.fingerprint, 12);
  e->ev = (bys_demod_event_t){ .type = BYS_EV_FRAME, .band = (uint8_t)band, .result = &e->res };
  return &e->ev;
}
static const bys_demod_event_t *ev_ack(evbuf_t *e, const char *text, int receipt, unsigned heard, int band) {
  memset(e, 0, sizeof *e);
  bys_ids_t ids; bys_text_ids(text, strlen(text), &ids);
  e->res.kind = BYS_KIND_ACK; e->res.receipt = (uint8_t)receipt; e->res.heard = (uint8_t)heard; memcpy(e->res.hash4, ids.ack, 4);
  e->ev = (bys_demod_event_t){ .type = BYS_EV_FRAME, .band = (uint8_t)band, .result = &e->res };
  return &e->ev;
}
static const bys_demod_event_t *ev_risk(evbuf_t *e, unsigned reason, unsigned source, unsigned hops, const uint8_t own[3], const uint8_t intr[3], int band) {
  memset(e, 0, sizeof *e);
  e->res.kind = BYS_KIND_RISK; e->res.reason = (uint8_t)reason; e->res.source = (uint8_t)source; e->res.hops = (uint8_t)hops;
  memcpy(e->res.own, own, 3); memcpy(e->res.intruder, intr, 3);
  e->ev = (bys_demod_event_t){ .type = BYS_EV_FRAME, .band = (uint8_t)band, .result = &e->res };
  return &e->ev;
}
static const bys_demod_event_t *ev_simple(evbuf_t *e, int type, int band) {
  memset(e, 0, sizeof *e);
  e->ev = (bys_demod_event_t){ .type = type, .band = (uint8_t)band };
  return &e->ev;
}
static void short_of(const char *t, uint8_t out[3]) { bys_ids_t ids; bys_text_ids(t, strlen(t), &ids); memcpy(out, ids.short_id, 3); }

#define TEXT "pay 12.50 to shop 7"
#define OTHER "pay 99.00 to shop 3"

/* Plays frames that each take `frame_ms` (frame + gap) until the loop of the device ends. Returns elapsed ms. */
static uint32_t spin_sender(tdev_t *d, uint32_t *now, uint32_t frame_ms, int max) {
  uint32_t t0 = *now;
  for (int i = 0; i < max && d->snd.core.playing; i++) { *now += frame_ms; bys_sender_play_done(&d->snd, *now); bys_sender_tick(&d->snd, *now); }
  return *now - t0;
}
static uint32_t spin_receiver(tdev_t *d, uint32_t *now, uint32_t frame_ms, int max) {
  uint32_t t0 = *now;
  for (int i = 0; i < max && d->rcv.core.playing; i++) { *now += frame_ms; bys_receiver_play_done(&d->rcv, *now); bys_receiver_tick(&d->rcv, *now); }
  return *now - t0;
}

/* ======================================================================================================================== */
/* Sender, synthetic events                                                                                                 */
/* ======================================================================================================================== */
static tdev_t A, B;
static evbuf_t E;

static void test_sender_ack(void) {
  printf("2. sender: pre-listen, repetitions and windows, ack, receipts, calibration saved\n");
  dev_init(&A, "A");
  bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD);
  uint32_t now = 1000;
  A.rnd = 0;
  CHECK(bys_sender_start(&A.snd, &c, &A.io, now) == BYS_OK);
  CHECK(bys_sender_state(&A.snd) == BYS_SS_PRE_LISTEN && A.nplay == 0);
  bys_sender_tick(&A.snd, now + 1999);
  CHECK(A.nplay == 0);
  now += 2000; bys_sender_tick(&A.snd, now);
  CHECK(A.nplay == 1 && A.plays[0].kind == BYS_KIND_PAYLOAD && A.plays[0].band == BYS_TOP && A.plays[0].profile == BYS_NORMAL && A.plays[0].gap_ms == BYS_GAP_MS && A.plays[0].text == c.text);
  CHECK(bys_sender_state(&A.snd) == BYS_SS_SENDING && A.nrep == 1 && A.reps[0].band == BYS_TOP);
  now += 1850; bys_sender_play_done(&A.snd, now);
  CHECK(bys_sender_state(&A.snd) == BYS_SS_WINDOW);
  bys_sender_tick(&A.snd, now + 1999); CHECK(A.nplay == 1);
  bys_sender_tick(&A.snd, now + 2000); CHECK(A.nplay == 2); /* next repetition after the 2 s window */
  now += 2000 + 500;
  /* acks that must be ignored: another text, a receipt, one while pre-... */
  bys_sender_hear(&A.snd, now, ev_ack(&E, OTHER, 0, 0, BYS_MID)); CHECK(A.ndeliv == 0);
  bys_sender_hear(&A.snd, now, ev_ack(&E, TEXT, 1, 0, BYS_MID)); CHECK(A.ndeliv == 0);
  /* the ack with our id, saying it was read on `high`, robust */
  const unsigned heard = bys_heard_nibble(BYS_HIGH, BYS_ROBUST);
  const int stops = A.nstop;
  bys_sender_hear(&A.snd, now, ev_ack(&E, TEXT, 0, heard, BYS_MID));
  CHECK(A.ndeliv == 1 && A.nstop == stops + 1 && A.dl.stated && A.dl.band == BYS_HIGH && A.dl.profile == BYS_ROBUST && A.dl.repetitions == 2 && A.dl.ms == 2000 + 1850 + 500 + 0);
  CHECK(bys_sender_state(&A.snd) == BYS_SS_RECEIPT);
  CHECK(A.dl.saved && A.dl.calibration.band == BYS_HIGH && A.dl.calibration.uses == 0 && A.have_cal);
  CHECK(A.nplay == 3 && A.plays[2].kind == BYS_KIND_ACK && A.plays[2].receipt == 1 && A.plays[2].band == BYS_HIGH && A.plays[2].heard == heard && A.plays[2].gap_ms == BYS_CONTROL_GAP_MS);
  bys_ids_t ids; bys_text_ids(TEXT, strlen(TEXT), &ids);
  CHECK(!memcmp(A.plays[2].hash4, ids.ack, 4));
  bys_sender_hear(&A.snd, now, ev_ack(&E, TEXT, 0, heard, BYS_MID)); CHECK(A.ndeliv == 1); /* a second ack is ignored */
  /* receipts for ackRun(L) + 3 s = 10.86 s: the 19-char text has L = 3.36 s (SESSION.md section 10), frames of 0.822 s */
  uint32_t t0 = now;
  uint32_t el = spin_sender(&A, &now, 822, 100);
  CHECK(el >= 10860 && el < 10860 + 822 && now - t0 == el);
  CHECK(bys_sender_state(&A.snd) == BYS_SS_DONE && A.nfin == 1 && A.nfail == 0);
  for (int i = 2; i < A.nplay && i < NPLAY; i++) CHECK(A.plays[i].kind == BYS_KIND_ACK && A.plays[i].receipt == 1 && A.plays[i].band == BYS_HIGH);
  CHECK(A.nplay == 2 + 14); /* 14 frames of 0.822 s cover 10.86 s: (14 - 1) * 822 = 10686 < 10860 <= 14 * 822 */
}

static void test_sender_timeout_stop(void) {
  dev_init(&A, "A");
  bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_BASIC);
  c.timeout_ms = 10000;
  uint32_t now = 0;
  bys_sender_start(&A.snd, &c, &A.io, now); /* basic: pre-listen 1 s for the ambient check */
  CHECK(bys_sender_state(&A.snd) == BYS_SS_PRE_LISTEN);
  bys_sender_tick(&A.snd, 1000);
  CHECK(A.nplay == 1);
  /* silent for the whole time: after 10 s from the first repetition, TIMEOUT */
  now = 1000;
  for (int i = 0; i < 20 && !A.nfail; i++) { now += 1850; bys_sender_play_done(&A.snd, now); now += 2000; bys_sender_tick(&A.snd, now); }
  CHECK(A.nfail == 1 && A.err == BYS_SERR_TIMEOUT && bys_sender_state(&A.snd) == BYS_SS_DONE && A.nfin == 1);
  /* stop() before the ack */
  dev_init(&A, "A");
  c = scfg(&A, TEXT, BYS_SEC_BASIC);
  c.ambient = 0; /* basic without ambient check: no pre-listen at all */
  bys_sender_start(&A.snd, &c, &A.io, 0);
  CHECK(A.nplay == 1 && bys_sender_state(&A.snd) == BYS_SS_SENDING);
  bys_sender_stop(&A.snd, 10);
  CHECK(A.nfail == 1 && A.err == BYS_SERR_STOPPED && A.nstop == 1 && bys_sender_state(&A.snd) == BYS_SS_DONE);
  /* arguments */
  bys_sender_t s;
  CHECK(bys_sender_start(&s, &c, &A.io, 0) == BYS_OK);
  c.text_len = 0; CHECK(bys_sender_start(&s, &c, &A.io, 0) == BYS_E_ARG);
  /* io.play failing ends the session */
  dev_init(&A, "A"); A.play_rc = -1;
  c = scfg(&A, TEXT, BYS_SEC_BASIC); c.ambient = 0;
  bys_sender_start(&A.snd, &c, &A.io, 0);
  CHECK(A.nfail == 1 && A.err == BYS_SERR_PLAY && bys_sender_state(&A.snd) == BYS_SS_DONE);
  /* bad band or profile */
  dev_init(&A, "A");
  c = scfg(&A, TEXT, BYS_SEC_BASIC); c.ambient = 0; c.band = -2; CHECK(bys_sender_start(&s, &c, &A.io, 0) == BYS_E_ARG);
  c.band = -1; c.profile = 2; CHECK(bys_sender_start(&s, &c, &A.io, 0) == BYS_E_ARG);
  /* a text too long for a band the session may play is refused before anything plays (reference mode sends only the fingerprint) */
  {
    static char longtxt[3000];
    const int maxn = bys_max_payload_bytes(BYS_TOP, BYS_NORMAL), maxr = bys_max_payload_bytes(BYS_TOP, BYS_ROBUST);
    memset(longtxt, 'a', sizeof longtxt);
    CHECK(maxn > maxr && maxn < (int)sizeof longtxt);
    dev_init(&A, "A");
    c = scfg(&A, TEXT, BYS_SEC_BASIC); c.ambient = 0; c.text = longtxt; c.text_len = (size_t)maxn + 1;
    CHECK(bys_sender_start(&s, &c, &A.io, 0) == BYS_E_TOO_LONG && A.nplay == 0);
    c.band = BYS_MID; CHECK(bys_sender_start(&s, &c, &A.io, 0) == BYS_OK);
    dev_init(&A, "A");
    c.band = -1; c.reference = 1; CHECK(bys_sender_start(&s, &c, &A.io, 0) == BYS_OK);
    dev_init(&A, "A");
    c.reference = 0; c.band = BYS_TOP; c.profile = BYS_ROBUST; c.text_len = (size_t)maxr + 1;
    CHECK(bys_sender_start(&s, &c, &A.io, 0) == BYS_E_TOO_LONG);
    /* one that fits normal but not robust keeps the normal profile when the ladder goes robust */
    dev_init(&A, "A");
    c.profile = -1; c.text_len = (size_t)(maxn + maxr) / 2; c.timeout_ms = 1000000;
    CHECK(bys_sender_start(&A.snd, &c, &A.io, 0) == BYS_OK);
    uint32_t t = 0;
    for (int i = 0; i < 9; i++) { t += 20000; bys_sender_play_done(&A.snd, t); t += 2000; bys_sender_tick(&A.snd, t); }
    int robust = 0;
    for (int i = 0; i < A.nrep && i < 64; i++) robust += A.reps[i].profile == BYS_ROBUST;
    CHECK(A.nrep >= 8 && robust == 0 && A.nfail == 0 && A.nplay == A.nrep);
    bys_sender_stop(&A.snd, t);
  }
}

static void test_sender_prelisten(void) {
  printf("3. sender: pre-listen refusals, second emitter while sending, clone\n");
  uint32_t now = 0;
  uint8_t sh[3];
  /* a decoded frame with other content: refuse at once, nothing sent, no risk frame */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, 500, ev_payload(&E, OTHER, BYS_TOP, 0));
  CHECK(A.nfail == 1 && A.err == BYS_SERR_OTHER_TRANSMISSION && !A.fail_has_alert && A.nplay == 0 && bys_sender_state(&A.snd) == BYS_SS_DONE);
  /* own content: CLONED */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, 500, ev_payload(&E, TEXT, BYS_HIGH, 1));
  CHECK(A.err == BYS_SERR_CLONED_TRANSMISSION && A.nplay == 0);
  { dev_init(&A, "A"); bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); bys_sender_start(&A.snd, &c, &A.io, now);
    bys_sender_hear(&A.snd, 500, ev_ref(&E, TEXT, BYS_HIGH)); CHECK(A.err == BYS_SERR_CLONED_TRANSMISSION);
    dev_init(&A, "A"); c = scfg(&A, TEXT, BYS_SEC_STANDARD); bys_sender_start(&A.snd, &c, &A.io, now);
    bys_sender_hear(&A.snd, 500, ev_ref(&E, OTHER, BYS_HIGH)); CHECK(A.err == BYS_SERR_OTHER_TRANSMISSION); }
  /* a header or a clash: wait until the pre-listen ends, then OTHER_TRANSMISSION */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, 500, ev_simple(&E, BYS_EV_HEADER, BYS_TOP));
  CHECK(A.nfail == 0);
  bys_sender_tick(&A.snd, 1999); CHECK(A.nfail == 0);
  bys_sender_tick(&A.snd, 2000); CHECK(A.nfail == 1 && A.err == BYS_SERR_OTHER_TRANSMISSION && A.nplay == 0);
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, 500, ev_simple(&E, BYS_EV_CLASH, 0)); bys_sender_tick(&A.snd, 2000);
  CHECK(A.err == BYS_SERR_OTHER_TRANSMISSION);
  /* header then the frame decoded with our own content before the end: reported as a clone */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, 500, ev_simple(&E, BYS_EV_HEADER, BYS_TOP)); bys_sender_hear(&A.snd, 900, ev_payload(&E, TEXT, BYS_TOP, 0));
  CHECK(A.err == BYS_SERR_CLONED_TRANSMISSION);
  /* acks and risk frames are not another transmission */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, 100, ev_ack(&E, OTHER, 0, 0, BYS_MID)); bys_sender_hear(&A.snd, 100, ev_ack(&E, TEXT, 1, 0, BYS_MID));
  { const uint8_t z[3] = { 0, 0, 0 }; const uint8_t o[3] = { 1, 2, 3 }; bys_sender_hear(&A.snd, 100, ev_risk(&E, 4, 0, 2, o, z, BYS_MID)); }
  bys_sender_hear(&A.snd, 100, ev_risk(&E, 1, 1, 2, (uint8_t[3]){ 9, 9, 9 }, (uint8_t[3]){ 1, 1, 1 }, BYS_MID)); /* honored: this ends it, hops 2: no relay */
  CHECK(A.err == BYS_SERR_RISK_SIGNAL);
  /* basic: the pre-listen (1 s, ambient check) acts on nothing it hears */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_BASIC); bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, 100, ev_payload(&E, OTHER, BYS_TOP, 0)); bys_sender_hear(&A.snd, 100, ev_simple(&E, BYS_EV_CLASH, 0));
  bys_sender_tick(&A.snd, 1000); CHECK(A.nfail == 0 && A.nplay == 1);
  /* jitter: standard 2000 + random % 600; maximum 3000 + random % 1200 */
  dev_init(&A, "A"); A.rnd = 599; { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); bys_sender_start(&A.snd, &c, &A.io, 0); }
  bys_sender_tick(&A.snd, 2598); CHECK(A.nplay == 0); bys_sender_tick(&A.snd, 2599); CHECK(A.nplay == 1);
  dev_init(&A, "A"); A.rnd = 1199; { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_MAXIMUM); bys_sender_start(&A.snd, &c, &A.io, 0); }
  bys_sender_tick(&A.snd, 4198); CHECK(A.nplay == 0); bys_sender_tick(&A.snd, 4199); CHECK(A.nplay == 1);
  /* no detection: no jitter */
  dev_init(&A, "A"); A.rnd = 599; { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_BASIC); bys_sender_start(&A.snd, &c, &A.io, 0); }
  bys_sender_tick(&A.snd, 1000); CHECK(A.nplay == 1);

  /* --- while sending --- */
  const uint8_t zero3[3] = { 0, 0, 0 };
  (void)zero3;
  /* another text in a listen window: OTHER_EMITTER, risk frame {other, sender, hops 0, own, intruder} avoiding the intruder's band */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.ambient = 0; c.band = BYS_TOP; bys_sender_start(&A.snd, &c, &A.io, 0); }
  bys_sender_tick(&A.snd, 2000); /* first repetition on top */
  bys_sender_play_done(&A.snd, 3850);
  bys_sender_hear(&A.snd, 4000, ev_simple(&E, BYS_EV_HEADER, BYS_UPPER)); bys_sender_hear(&A.snd, 4000, ev_simple(&E, BYS_EV_CLASH, 0)); /* ignored while sending */
  CHECK(A.nfail == 0 && bys_sender_state(&A.snd) == BYS_SS_WINDOW);
  const int plays = A.nplay;
  bys_sender_hear(&A.snd, 4500, ev_payload(&E, OTHER, BYS_UPPER, 0));
  short_of(TEXT, sh); uint8_t sho[3]; short_of(OTHER, sho);
  CHECK(A.nfail == 1 && A.err == BYS_SERR_OTHER_EMITTER && A.fail_has_alert && bys_sender_state(&A.snd) == BYS_SS_ALERTING);
  CHECK(A.nplay == plays + 1 && A.plays[plays].kind == BYS_KIND_RISK && A.plays[plays].reason == BYS_RISK_OTHER && A.plays[plays].source == BYS_ROLE_SENDER && A.plays[plays].hops == 0);
  CHECK(!memcmp(A.plays[plays].own, sh, 3) && !memcmp(A.plays[plays].intruder, sho, 3));
  CHECK(A.plays[plays].band == BYS_MID || A.plays[plays].band == BYS_TOP);
  CHECK(A.nalert == 1 && !A.alerts[0].heard && !A.alerts[0].relay && A.nquar == 1 && A.qms == 30000 && A.qreason == BYS_RISK_OTHER && A.qsource == BYS_ROLE_SENDER);
  { bys_band_t want[2]; unsigned nb = bys_risk_bands(BYS_TOP, BYS_UPPER, want); CHECK(A.alerts[0].nbands == nb && A.alerts[0].bands[0] == want[0]); }
  { uint32_t t = 4500; uint32_t el = spin_sender(&A, &t, 890, 200); CHECK(el >= 15000 && el < 15000 + 960 && bys_sender_state(&A.snd) == BYS_SS_DONE); } /* detectorDurationMs of standard */
  /* the same with emit off: only fails */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.ambient = 0; c.security.risk.emit_auto = 0; bys_sender_start(&A.snd, &c, &A.io, 0); }
  bys_sender_tick(&A.snd, 2000); bys_sender_play_done(&A.snd, 3850); bys_sender_hear(&A.snd, 4500, ev_payload(&E, OTHER, BYS_UPPER, 0));
  CHECK(A.err == BYS_SERR_OTHER_EMITTER && !A.fail_has_alert && A.nplay == 1 && bys_sender_state(&A.snd) == BYS_SS_DONE && A.nalert == 0);
  /* basic: listenWhileSending is off: hears nothing */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_BASIC); c.ambient = 0; bys_sender_start(&A.snd, &c, &A.io, 0); }
  bys_sender_play_done(&A.snd, 1850); bys_sender_hear(&A.snd, 2500, ev_payload(&E, OTHER, BYS_UPPER, 0));
  CHECK(A.nfail == 0 && bys_sender_state(&A.snd) == BYS_SS_WINDOW);
  /* clone: own data when silent >= 0.5 s after the last repetition */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.ambient = 0; bys_sender_start(&A.snd, &c, &A.io, 0); }
  bys_sender_tick(&A.snd, 2000); bys_sender_play_done(&A.snd, 3850);
  bys_sender_hear(&A.snd, 3850 + 499, ev_payload(&E, TEXT, BYS_TOP, 0)); CHECK(A.nfail == 0); /* its own frame, decoded inside the guard */
  bys_sender_hear(&A.snd, 3850 + 500, ev_payload(&E, TEXT, BYS_TOP, 0));
  CHECK(A.err == BYS_SERR_CLONED_TRANSMISSION && A.fail_has_alert && A.plays[A.nplay - 1].kind == BYS_KIND_RISK && A.plays[A.nplay - 1].reason == BYS_RISK_CLONE);
  CHECK(!memcmp(A.plays[A.nplay - 1].intruder, sh, 3));
  /* clone while playing: same band is our own frame; another band is someone else */
  dev_init(&A, "A"); { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.ambient = 0; bys_sender_start(&A.snd, &c, &A.io, 0); }
  bys_sender_tick(&A.snd, 2000);
  bys_sender_hear(&A.snd, 2500, ev_payload(&E, TEXT, BYS_TOP, 0)); CHECK(A.nfail == 0);
  bys_sender_hear(&A.snd, 2600, ev_payload(&E, TEXT, BYS_HIGH, 0)); CHECK(A.err == BYS_SERR_CLONED_TRANSMISSION);
}

static void test_sender_risk(void) {
  printf("4. sender: risk frames heard, relay hops, honor, memory, quarantine is a callback\n");
  const uint8_t own[3] = { 1, 2, 3 }, intr[3] = { 4, 5, 6 };
  /* honored: stop, relay with hops + 1 avoiding the band it was heard on, for detectorDurationMs */
  for (int hops = 0; hops <= 2; hops++) {
    dev_init(&A, "A");
    bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.ambient = 0; c.band = BYS_TOP;
    bys_sender_start(&A.snd, &c, &A.io, 0);
    bys_sender_tick(&A.snd, 2000); bys_sender_play_done(&A.snd, 3850);
    const int plays = A.nplay;
    bys_sender_hear(&A.snd, 4500, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_RECEIVER, (unsigned)hops, own, intr, BYS_MID));
    CHECK(A.nfail == 1 && A.err == BYS_SERR_RISK_SIGNAL && A.fail_has_alert && A.fail_alert.heard && A.fail_alert.hops == hops && !A.fail_alert.matches_mine);
    CHECK(A.nalert >= 1 && A.alerts[0].heard && A.nquar == 1);
    if (hops < 2) {
      CHECK(A.nplay == plays + 1 && A.plays[plays].kind == BYS_KIND_RISK && A.plays[plays].hops == hops + 1 && A.plays[plays].reason == BYS_RISK_OTHER && A.plays[plays].source == BYS_ROLE_RECEIVER);
      CHECK(!memcmp(A.plays[plays].own, own, 3) && !memcmp(A.plays[plays].intruder, intr, 3));
      CHECK(A.nalert == 2 && !A.alerts[1].heard && A.alerts[1].relay && A.alerts[1].hops == hops + 1 && A.nquar == 1); /* a relay raises no quarantine notice */
      CHECK(A.alerts[1].nbands == 1 && A.alerts[1].bands[0] == BYS_TOP); /* heard on mid: avoid it, work band top */
      uint32_t t = 4500; uint32_t el = spin_sender(&A, &t, 1110, 200);
      CHECK(el >= 15000 && el < 15000 + 1110 && bys_sender_state(&A.snd) == BYS_SS_DONE);
    } else {
      CHECK(A.nplay == plays && A.nalert == 1 && bys_sender_state(&A.snd) == BYS_SS_DONE && A.nfin == 1); /* hops 2: no relay */
    }
  }
  /* matches_mine, duplicates ignored (once per session), no effect on the next device state */
  dev_init(&A, "A");
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.ambient = 0; bys_sender_start(&A.snd, &c, &A.io, 0);
    uint8_t mine[3]; short_of(TEXT, mine);
    bys_sender_tick(&A.snd, 2000); bys_sender_play_done(&A.snd, 3850);
    bys_sender_hear(&A.snd, 4000, ev_risk(&E, BYS_RISK_CLONE, BYS_ROLE_SENDER, 0, mine, mine, BYS_MID));
    CHECK(A.fail_alert.matches_mine && A.nalert == 2); }
  /* honor off: only the alert and the quarantine notice */
  dev_init(&A, "A");
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.ambient = 0; c.security.risk.honor = 0; bys_sender_start(&A.snd, &c, &A.io, 0);
    bys_sender_tick(&A.snd, 2000); bys_sender_play_done(&A.snd, 3850);
    bys_sender_hear(&A.snd, 4000, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_RECEIVER, 0, own, intr, BYS_MID));
    CHECK(A.nfail == 0 && A.nalert == 1 && A.nquar == 1 && A.nplay == 1 && bys_sender_state(&A.snd) == BYS_SS_WINDOW);
    bys_sender_hear(&A.snd, 4100, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_RECEIVER, 1, own, intr, BYS_MID)); /* same incident: once per session */
    CHECK(A.nalert == 1); }
  /* listen off (basic): nothing */
  dev_init(&A, "A");
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_BASIC); c.ambient = 0; bys_sender_start(&A.snd, &c, &A.io, 0);
    bys_sender_hear(&A.snd, 4000, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_RECEIVER, 0, own, intr, BYS_MID));
    CHECK(A.nalert == 0 && A.nfail == 0); }
  /* a device ignores the incident it emitted itself (it hears its own speaker), within quarantineMs, across sessions (shared memory) */
  dev_init(&A, "A");
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.ambient = 0; c.band = BYS_TOP; bys_sender_start(&A.snd, &c, &A.io, 0);
    bys_sender_tick(&A.snd, 2000); bys_sender_play_done(&A.snd, 3850);
    bys_sender_hear(&A.snd, 4500, ev_payload(&E, OTHER, BYS_UPPER, 0));
    const int na = A.nalert, nf = A.nfail;
    uint8_t sh[3], sho[3]; short_of(TEXT, sh); short_of(OTHER, sho);
    bys_sender_hear(&A.snd, 4600, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_SENDER, 0, sh, sho, BYS_MID)); /* own frame heard back */
    CHECK(A.nalert == na && A.nfail == nf);
    /* a new session of the same device, at once: nothing is blocked (quarantine is a callback) */
    tdev_t *n = &B; dev_init(n, "B"); n->mem = A.mem;
    bys_sender_cfg_t c2 = scfg(n, "next text", BYS_SEC_STANDARD); c2.ambient = 0;
    CHECK(bys_sender_start(&n->snd, &c2, &n->io, 5000) == BYS_OK);
    bys_sender_tick(&n->snd, 7000); CHECK(n->nplay == 1 && n->nfail == 0 && n->nquar == 0);
    /* the same incident heard by the new session within quarantineMs is still its device's own: ignored */
    bys_sender_hear(&n->snd, 8000, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_SENDER, 0, sh, sho, BYS_MID)); CHECK(n->nalert == 0 && n->nfail == 0);
    /* ... but after quarantineMs (30 s) it is news again */
    bys_sender_hear(&n->snd, 4500 + 30000, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_SENDER, 0, sh, sho, BYS_MID)); CHECK(n->nalert >= 1); }
}

/* ======================================================================================================================== */
/* Receiver, synthetic events                                                                                               */
/* ======================================================================================================================== */
static const char *res_ok(void *u, const uint8_t fp[12]) { (void)u; (void)fp; return "menu item 4"; }
static const char *res_none(void *u, const uint8_t fp[12]) { (void)u; (void)fp; return NULL; }
static const char *res_bad(void *u, const uint8_t fp[12]) { (void)u; (void)fp; return "menu item 5"; }

static void test_receiver(void) {
  printf("5. receiver: final listen, delivery, ack loop and receipt, clash, risk, confirm gate, reference mode\n");
  uint8_t sh[3]; short_of(TEXT, sh);
  bys_ids_t ids; bys_text_ids(TEXT, strlen(TEXT), &ids);
  /* standard: first frame -> 1.5 s final listen -> delivered -> acks */
  dev_init(&A, "R");
  bys_receiver_cfg_t c = rcfg(&A, BYS_SEC_STANDARD);
  CHECK(bys_receiver_start(&A.rcv, &c, &A.io, 100) == BYS_OK && bys_receiver_state(&A.rcv) == BYS_RS_LISTENING);
  bys_receiver_hear(&A.rcv, 5000, ev_simple(&E, BYS_EV_HEADER, BYS_TOP));  /* header before the first frame: nothing */
  bys_receiver_hear(&A.rcv, 5000, ev_payload(&E, TEXT, BYS_HIGH, 1));
  CHECK(bys_receiver_state(&A.rcv) == BYS_RS_FINAL_LISTEN && A.nrecv == 0);
  bys_receiver_hear(&A.rcv, 5500, ev_payload(&E, TEXT, BYS_TOP, 0)); /* the same content again, any band: the sender repeating */
  CHECK(A.nfail == 0);
  bys_receiver_tick(&A.rcv, 6499); CHECK(A.nrecv == 0);
  bys_receiver_tick(&A.rcv, 6500);
  CHECK(A.nrecv == 1 && !strcmp(A.rtext, TEXT) && A.rband == BYS_HIGH && A.rvia == 0 && bys_receiver_state(&A.rcv) == BYS_RS_DELIVERED);
  CHECK(!memcmp(bys_receiver_short_id(&A.rcv), sh, 3));
  CHECK(A.nplay == 1 && A.plays[0].kind == BYS_KIND_ACK && A.plays[0].receipt == 0 && A.plays[0].band == BYS_MID && A.plays[0].heard == bys_heard_nibble(BYS_HIGH, BYS_ROBUST) && !memcmp(A.plays[0].hash4, ids.ack, 4));
  /* the heard nibble says robust: the first frame was read in the robust profile */
  { uint32_t now = 6500;
    uint32_t run = spin_receiver(&A, &now, 822, 100);
    CHECK(run >= 7860 && run < 7860 + 822); /* ackRun(3.36 s) */
    CHECK(A.rcv.acking && A.rcv.ack_phase == 1 && !A.rcv.core.playing);
    bys_receiver_tick(&A.rcv, now + 2999); CHECK(A.nplay == (int)(run / 822)); /* listening 3 s for the receipt */
    const int before = A.nplay;
    bys_receiver_tick(&A.rcv, now + 3000); CHECK(A.nplay == before + 1 && A.plays[before].kind == BYS_KIND_ACK);
    /* a receipt with our id ends it; another id or an ack does not */
    bys_receiver_hear(&A.rcv, now + 3500, ev_ack(&E, OTHER, 1, 0, BYS_HIGH)); bys_receiver_hear(&A.rcv, now + 3500, ev_ack(&E, TEXT, 0, 0, BYS_HIGH));
    CHECK(A.nreceipt == 0 && A.rcv.acking);
    const int stops = A.nstop;
    bys_receiver_hear(&A.rcv, now + 3600, ev_ack(&E, TEXT, 1, 0, BYS_HIGH));
    CHECK(A.nreceipt == 1 && A.receipt == 1 && !A.rcv.acking && A.nstop == stops + 1);
    /* the confirm watch (standard) goes on; confirmed() works after delivery and does not stop anything else */
    CHECK(A.nfin == 0);
    bys_confirmation_t cf;
    CHECK(bys_receiver_confirmed(&A.rcv, now + 5000, &cf) == 0 && cf.watch == BYS_WATCH_CLEAN && cf.watched_ms == now + 5000 - 6500 && cf.uncovered_ms == 0);
    CHECK(A.nfin == 1 && bys_receiver_state(&A.rcv) == BYS_RS_CLOSED);
    bys_confirmation_t cf2; CHECK(bys_receiver_confirmed(&A.rcv, now + 9000, &cf2) == 0 && cf2.watched_ms == cf.watched_ms); /* a second call returns the same */ }

  /* acks stop after ACK_TOTAL (30 s) without a receipt */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD);
  bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_tick(&A.rcv, 2500);
  { uint32_t now = 2500;
    for (int i = 0; i < 400 && A.rcv.acking; i++) { spin_receiver(&A, &now, 822, 100); now += 3000; bys_receiver_tick(&A.rcv, now); }
    CHECK(!A.rcv.acking && A.nreceipt == 1 && A.receipt == 0 && now - 2500 >= 30000 && now - 2500 < 30000 + 7860 + 3000 + 1); }

  /* final listen: different content -> MULTIPLE_TRANSMISSIONS and a risk frame {other, receiver, own = first, intruder} */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0));
  bys_receiver_hear(&A.rcv, 1800, ev_payload(&E, OTHER, BYS_UPPER, 0));
  { uint8_t sho[3]; short_of(OTHER, sho);
    CHECK(A.nfail == 1 && A.err == BYS_SERR_MULTIPLE_TRANSMISSIONS && A.nrecv == 0 && bys_receiver_state(&A.rcv) == BYS_RS_FAILED && A.nreceipt == 1 && A.receipt == 0);
    CHECK(A.nplay == 1 && A.plays[0].kind == BYS_KIND_RISK && A.plays[0].reason == BYS_RISK_OTHER && A.plays[0].source == BYS_ROLE_RECEIVER && !memcmp(A.plays[0].own, sh, 3) && !memcmp(A.plays[0].intruder, sho, 3));
    CHECK(A.fail_has_alert && !A.fail_alert.heard && A.nquar == 1 && A.qms == 30000 && A.nfin == 0); /* finished waits for the risk frame */
    uint32_t now = 1800; uint32_t el = spin_receiver(&A, &now, 890, 100);
    CHECK(el >= 4000 && el < 4000 + 890 && A.nfin == 1);
    CHECK(bys_receiver_confirmed(&A.rcv, now, NULL) == BYS_SERR_MULTIPLE_TRANSMISSIONS); }
  /* nothing is delivered from a clash; overlap risk frame with avoid = the first frame's band (or none) */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_simple(&E, BYS_EV_CLASH, 0));
  CHECK(A.err == BYS_SERR_MULTIPLE_TRANSMISSIONS && A.plays[0].reason == BYS_RISK_OVERLAP && A.plays[0].own[0] == 0 && A.plays[0].intruder[0] == 0 && A.alerts[0].nbands == 2); /* none known: [mid, top] */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_hear(&A.rcv, 1200, ev_simple(&E, BYS_EV_CLASH, 0));
  CHECK(A.err == BYS_SERR_MULTIPLE_TRANSMISSIONS && A.plays[0].reason == BYS_RISK_OVERLAP && !memcmp(A.plays[0].own, sh, 3));
  /* basic: no final listen, delivered at once, a clash is ignored, no risk frames */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_BASIC); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_simple(&E, BYS_EV_CLASH, 0)); CHECK(A.nfail == 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0));
  CHECK(A.nrecv == 1 && A.nplay == 1 && A.plays[0].kind == BYS_KIND_ACK);
  { bys_confirmation_t cf; CHECK(bys_receiver_confirmed(&A.rcv, 3000, &cf) == 0 && cf.watch == BYS_WATCH_OFF && cf.watched_ms == 0 && cf.uncovered_ms == 0); }
  /* header on another band extends the final listen to max(end, now + 2.5 s), never past 6 s from the first frame */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0));
  bys_receiver_hear(&A.rcv, 1200, ev_simple(&E, BYS_EV_HEADER, BYS_TOP)); /* same band: nothing */
  bys_receiver_tick(&A.rcv, 2500); CHECK(A.nrecv == 1);
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0));
  bys_receiver_hear(&A.rcv, 1200, ev_simple(&E, BYS_EV_HEADER, BYS_UPPER));
  bys_receiver_tick(&A.rcv, 3699); CHECK(A.nrecv == 0); bys_receiver_tick(&A.rcv, 3700); CHECK(A.nrecv == 1); /* 1200 + 2500 */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0));
  bys_receiver_hear(&A.rcv, 5000, ev_simple(&E, BYS_EV_HEADER, BYS_UPPER)); /* 5000 + 2500 > 1000 + 6000: capped */
  bys_receiver_tick(&A.rcv, 6999); CHECK(A.nrecv == 0); bys_receiver_tick(&A.rcv, 7000); CHECK(A.nrecv == 1);
  /* a header while delivering is ignored by acks: no header events for acks exist (the demodulator does not raise them) */
  /* the text buffer must hold the text and its NUL; packed bytes that are not a valid text are not a frame */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_BASIC); c.text_cap = strlen(TEXT); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); CHECK(A.nrecv == 0 && A.err == BYS_SERR_NO_SPACE);
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_BASIC); c.text_cap = strlen(TEXT) + 1; bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); CHECK(A.nrecv == 1 && !strcmp(A.rtext, TEXT));
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_BASIC); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  ev_payload(&E, TEXT, BYS_TOP, 0); E.packed[0] = 0x01; /* not a packing byte */
  bys_receiver_hear(&A.rcv, 1000, &E.ev); CHECK(A.nrecv == 0 && A.nfail == 0 && bys_receiver_state(&A.rcv) == BYS_RS_LISTENING);
  bys_receiver_hear(&A.rcv, 1500, ev_payload(&E, TEXT, BYS_TOP, 0)); CHECK(A.nrecv == 1);

  /* timeout while nothing is heard: TIMEOUT without a risk frame */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.timeout_ms = 5000; bys_receiver_start(&A.rcv, &c, &A.io, 100);
  bys_receiver_tick(&A.rcv, 5099); CHECK(A.nfail == 0); bys_receiver_tick(&A.rcv, 5100);
  CHECK(A.err == BYS_SERR_TIMEOUT && A.nplay == 0 && A.nfin == 1 && A.nreceipt == 1);

  /* risk frames heard before delivery: RISK_SIGNAL (honored, never relayed) */
  { const uint8_t own[3] = { 1, 2, 3 }, intr[3] = { 4, 5, 6 };
    dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
    bys_receiver_hear(&A.rcv, 1000, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_SENDER, 1, own, intr, BYS_MID));
    CHECK(A.err == BYS_SERR_RISK_SIGNAL && A.nplay == 0 && A.nalert == 1 && A.alerts[0].heard && A.nquar == 1 && A.fail_has_alert && A.nfin == 1);
    dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.security.risk.honor = 0; bys_receiver_start(&A.rcv, &c, &A.io, 0);
    bys_receiver_hear(&A.rcv, 1000, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_SENDER, 1, own, intr, BYS_MID));
    CHECK(A.nfail == 0 && A.nalert == 1 && A.nquar == 1); }

  /* confirm gate ---------------------------------------------------------------------------------------------------- */
  /* a different text read whole closes it: confirmed() fails, a risk frame {other} is looped, acks stop */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_tick(&A.rcv, 2500);
  CHECK(A.nrecv == 1 && A.rcv.acking);
  bys_receiver_hear(&A.rcv, 3000, ev_payload(&E, TEXT, BYS_UPPER, 0)); bys_receiver_hear(&A.rcv, 3000, ev_simple(&E, BYS_EV_CLASH, 0)); bys_receiver_hear(&A.rcv, 3000, ev_simple(&E, BYS_EV_HEADER, BYS_UPPER));
  CHECK(A.rcv.acking && !A.rcv.has_conflict); /* same content again, clash and header: ignored */
  { const int stops = A.nstop;
    bys_receiver_hear(&A.rcv, 3500, ev_payload(&E, OTHER, BYS_UPPER, 0));
    CHECK(A.rcv.has_conflict && !A.rcv.acking && A.nstop == stops + 1 && A.nreceipt == 1 && A.receipt == 0);
    CHECK(A.plays[A.nplay - 1].kind == BYS_KIND_RISK && A.plays[A.nplay - 1].reason == BYS_RISK_OTHER && A.plays[A.nplay - 1].source == BYS_ROLE_RECEIVER && A.nfin == 0);
    CHECK(bys_receiver_confirmed(&A.rcv, 4000, NULL) == BYS_SERR_MULTIPLE_TRANSMISSIONS); }
  /* a risk frame during the watch (standard listenUntil confirm): RISK_SIGNAL, never relayed, no risk frame of its own */
  { const uint8_t own[3] = { 1, 2, 3 }, intr[3] = { 4, 5, 6 };
    dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
    bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_tick(&A.rcv, 2500);
    const int plays = A.nplay;
    bys_receiver_hear(&A.rcv, 3000, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_SENDER, 0, own, intr, BYS_MID));
    CHECK(A.nalert == 1 && A.nquar == 1 && A.rcv.has_conflict && A.nplay == plays && !A.rcv.acking && A.nfin == 1);
    CHECK(bys_receiver_confirmed(&A.rcv, 4000, NULL) == BYS_SERR_RISK_SIGNAL);
    /* with listenUntil = delivery the risk frame after delivery is not heard at all */
    dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.security.risk.listen_until_confirm = 0; bys_receiver_start(&A.rcv, &c, &A.io, 0);
    bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_tick(&A.rcv, 2500);
    bys_receiver_hear(&A.rcv, 3000, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_SENDER, 0, own, intr, BYS_MID));
    CHECK(A.nalert == 0 && !A.rcv.has_conflict);
    /* honor off during the watch: alert only */
    dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.security.risk.honor = 0; bys_receiver_start(&A.rcv, &c, &A.io, 0);
    bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_tick(&A.rcv, 2500);
    bys_receiver_hear(&A.rcv, 3000, ev_risk(&E, BYS_RISK_OTHER, BYS_ROLE_SENDER, 0, own, intr, BYS_MID));
    CHECK(A.nalert == 1 && !A.rcv.has_conflict); }
  /* the watch expires: not a failure, the app learns the uncovered gap */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.security.confirm_watch_ms = 10000; bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_tick(&A.rcv, 2500);
  bys_receiver_tick(&A.rcv, 12499); CHECK(A.rcv.watching); bys_receiver_tick(&A.rcv, 12500); CHECK(!A.rcv.watching && !A.rcv.has_conflict);
  { bys_confirmation_t cf; CHECK(bys_receiver_confirmed(&A.rcv, 20000, &cf) == 0 && cf.watch == BYS_WATCH_EXPIRED && cf.watched_ms == 10000 && cf.uncovered_ms == 7500); }
  /* a different text after the watch expired is not a conflict (nobody was watching) */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.security.confirm_watch_ms = 10000; bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_tick(&A.rcv, 2500); bys_receiver_tick(&A.rcv, 12500);
  bys_receiver_hear(&A.rcv, 13000, ev_payload(&E, OTHER, BYS_TOP, 0)); CHECK(!A.rcv.has_conflict);
  /* cancel: before delivery CANCELLED; after delivery stops watching and acking; after confirmed() nothing */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_cancel(&A.rcv, 500); CHECK(A.err == BYS_SERR_CANCELLED && A.nfin == 1 && bys_receiver_confirmed(&A.rcv, 600, NULL) == BYS_SERR_CANCELLED);
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_tick(&A.rcv, 2500);
  bys_receiver_cancel(&A.rcv, 3000);
  CHECK(!A.rcv.acking && !A.rcv.watching && A.nfin == 1 && A.nstop == 1 && bys_receiver_confirmed(&A.rcv, 3100, NULL) == BYS_SERR_CANCELLED);
  CHECK(bys_receiver_confirmed(&A.rcv, 3100, NULL) == BYS_SERR_CANCELLED);
  /* confirmed() before delivery */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); bys_receiver_start(&A.rcv, &c, &A.io, 0);
  CHECK(bys_receiver_confirmed(&A.rcv, 10, NULL) == BYS_SERR_PENDING);
  /* ack off: no acks, receipt() resolves false */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.ack = 0; bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); bys_receiver_tick(&A.rcv, 2500);
  CHECK(A.nrecv == 1 && A.nplay == 0 && A.nreceipt == 1 && A.receipt == 0);
  /* text buffer too small */
  dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.text_cap = 8; bys_receiver_start(&A.rcv, &c, &A.io, 0);
  bys_receiver_hear(&A.rcv, 1000, ev_payload(&E, TEXT, BYS_TOP, 0)); CHECK(A.err == BYS_SERR_NO_SPACE);

  /* reference mode -------------------------------------------------------------------------------------------------- */
  { static const char *CATALOG = "menu item 4";
    dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD);
    bys_receiver_start(&A.rcv, &c, &A.io, 0);
    bys_receiver_hear(&A.rcv, 1000, ev_ref(&E, CATALOG, BYS_TOP)); CHECK(A.err == BYS_SERR_NO_RESOLVER);
    dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.resolve = res_ok; bys_receiver_start(&A.rcv, &c, &A.io, 0);
    bys_receiver_hear(&A.rcv, 1000, ev_ref(&E, CATALOG, BYS_TOP)); bys_receiver_tick(&A.rcv, 2500);
    CHECK(A.nrecv == 1 && !strcmp(A.rtext, CATALOG) && A.rvia == 1 && A.plays[0].kind == BYS_KIND_ACK);
    bys_ids_t ci; bys_text_ids(CATALOG, strlen(CATALOG), &ci); CHECK(!memcmp(A.plays[0].hash4, ci.ack, 4)); /* the ack id is that of the text */
    dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.resolve = res_none; bys_receiver_start(&A.rcv, &c, &A.io, 0);
    bys_receiver_hear(&A.rcv, 1000, ev_ref(&E, CATALOG, BYS_TOP)); CHECK(A.err == BYS_SERR_NOT_FOUND && A.nplay == 0);
    dev_init(&A, "R"); c = rcfg(&A, BYS_SEC_STANDARD); c.resolve = res_bad; bys_receiver_start(&A.rcv, &c, &A.io, 0);
    bys_receiver_hear(&A.rcv, 1000, ev_ref(&E, CATALOG, BYS_TOP)); CHECK(A.err == BYS_SERR_MISMATCH && A.nplay == 0);
    /* the reference sender alternates parity sets A and B, and acks use the same ack id */
    dev_init(&B, "S"); bys_sender_cfg_t sc = scfg(&B, CATALOG, BYS_SEC_BASIC); sc.ambient = 0; sc.reference = 1; bys_sender_start(&B.snd, &sc, &B.io, 0);
    uint32_t now = 0;
    for (int i = 0; i < 4; i++) { now += 1000; bys_sender_play_done(&B.snd, now); now += 2000; bys_sender_tick(&B.snd, now); }
    CHECK(B.nplay == 5 && B.plays[0].kind == BYS_KIND_REFERENCE && B.plays[0].set == 0 && B.plays[1].set == 1 && B.plays[2].set == 0 && !memcmp(B.plays[0].fp, ci.fingerprint, 12));
    bys_sender_hear(&B.snd, now, ev_ack(&E, CATALOG, 0, 0, BYS_MID)); CHECK(B.ndeliv == 1 && !B.dl.stated);
  }
}

/* ======================================================================================================================== */
/* Ladder, ambient, robust, calibration                                                                                     */
/* ======================================================================================================================== */
/* Plays n repetitions of a sender with no ack and no events (each frame 1000 ms, windows 2000 ms). */
static void run_reps(tdev_t *d, uint32_t *now, int n) {
  for (int i = 0; i < n; i++) { *now += 1000; bys_sender_play_done(&d->snd, *now); *now += 2000; bys_sender_tick(&d->snd, *now); }
}
static bys_sender_cfg_t quick(tdev_t *d) { bys_sender_cfg_t c = scfg(d, TEXT, BYS_SEC_BASIC); c.ambient = 0; c.timeout_ms = 1000000; return c; }

static void test_adapt(void) {
  printf("6. ladder, robust, ambient, calibration\n");
  uint32_t now = 0;
  /* default ladder: top x3, high x3, upper x3, mid x3, then around again; robust after 6 normal repetitions (all four bands have it) */
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); bys_sender_start(&A.snd, &c, &A.io, now); }
  run_reps(&A, &now, 13);
  static const int LADDER[14] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 0, 0 };
  CHECK(A.nrep == 14);
  for (int i = 0; i < 14 && i < A.nrep; i++) { CHECK(A.reps[i].band == LADDER[i]); CHECK(A.reps[i].profile == (i < 6 ? BYS_NORMAL : BYS_ROBUST)); CHECK(A.reps[i].i == (uint32_t)i); }
  /* explicit profile: no ladder of profiles */
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); c.profile = BYS_NORMAL; now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  run_reps(&A, &now, 9); { int ok = 1; for (int i = 0; i < 10; i++) if (A.reps[i].profile != BYS_NORMAL) ok = 0; CHECK(ok); }
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); c.profile = BYS_ROBUST; now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  run_reps(&A, &now, 9); { int ok = 1; for (int i = 0; i < 10; i++) if (A.reps[i].profile != BYS_ROBUST) ok = 0; CHECK(ok); }
  /* fixed band: that band only; xhigh has no robust profile */
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); c.band = BYS_XHIGH; now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  run_reps(&A, &now, 9); { int ok = 1; for (int i = 0; i < 10; i++) if (A.reps[i].band != BYS_XHIGH || A.reps[i].profile != BYS_NORMAL) ok = 0; CHECK(ok); }
  /* robust bands: wide and xhigh stay normal even after the robust switch */
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); c.nchannels = 2; c.channels[0] = BYS_XHIGH; c.channels[1] = BYS_MID; now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  run_reps(&A, &now, 10); CHECK(A.reps[8].band == BYS_XHIGH && A.reps[8].profile == BYS_NORMAL && A.reps[9].band == BYS_MID && A.reps[9].profile == BYS_ROBUST);

  /* calibration: valid -> route from the calibrated band (mid, upper, high, top), 6 per channel, uses + 1 saved at once */
  dev_init(&A, "A"); A.have_cal = 1; A.cal = (bys_calibration_t){ BYS_MID, BYS_NORMAL, 3 };
  { bys_sender_cfg_t c = quick(&A); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  CHECK(A.nsave == 1 && A.cal.band == BYS_MID && A.cal.uses == 4);
  run_reps(&A, &now, 13);
  { static const int L2[14] = { 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 1, 1 }; for (int i = 0; i < 14 && i < A.nrep; i++) CHECK(A.reps[i].band == L2[i]); }
  CHECK(A.nroute == 4 && A.route[0] == BYS_MID && A.route[1] == BYS_UPPER && A.route[2] == BYS_HIGH && A.route[3] == BYS_TOP);
  /* expired (10 uses), or its band is not in the chain: the ladder starts from the top again */
  dev_init(&A, "A"); A.have_cal = 1; A.cal = (bys_calibration_t){ BYS_MID, BYS_NORMAL, 10 };
  { bys_sender_cfg_t c = quick(&A); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); } CHECK(A.reps[0].band == BYS_TOP && A.nsave == 0);
  dev_init(&A, "A"); A.have_cal = 1; A.cal = (bys_calibration_t){ BYS_UPPER, BYS_NORMAL, 0 };
  { bys_sender_cfg_t c = quick(&A); c.nchannels = 2; c.channels[0] = BYS_TOP; c.channels[1] = BYS_MID; now = 0; bys_sender_start(&A.snd, &c, &A.io, now); } CHECK(A.reps[0].band == BYS_TOP);
  /* a robust calibration starts robust; a fixed band ignores it */
  dev_init(&A, "A"); A.have_cal = 1; A.cal = (bys_calibration_t){ BYS_HIGH, BYS_ROBUST, 0 };
  { bys_sender_cfg_t c = quick(&A); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); } CHECK(A.reps[0].band == BYS_HIGH && A.reps[0].profile == BYS_ROBUST);
  dev_init(&A, "A"); A.have_cal = 1; A.cal = (bys_calibration_t){ BYS_HIGH, BYS_ROBUST, 0 };
  { bys_sender_cfg_t c = quick(&A); c.band = BYS_TOP; now = 0; bys_sender_start(&A.snd, &c, &A.io, now); } CHECK(A.reps[0].band == BYS_TOP && A.reps[0].profile == BYS_NORMAL && A.nsave == 0);

  /* learning: the ack's heard nibble becomes the calibration, uses 0 */
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  run_reps(&A, &now, 3); /* 4th repetition on high */
  bys_sender_hear(&A.snd, now, ev_ack(&E, TEXT, 0, bys_heard_nibble(BYS_HIGH, BYS_NORMAL), BYS_MID));
  CHECK(A.ndeliv == 1 && A.dl.saved && A.cal.band == BYS_HIGH && A.cal.profile == BYS_NORMAL && A.cal.uses == 0);
  /* robust reached through the robust ladder belongs to the place: saved normal */
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  run_reps(&A, &now, 9); /* mid, robust */
  bys_sender_hear(&A.snd, now, ev_ack(&E, TEXT, 0, bys_heard_nibble(BYS_MID, BYS_ROBUST), BYS_MID));
  CHECK(A.dl.saved && A.cal.band == BYS_MID && A.cal.profile == BYS_NORMAL && A.dl.profile == BYS_ROBUST);
  /* an explicit robust profile saves robust */
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); c.profile = BYS_ROBUST; now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, now + 100, ev_ack(&E, TEXT, 0, bys_heard_nibble(BYS_TOP, BYS_ROBUST), BYS_MID)); CHECK(A.cal.profile == BYS_ROBUST && A.dl.saved);
  /* not learned: not stated, audible, fixed band */
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, now + 100, ev_ack(&E, TEXT, 0, 0, BYS_MID)); CHECK(A.ndeliv == 1 && !A.dl.saved && !A.dl.stated && A.dl.band == BYS_TOP && A.nsave == 0);
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, now + 100, ev_ack(&E, TEXT, 0, bys_heard_nibble(BYS_AUDIBLE, BYS_NORMAL), BYS_MID)); CHECK(A.ndeliv == 1 && !A.dl.saved && A.dl.band == BYS_AUDIBLE && A.nsave == 0);
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); c.band = BYS_HIGH; now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_hear(&A.snd, now + 100, ev_ack(&E, TEXT, 0, bys_heard_nibble(BYS_HIGH, BYS_NORMAL), BYS_MID)); CHECK(A.ndeliv == 1 && !A.dl.saved && A.nsave == 0);
  /* lapped: it went round the whole route (changes of band > channels) and came back: a late read says nothing about the speaker */
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  run_reps(&A, &now, 12); /* 13th repetition: top again after top, high, upper, mid, top = 5 changes > 4 */
  CHECK(A.reps[12].band == BYS_TOP);
  bys_sender_hear(&A.snd, now, ev_ack(&E, TEXT, 0, bys_heard_nibble(BYS_TOP, BYS_NORMAL), BYS_MID)); CHECK(A.ndeliv == 1 && !A.dl.saved && A.nsave == 0);
  dev_init(&A, "A"); { bys_sender_cfg_t c = quick(&A); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  run_reps(&A, &now, 9); /* the 4th channel (mid): 4 changes, not lapped */
  bys_sender_hear(&A.snd, now, ev_ack(&E, TEXT, 0, bys_heard_nibble(BYS_MID, BYS_NORMAL), BYS_MID)); CHECK(A.dl.saved);

  /* ambient: noise reports of the pre-listen */
  dev_init(&A, "A"); A.rnd = 0;
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.timeout_ms = 1000000; now = 0; bys_sender_start(&A.snd, &c, &A.io, now);
    float db[BYS_BAND_COUNT];
    for (int i = 0; i < 8; i++) {
      for (int b = 0; b < BYS_BAND_COUNT; b++) db[b] = NAN;
      db[BYS_TOP] = -80.0f + (i == 3 ? 30.0f : 0.0f);   /* a spike: the median ignores it */
      db[BYS_HIGH] = -66.0f; db[BYS_UPPER] = -75.0f; db[BYS_MID] = -90.0f; /* mid is the quietest; high is 24 dB above, upper 15 dB: both noisy */
      bys_sender_noise(&A.snd, now, db); now += 250;
    }
    CHECK(A.nplay == 0); }
  bys_sender_tick(&A.snd, 2000);
  CHECK(A.nplay == 1 && A.namb == 1 && A.amb.present && A.amb.loud == 0);
  CHECK(A.amb.noisy_mask == ((1u << BYS_HIGH) | (1u << BYS_UPPER)) && A.amb.noise_db[BYS_TOP] == -80.0f && A.amb.noise_db[BYS_MID] == -90.0f);
  CHECK(A.nroute == 2 && A.route[0] == BYS_TOP && A.route[1] == BYS_MID);
  CHECK(A.reps[0].band == BYS_TOP && A.reps[0].profile == BYS_NORMAL); /* -80 dB: below -68, starts normal */
  /* loud place and robust start: the first channel of the route at -68 dB or more */
  dev_init(&A, "A"); A.rnd = 0;
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.timeout_ms = 1000000; now = 0; bys_sender_start(&A.snd, &c, &A.io, now);
    float db[BYS_BAND_COUNT]; for (int b = 0; b < BYS_BAND_COUNT; b++) db[b] = NAN;
    db[BYS_TOP] = -67.0f; db[BYS_HIGH] = -67.0f; db[BYS_UPPER] = -67.0f; db[BYS_MID] = -62.0f;
    for (int i = 0; i < 4; i++) bys_sender_noise(&A.snd, now, db); }
  bys_sender_tick(&A.snd, 2000);
  CHECK(A.amb.noisy_mask == 0 && !A.amb.loud && A.reps[0].band == BYS_TOP && A.reps[0].profile == BYS_ROBUST);
  dev_init(&A, "A"); A.rnd = 0;
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); now = 0; bys_sender_start(&A.snd, &c, &A.io, now);
    float db[BYS_BAND_COUNT]; for (int b = 0; b < BYS_BAND_COUNT; b++) db[b] = -60.0f;
    bys_sender_noise(&A.snd, now, db); }
  bys_sender_tick(&A.snd, 2000); CHECK(A.amb.loud && A.reps[0].profile == BYS_ROBUST);
  /* an explicit profile wins over the noise start */
  dev_init(&A, "A"); A.rnd = 0;
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.profile = BYS_NORMAL; now = 0; bys_sender_start(&A.snd, &c, &A.io, now);
    float db[BYS_BAND_COUNT]; for (int b = 0; b < BYS_BAND_COUNT; b++) db[b] = -60.0f;
    bys_sender_noise(&A.snd, now, db); }
  bys_sender_tick(&A.snd, 2000); CHECK(A.reps[0].profile == BYS_NORMAL);
  /* ambient off: no decision at all */
  dev_init(&A, "A"); A.rnd = 0;
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.ambient = 0; now = 0; bys_sender_start(&A.snd, &c, &A.io, now);
    float db[BYS_BAND_COUNT]; for (int b = 0; b < BYS_BAND_COUNT; b++) db[b] = -60.0f;
    bys_sender_noise(&A.snd, now, db); }
  bys_sender_tick(&A.snd, 2000); CHECK(!A.amb.present && A.reps[0].profile == BYS_NORMAL && A.nroute == 4);
  /* no reports (no way to measure): no ambient decision, the plain ladder */
  dev_init(&A, "A"); A.rnd = 0; { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); now = 0; bys_sender_start(&A.snd, &c, &A.io, now); }
  bys_sender_tick(&A.snd, 2000); CHECK(!A.amb.present && A.nroute == 4 && A.reps[0].band == BYS_TOP);
  /* the calibrated channel stays even if noisy; and the calibration is not replaced by a read elsewhere when the ambient moved it */
  dev_init(&A, "A"); A.rnd = 0; A.have_cal = 1; A.cal = (bys_calibration_t){ BYS_MID, BYS_NORMAL, 0 };
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.timeout_ms = 1000000; now = 0; bys_sender_start(&A.snd, &c, &A.io, now);
    float db[BYS_BAND_COUNT]; for (int b = 0; b < BYS_BAND_COUNT; b++) db[b] = NAN;
    db[BYS_TOP] = -85.0f; db[BYS_HIGH] = -85.0f; db[BYS_UPPER] = -85.0f; db[BYS_MID] = -60.0f; /* mid is 25 dB noisier: normally skipped */
    bys_sender_noise(&A.snd, now, db); }
  bys_sender_tick(&A.snd, 2000);
  CHECK(A.nroute == 4 && A.route[0] == BYS_MID && A.reps[0].band == BYS_MID && A.reps[0].profile == BYS_ROBUST); /* -60 dB at the start: robust, from the noise */
  { const int saves = A.nsave;
    bys_sender_hear(&A.snd, 2500, ev_ack(&E, TEXT, 0, bys_heard_nibble(BYS_UPPER, BYS_NORMAL), BYS_MID)); /* read elsewhere: the ambient moved us */
    CHECK(A.ndeliv == 1 && !A.dl.saved && A.nsave == saves && A.cal.band == BYS_MID); }
  /* noisy channels dropped; if nothing is left, the quietest of the chain (no report counts as 0 dB): chain of one noisy channel */
  dev_init(&A, "A"); A.rnd = 0;
  { bys_sender_cfg_t c = scfg(&A, TEXT, BYS_SEC_STANDARD); c.nchannels = 2; c.channels[0] = BYS_TOP; c.channels[1] = BYS_HIGH; now = 0; bys_sender_start(&A.snd, &c, &A.io, now);
    float db[BYS_BAND_COUNT]; for (int b = 0; b < BYS_BAND_COUNT; b++) db[b] = NAN; db[BYS_TOP] = -90.0f; db[BYS_HIGH] = -50.0f;
    bys_sender_noise(&A.snd, now, db); }
  bys_sender_tick(&A.snd, 2000); CHECK(A.nroute == 1 && A.route[0] == BYS_TOP && A.amb.noisy_mask == (1u << BYS_HIGH));
}

/* ======================================================================================================================== */
/* Simulated air: devices with a speaker, a microphone and real demodulators                                                */
/* ======================================================================================================================== */
#define RATE 48000
#define BLOCK 480
#define MAXDEV 6
#define FIR_TAPS 257
#define PI_ 3.14159265358979323846

typedef struct {
  tdev_t log;                         /* what its session did (log.io is the session's io) */
  int half_duplex, robust_rx, lowpass_hz;
  bys_demod_t dn;
  bys_demod_t dr;
  uint8_t outn[255 * 48 + 8], outr[255 * 48 + 8];
  /* speaker */
  bys_tx_t tx; int playing, ended; uint32_t hist_n; float hist[FIR_TAPS];
  /* raw emitter: not a session, plays a frame in a loop from `raw_from` until `raw_until` */
  int raw_on; uint32_t raw_from, raw_until; bys_session_frame_t raw_frame;
  /* what the microphone decoded */
  int n_payload, n_ack, n_receipt, n_risk, n_header, n_clash;
  bys_result_t last_risk; uint8_t last_risk_band; int n_risk_hops[4];
  char last_text[400];
  uint32_t last_noise_ms;
  float *fir;
} adev_t;

static adev_t W[MAXDEV];
static int nW;
static float GAIN[MAXDEV][MAXDEV];
static uint32_t T_SAMPLES;
static float FIR[FIR_TAPS];
static uint32_t noise_state = 777;
static double noise_sigma;
static uint32_t now_ms(void) { return (uint32_t)((uint64_t)T_SAMPLES * 1000u / RATE); }

static void fir_design(float cutoff_hz) {
  const double fc = cutoff_hz / RATE;
  double sum = 0;
  for (int i = 0; i < FIR_TAPS; i++) {
    const double x = i - (FIR_TAPS - 1) / 2.0;
    const double s = x == 0 ? 2 * fc : sin(2 * PI_ * fc * x) / (PI_ * x);
    const double w = 0.42 - 0.5 * cos(2 * PI_ * i / (FIR_TAPS - 1)) + 0.08 * cos(4 * PI_ * i / (FIR_TAPS - 1));
    FIR[i] = (float)(s * w);
    sum += FIR[i];
  }
  for (int i = 0; i < FIR_TAPS; i++) FIR[i] = (float)(FIR[i] / sum);
}
static double gauss(void) {
  double u1, u2;
  noise_state ^= noise_state << 13; noise_state ^= noise_state >> 17; noise_state ^= noise_state << 5; u1 = (noise_state >> 8) / 16777216.0 + 1e-9;
  noise_state ^= noise_state << 13; noise_state ^= noise_state >> 17; noise_state ^= noise_state << 5; u2 = (noise_state >> 8) / 16777216.0;
  return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

static void a_hear(void *user, const bys_demod_event_t *ev) {
  adev_t *a = user;
  tdev_t *d = &a->log;
  const uint32_t now = now_ms();
  if (ev->type == BYS_EV_HEADER) a->n_header++;
  else if (ev->type == BYS_EV_CLASH) a->n_clash++;
  else if (ev->result) {
    switch (ev->result->kind) {
      case BYS_KIND_PAYLOAD: a->n_payload++; if (ev->payload) bys_unpack(ev->payload, ev->payload_len, a->last_text, sizeof a->last_text); break;
      case BYS_KIND_ACK: if (ev->result->receipt) a->n_receipt++; else a->n_ack++; break;
      case BYS_KIND_RISK: a->n_risk++; a->last_risk = *ev->result; a->last_risk_band = ev->band; if (ev->result->hops < 4) a->n_risk_hops[ev->result->hops]++; break;
      default: break;
    }
  }
  if (d->has_snd) bys_sender_hear(&d->snd, now, ev);
  if (d->has_rcv) bys_receiver_hear(&d->rcv, now, ev);
}

static void a_on_play(tdev_t *d, const bys_session_frame_t *f) {
  adev_t *a = (adev_t *)d; /* log is the first member */
  if (bys_session_tx_init(&a->tx, &TXC, f) != BYS_OK) { d->play_rc = -1; return; }
  a->playing = 1; a->ended = 0; a->hist_n = 0; memset(a->hist, 0, sizeof a->hist);
}
static void a_on_stop(tdev_t *d) { adev_t *a = (adev_t *)d; a->playing = 0; a->ended = 0; }

static adev_t *add_dev(const char *name, int half_duplex, int robust_rx) {
  adev_t *a = &W[nW++];
  memset(a, 0, sizeof *a);
  dev_init(&a->log, name);
  a->log.on_play = a_on_play; a->log.on_stop = a_on_stop;
  a->half_duplex = half_duplex; a->robust_rx = robust_rx;
  bys_demod_cfg_t cfg;
  bys_demod_cfg_default(&cfg);
  cfg.sample_rate = RATE; cfg.profile = BYS_NORMAL; cfg.out = a->outn; cfg.out_cap = sizeof a->outn; cfg.cb = a_hear; cfg.user = a;
  CHECK(bys_demod_init(&a->dn, &cfg) == BYS_OK);
  if (robust_rx) { cfg.profile = BYS_ROBUST; cfg.out = a->outr; cfg.out_cap = sizeof a->outr; CHECK(bys_demod_init(&a->dr, &cfg) == BYS_OK); }
  return a;
}

static void world_reset(float noise_sigma_fs) {
  nW = 0; T_SAMPLES = 0; noise_sigma = noise_sigma_fs * 32768.0; noise_state = 777;
  for (int i = 0; i < MAXDEV; i++) for (int j = 0; j < MAXDEV; j++) GAIN[i][j] = i == j ? 0.0f : 1.0f;
}
static void gain(adev_t *from, adev_t *to, float g, int oneway) {
  GAIN[from - W][to - W] = g;
  if (!oneway) GAIN[to - W][from - W] = g;
}

static void raw_play(adev_t *a, const bys_session_frame_t *f, uint32_t from_ms, uint32_t until_ms) { a->raw_on = 1; a->raw_frame = *f; a->raw_from = from_ms; a->raw_until = until_ms; }
static bys_session_frame_t raw_payload(const char *text, bys_band_t band) {
  bys_session_frame_t f; memset(&f, 0, sizeof f); f.kind = BYS_KIND_PAYLOAD; f.band = (uint8_t)band; f.text = text; f.text_len = strlen(text); f.gap_ms = BYS_GAP_MS; return f;
}

/* One block of audio for every device. */
static void world_step(void) {
  static int16_t spk[MAXDEV][BLOCK];
  static int16_t mic[BLOCK];
  uint32_t now = now_ms();
  for (int i = 0; i < nW; i++) {
    adev_t *a = &W[i];
    if (a->raw_on && !a->playing && now >= a->raw_from && now < a->raw_until) {
      if (bys_session_tx_init(&a->tx, &TXC, &a->raw_frame) == BYS_OK) { a->playing = 1; a->ended = 0; a->hist_n = 0; memset(a->hist, 0, sizeof a->hist); }
    }
    memset(spk[i], 0, sizeof spk[i]);
    if (!a->playing) continue;
    size_t n = bys_tx_fill(&a->tx, spk[i], BLOCK);
    if (a->lowpass_hz) {
      for (size_t k = 0; k < BLOCK; k++) {
        a->hist[a->hist_n % FIR_TAPS] = (float)spk[i][k];
        float y = 0;
        for (int t = 0; t < FIR_TAPS; t++) y += FIR[t] * a->hist[(a->hist_n + FIR_TAPS - (uint32_t)t) % FIR_TAPS];
        a->hist_n++;
        spk[i][k] = (int16_t)y;
      }
    }
    if (n < BLOCK || bys_tx_done(&a->tx)) { a->playing = 0; a->ended = 1; }
  }
  for (int d = 0; d < nW; d++) {
    adev_t *a = &W[d];
    for (int k = 0; k < BLOCK; k++) {
      double v = noise_sigma ? noise_sigma * gauss() : 0;
      for (int j = 0; j < nW; j++) if (GAIN[j][d] != 0.0f) v += GAIN[j][d] * spk[j][k];
      mic[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    const int deaf = a->half_duplex && (a->playing || a->ended);
    if (!deaf) { bys_demod_push(&a->dn, mic, BLOCK); if (a->robust_rx) bys_demod_push(&a->dr, mic, BLOCK); }
  }
  T_SAMPLES += BLOCK;
  now = now_ms();
  for (int i = 0; i < nW; i++) {
    adev_t *a = &W[i];
    tdev_t *d = &a->log;
    if (a->ended) {
      a->ended = 0;
      if (a->half_duplex) { bys_demod_reset(&a->dn); if (a->robust_rx) bys_demod_reset(&a->dr); } /* forget our own sound */
      if (d->has_snd) bys_sender_play_done(&d->snd, now);
      else if (d->has_rcv) bys_receiver_play_done(&d->rcv, now);
    }
    if (d->has_snd) {
      if (bys_sender_state(&d->snd) == BYS_SS_PRE_LISTEN && now - a->last_noise_ms >= BYS_NOISE_REPORT_MS) {
        float db[BYS_BAND_COUNT];
        bys_session_noise_from_demod(&a->dn, db);
        bys_sender_noise(&d->snd, now, db);
        a->last_noise_ms = now;
      }
      bys_sender_tick(&d->snd, now);
    }
    if (d->has_rcv) bys_receiver_tick(&d->rcv, now);
  }
}
typedef int (*until_fn)(void);
static void world_run(uint32_t max_ms, until_fn until) {
  const uint32_t end = now_ms() + max_ms;
  while (now_ms() < end && !(until && until())) world_step();
}
static void start_sender(adev_t *a, const bys_sender_cfg_t *c) { a->log.has_snd = 1; CHECK(bys_sender_start(&a->log.snd, c, &a->log.io, now_ms()) == BYS_OK); }
static void start_receiver(adev_t *a, const bys_receiver_cfg_t *c) { a->log.has_rcv = 1; CHECK(bys_receiver_start(&a->log.rcv, c, &a->log.io, now_ms()) == BYS_OK); }

static int snd_done(const adev_t *a) { return bys_sender_state(&a->log.snd) == BYS_SS_DONE; }

/* ---- scenario 1: sender, receiver; the ack stops the sender, the receipt stops the receiver ---- */
static int u1(void) { return snd_done(&W[0]) && W[1].log.nreceipt; }
static void test_air_ack(void) {
  printf("7. air: sender -> receiver, ack stops the sender, receipt stops the receiver (maximum security, half duplex)\n");
  world_reset(0.002f);
  adev_t *s = add_dev("S", 1, 0), *r = add_dev("R", 1, 1);
  bys_sender_cfg_t sc = scfg(&s->log, TEXT, BYS_SEC_MAXIMUM);
  bys_receiver_cfg_t rc = rcfg(&r->log, BYS_SEC_MAXIMUM);
  s->log.rnd = 300;
  start_receiver(r, &rc); start_sender(s, &sc);
  world_run(70000, u1);
  CHECK(snd_done(s) && s->log.ndeliv == 1 && s->log.nfail == 0);
  CHECK(r->log.nrecv == 1 && !strcmp(r->log.rtext, TEXT) && r->log.nreceipt == 1 && r->log.receipt == 1 && r->log.nfail == 0);
  CHECK(s->log.dl.stated && s->log.dl.band == BYS_TOP && s->log.dl.profile == BYS_NORMAL); /* the band nibble made the trip */
  CHECK(s->log.dl.saved && s->log.cal.band == BYS_TOP);
  /* the sender stopped the data as soon as it heard the ack: at most two repetitions */
  { int data = 0; for (int i = 0; i < s->log.nplay && i < NPLAY; i++) if (s->log.plays[i].kind == BYS_KIND_PAYLOAD) data++; CHECK(data >= 1 && data <= 2); CHECK(s->log.dl.repetitions == (uint32_t)data); }
  /* acks go on mid, receipts on high; the demodulators decoded them and raised neither header nor clash events for them */
  CHECK(s->n_ack >= 1 && r->n_receipt >= 1 && s->n_clash == 0 && r->n_clash == 0);
  CHECK(r->n_header >= 1); /* the receiver saw the data header */
  /* the receipts ended with the receiver's acks: the receiver is no longer acking */
  CHECK(!r->log.rcv.acking && r->log.nfin == 0); /* still watching: finished waits for confirmed() */
  /* the confirm gate: nothing else was heard */
  { bys_confirmation_t cf; CHECK(bys_receiver_confirmed(&r->log.rcv, now_ms(), &cf) == 0 && cf.watch == BYS_WATCH_CLEAN && cf.watched_ms > 0); CHECK(r->log.nfin == 1); }
  printf("  ack path took %.1f s of simulated time\n", now_ms() / 1000.0);
}

/* ---- scenario 2: a speaker deaf above 17 kHz: the ladder falls to mid, the calibration is learned and used ---- */
static int u2a(void) { return snd_done(&W[0]) && W[1].log.nrecv; }
static void test_air_calibration(void) {
  printf("8. air: speaker that cannot play above 17 kHz: ladder down to mid, calibration learned, then read on the first repetition\n");
  fir_design(16700);
  world_reset(0.002f);
  adev_t *s = add_dev("S", 1, 0), *r = add_dev("R", 1, 1);
  s->lowpass_hz = 16700;
  bys_calibration_t store; int have = 0; (void)store; (void)have;
  bys_sender_cfg_t sc = scfg(&s->log, TEXT, BYS_SEC_STANDARD);
  bys_receiver_cfg_t rc = rcfg(&r->log, BYS_SEC_STANDARD);
  s->log.rnd = 0;
  start_receiver(r, &rc); start_sender(s, &sc);
  world_run(110000, u2a);
  CHECK(s->log.ndeliv == 1 && s->log.nfail == 0 && r->log.nrecv == 1);
  if (s->log.ndeliv) {
    CHECK(s->log.dl.stated && s->log.dl.band == BYS_MID);
    CHECK(s->log.nrep >= 10); /* top x3, high x3, upper x3 were not read */
    int ok = 1; for (int i = 0; i < 9 && i < s->log.nrep; i++) if (s->log.reps[i].band == BYS_MID) ok = 0; CHECK(ok);
    CHECK(s->log.dl.saved && s->log.cal.band == BYS_MID && s->log.cal.uses == 0 && s->log.cal.profile == BYS_NORMAL); /* the robust read came from the ladder, not the speaker */
    printf("  first send: read on %s after %u repetitions, %.1f s\n", "mid", s->log.dl.repetitions, s->log.dl.ms / 1000.0);
  }
  /* second send, same device: starts on mid (its calibration) and is read on the first repetition */
  const bys_calibration_t kept = s->log.cal;
  const int have_kept = s->log.have_cal;
  s->playing = r->playing = 0;
  dev_clear_log(&s->log); dev_clear_log(&r->log);
  s->log.has_snd = 0; r->log.has_rcv = 0;
  s->log.cal = kept; s->log.have_cal = have_kept;
  s->log.on_play = a_on_play; s->log.on_stop = a_on_stop; r->log.on_play = a_on_play; r->log.on_stop = a_on_stop;
  bys_demod_reset(&s->dn); bys_demod_reset(&r->dn); bys_demod_reset(&r->dr);
  world_run(2000, NULL);
  sc = scfg(&s->log, TEXT, BYS_SEC_STANDARD); rc = rcfg(&r->log, BYS_SEC_STANDARD);
  s->log.rnd = 0;
  start_receiver(r, &rc); start_sender(s, &sc);
  const uint32_t t0 = now_ms();
  world_run(60000, u2a);
  CHECK(s->log.ndeliv == 1 && s->log.nfail == 0 && r->log.nrecv == 1);
  CHECK(s->log.nrep >= 1 && s->log.reps[0].band == BYS_MID && s->log.reps[0].profile == BYS_NORMAL && s->log.dl.repetitions == 1);
  CHECK(s->log.dl.band == BYS_MID && s->log.cal.uses == 0 && s->log.nsave >= 2); /* uses + 1 when it started, 0 again after the read */
  printf("  second send: read on mid at the first repetition, %.1f s in total\n", (now_ms() - t0) / 1000.0);
}

/* ---- scenario 3: three parties: a rogue plays another text; the receiver warns; the real sender stops and relays ---- */
static int u3(void) { return snd_done(&W[0]) && W[1].log.nfin; }
static void test_air_second_sender(void) {
  printf("9. air: real sender A, receiver R, rogue X (A cannot hear X): R fails, warns by sound; A stops and relays\n");
  world_reset(0.002f);
  adev_t *a = add_dev("A", 1, 0), *r = add_dev("R", 1, 1), *x = add_dev("X", 1, 0);
  gain(a, x, 0.0f, 0);
  bys_sender_cfg_t sc = scfg(&a->log, TEXT, BYS_SEC_STANDARD);
  sc.ambient = 0; sc.band = BYS_TOP; sc.security.risk.detector_duration_ms = 6000;
  bys_receiver_cfg_t rc = rcfg(&r->log, BYS_SEC_STANDARD);
  a->log.rnd = 0;
  start_receiver(r, &rc); start_sender(a, &sc);
  /* X starts 0.3 s after A's first frame ends (inside A's listen window, in R's final listen), on another band */
  const bys_session_frame_t xf = raw_payload(OTHER, BYS_UPPER);
  raw_play(x, &xf, 2000 + 1500 + 350 + 300, 2000 + 1500 + 350 + 300 + 1); /* one frame */
  world_run(40000, u3);
  CHECK(r->log.nfail == 1 && r->log.err == BYS_SERR_MULTIPLE_TRANSMISSIONS && r->log.nrecv == 0);
  CHECK(r->log.nplay >= 1 && r->log.plays[0].kind == BYS_KIND_RISK && r->log.plays[0].reason == BYS_RISK_OTHER && r->log.plays[0].source == BYS_ROLE_RECEIVER);
  { uint8_t sh[3], sho[3]; short_of(TEXT, sh); short_of(OTHER, sho);
    CHECK(!memcmp(r->log.plays[0].own, sh, 3) && !memcmp(r->log.plays[0].intruder, sho, 3)); }
  CHECK(a->log.nfail == 1 && a->log.err == BYS_SERR_RISK_SIGNAL && a->log.fail_has_alert && a->log.fail_alert.heard && a->log.fail_alert.source == BYS_ROLE_RECEIVER && a->log.fail_alert.hops == 0);
  CHECK(a->log.nquar == 1 && a->log.qms == 30000);
  CHECK(a->log.nalert == 2 && a->log.alerts[1].relay && a->log.alerts[1].hops == 1);
  CHECK(snd_done(a) && a->log.ndeliv == 0);
  { int relays = 0; for (int i = 0; i < a->log.nplay && i < NPLAY; i++) if (a->log.plays[i].kind == BYS_KIND_RISK && a->log.plays[i].hops == 1) relays++; CHECK(relays >= 5); }
  printf("  risk frames on air: receiver %d, relayed by A %d, heard by the rogue %d\n", r->log.nplay, a->log.nplay, x->n_risk);
}

/* ---- scenario 3b: same band at once: a clash, nothing delivered, overlap risk ---- */
static int u3b(void) { return W[1].log.nfin; }
static void test_air_clash(void) {
  printf("10. air: two senders at once on the same band: clash, nothing delivered, overlap risk frame\n");
  world_reset(0.002f);
  adev_t *a = add_dev("A", 1, 0), *r = add_dev("R", 1, 1), *x = add_dev("X", 1, 0);
  gain(a, x, 0.0f, 0);
  bys_receiver_cfg_t rc = rcfg(&r->log, BYS_SEC_STANDARD);
  start_receiver(r, &rc);
  const bys_session_frame_t fa = raw_payload(TEXT, BYS_TOP), fx = raw_payload(OTHER, BYS_TOP);
  raw_play(a, &fa, 1000, 1001); raw_play(x, &fx, 1500, 1501); /* 0.5 s apart: not an echo (SESSION: overlap on one band) */
  world_run(20000, u3b);
  CHECK(r->n_clash >= 1 && r->log.nrecv == 0 && r->log.nfail == 1 && r->log.err == BYS_SERR_MULTIPLE_TRANSMISSIONS);
  CHECK(r->log.nplay >= 1 && r->log.plays[0].reason == BYS_RISK_OVERLAP && r->log.plays[0].intruder[0] == 0 && r->log.plays[0].intruder[1] == 0);
  /* A and X heard the risk frame on the air (they are raw here: it is just decoded) */
  CHECK(a->n_risk >= 1 && x->n_risk >= 1 && a->last_risk.reason == BYS_RISK_OVERLAP && a->last_risk.source == BYS_ROLE_RECEIVER);
  (void)a; (void)x;
}

/* ---- scenario 4: hops: a risk frame is relayed at most twice ---- */
static int u4(void) { return snd_done(&W[1]) && snd_done(&W[2]) && snd_done(&W[3]); }
static void test_air_hops(void) {
  printf("11. air: relay chain, original -> hop 1 -> hop 2 -> not relayed again\n");
  world_reset(0.002f);
  adev_t *src = add_dev("S", 1, 0), *a1 = add_dev("A1", 1, 0), *a2 = add_dev("A2", 1, 0), *a3 = add_dev("A3", 1, 0);
  for (int i = 0; i < MAXDEV; i++) for (int j = 0; j < MAXDEV; j++) GAIN[i][j] = 0.0f;
  GAIN[0][1] = 1; GAIN[1][2] = 1; GAIN[2][3] = 1; /* each one only hears the one before */
  adev_t *senders[3] = { a1, a2, a3 };
  static char texts[3][24] = { "chain text one", "chain text two", "chain text three" };
  for (int i = 0; i < 3; i++) {
    bys_sender_cfg_t sc = scfg(&senders[i]->log, texts[i], BYS_SEC_STANDARD);
    sc.ambient = 0; sc.band = BYS_TOP; sc.security.risk.detector_duration_ms = 8000; sc.timeout_ms = 120000;
    senders[i]->log.rnd = 0;
    start_sender(senders[i], &sc);
  }
  /* a risk frame with hops 0 from a receiver somewhere, looped for 12 s on mid, starting in everybody's first listen window... */
  bys_session_frame_t rf; memset(&rf, 0, sizeof rf);
  rf.kind = BYS_KIND_RISK; rf.band = BYS_MID; rf.reason = BYS_RISK_OTHER; rf.source = BYS_ROLE_RECEIVER; rf.hops = 0; rf.gap_ms = BYS_CONTROL_GAP_MS;
  rf.own[0] = 1; rf.own[1] = 2; rf.own[2] = 3; rf.intruder[0] = 4; rf.intruder[1] = 5; rf.intruder[2] = 6;
  raw_play(src, &rf, 6000, 6000 + 4500);
  world_run(120000, u4);
  CHECK(a1->log.err == BYS_SERR_RISK_SIGNAL && a2->log.err == BYS_SERR_RISK_SIGNAL && a3->log.err == BYS_SERR_RISK_SIGNAL);
  int emitted_hops[3] = { -1, -1, -1 };
  for (int i = 0; i < 3; i++) for (int k = 0; k < senders[i]->log.nalert && k < 16; k++) if (!senders[i]->log.alerts[k].heard) emitted_hops[i] = senders[i]->log.alerts[k].hops;
  CHECK(emitted_hops[0] == 1 && emitted_hops[1] == 2 && emitted_hops[2] == -1);
  CHECK(a1->log.alerts[0].hops == 0 && a2->log.alerts[0].hops == 1 && a3->log.alerts[0].hops == 2);
  CHECK(src->n_risk == 0); /* the source hears nobody */
  /* nobody played a frame with hops 3 */
  for (int i = 0; i < 3; i++) for (int k = 0; k < senders[i]->log.nplay && k < NPLAY; k++) CHECK(senders[i]->log.plays[k].kind != BYS_KIND_RISK || senders[i]->log.plays[k].hops <= 2);
  printf("  A1 relayed hops %d, A2 hops %d, A3 %s\n", emitted_hops[0], emitted_hops[1], emitted_hops[2] < 0 ? "did not relay" : "relayed");
}

/* ---- scenario 5: another sender is already sounding when A starts ---- */
static int u5(void) { return snd_done(&W[0]); }
static void test_air_prelisten(void) {
  printf("12. air: pre-listen hears another transmission: refused, nothing sent; and a sender hears another text while sending\n");
  world_reset(0.002f);
  adev_t *a = add_dev("A", 1, 1), *x = add_dev("X", 1, 0);
  const bys_session_frame_t fx = raw_payload(OTHER, BYS_HIGH);
  raw_play(x, &fx, 0, 4000);
  world_run(300, NULL);
  bys_sender_cfg_t sc = scfg(&a->log, TEXT, BYS_SEC_STANDARD); a->log.rnd = 0;
  start_sender(a, &sc);
  world_run(8000, u5);
  CHECK(a->log.nfail == 1 && a->log.err == BYS_SERR_OTHER_TRANSMISSION && a->log.nplay == 0 && !a->log.fail_has_alert);
  /* second part: while sending, another text arrives in the listen window: OTHER_EMITTER, risk frame with both short ids */
  world_reset(0.002f);
  a = add_dev("A", 1, 0); x = add_dev("X", 1, 0);
  adev_t *l = add_dev("L", 0, 0); /* a listener that decodes everything */
  gain(a, l, 1.0f, 0); gain(x, l, 1.0f, 0);
  sc = scfg(&a->log, TEXT, BYS_SEC_STANDARD); sc.ambient = 0; sc.band = BYS_TOP; a->log.rnd = 0; sc.security.risk.detector_duration_ms = 4000;
  start_sender(a, &sc);
  const bys_session_frame_t fx2 = raw_payload(OTHER, BYS_UPPER);
  raw_play(x, &fx2, 2000 + 1500 + 350 + 300, 2000 + 1500 + 350 + 301);
  world_run(15000, u5);
  CHECK(a->log.nfail == 1 && a->log.err == BYS_SERR_OTHER_EMITTER && a->log.fail_has_alert);
  CHECK(l->n_risk >= 1 && l->last_risk.reason == BYS_RISK_OTHER && l->last_risk.source == BYS_ROLE_SENDER && l->last_risk.hops == 0);
  { uint8_t sh[3], sho[3]; short_of(TEXT, sh); short_of(OTHER, sho); CHECK(!memcmp(l->last_risk.own, sh, 3) && !memcmp(l->last_risk.intruder, sho, 3)); }
}

/* ---- scenario 6: the confirm gate on the air: a different text while watching ---- */
static int u6(void) { return W[1].log.nrecv == 1 && W[1].log.nalert >= 1; }
static void test_air_confirm(void) {
  printf("13. air: a different text after delivery closes the confirm gate (full-duplex receiver)\n");
  world_reset(0.002f);
  adev_t *a = add_dev("A", 1, 0), *r = add_dev("R", 0, 1), *x = add_dev("X", 1, 0);
  gain(a, x, 0.0f, 0);
  bys_receiver_cfg_t rc = rcfg(&r->log, BYS_SEC_STANDARD);
  start_receiver(r, &rc);
  const bys_session_frame_t fa = raw_payload(TEXT, BYS_TOP);
  raw_play(a, &fa, 500, 501);
  world_run(5500, NULL);
  CHECK(r->log.nrecv == 1 && !strcmp(r->log.rtext, TEXT) && r->log.nreceipt == 0);
  const bys_session_frame_t fx = raw_payload(OTHER, BYS_UPPER);
  raw_play(x, &fx, now_ms() + 100, now_ms() + 101);
  world_run(8000, u6);
  CHECK(r->log.rcv.has_conflict && r->log.nalert >= 1 && bys_receiver_confirmed(&r->log.rcv, now_ms(), NULL) == BYS_SERR_MULTIPLE_TRANSMISSIONS);
  CHECK(!r->log.rcv.acking && r->log.nreceipt == 1 && r->log.receipt == 0);
}

/* ---- ambient scale: white noise measures about 20 log10(sigma) - 24 dB (SESSION.md 9.2) ---- */
static void test_noise_scale(void) {
  printf("14. noise report from the demodulator: white noise of sigma 0.003 and 0.01 (SESSION.md: about -74 and -64 dB)\n");
  static const double sig[2] = { 0.003, 0.01 };
  for (int k = 0; k < 2; k++) {
    world_reset(0);
    adev_t *a = add_dev("N", 0, 0);
    noise_sigma = sig[k] * 32768.0;
    world_run(3000, NULL);
    float db[BYS_BAND_COUNT];
    bys_session_noise_from_demod(&a->dn, db);
    const double want = 20 * log10(sig[k]) - 24;
    printf("  sigma %.3f: top %.1f dB, mid %.1f dB (expected about %.1f)\n", sig[k], db[BYS_TOP], db[BYS_MID], want);
    CHECK(fabs(db[BYS_TOP] - want) < 5 && fabs(db[BYS_MID] - want) < 5);
    CHECK(db[BYS_AUDIBLE] != db[BYS_AUDIBLE] && db[BYS_XHIGH] != db[BYS_XHIGH]); /* bands outside the mask: NAN */
  }
}

/* ---- ack and risk frames through the real demodulator: all vectors, no header or clash events for them ---- */
typedef struct { int frames, headers, clashes; bys_result_t res; uint8_t band; } vcap_t;
static void v_cb(void *u, const bys_demod_event_t *ev) {
  vcap_t *c = u;
  if (ev->type == BYS_EV_FRAME) { c->frames++; c->res = *ev->result; c->band = ev->band; }
  else if (ev->type == BYS_EV_HEADER) c->headers++;
  else c->clashes++;
}
static bys_demod_t VD; static uint8_t VOUT[600];
static int16_t VPCM[RATE * 3];
static void test_audio_vectors(void) {
  printf("15. ack, receipt and risk vectors through bys_tx -> bys_demod (frames decoded, no header or clash events)\n");
  int ok_sacks = 0, ok_risk = 0, total = 0;
  for (size_t i = 0; i < V_NSACKS + V_NRISKS; i++) {
    const int is_ack = i < V_NSACKS;
    bys_tx_t tx;
    int band;
    if (is_ack) {
      const vec_sack_t *v = &V_SACKS[i];
      uint8_t data[8]; from_hex(v->data, data);
      band = v->band;
      const unsigned nib = v->heard_band < 0 ? 0u : bys_heard_nibble((bys_band_t)v->heard_band, (bys_profile_t)v->heard_robust);
      if (bys_tx_init_ack_heard(&tx, &TXC, (bys_band_t)band, v->receipt, nib, data + 1) != BYS_OK) { CHECK(0); continue; }
    } else {
      const vec_risk_t *v = &V_RISKS[i - V_NSACKS];
      band = v->band;
      if (bys_tx_init_risk(&tx, &TXC, (bys_band_t)band, (unsigned)v->reason, (unsigned)v->source, (unsigned)v->hops, v->own, v->intruder) != BYS_OK) { CHECK(0); continue; }
    }
    if (band == BYS_AUDIBLE) continue;
    vcap_t cap; memset(&cap, 0, sizeof cap);
    bys_demod_cfg_t cfg; bys_demod_cfg_default(&cfg);
    cfg.sample_rate = RATE; cfg.out = VOUT; cfg.out_cap = sizeof VOUT; cfg.cb = v_cb; cfg.user = &cap; cfg.band_mask = (uint8_t)(1u << band);
    CHECK(bys_demod_init(&VD, &cfg) == BYS_OK);
    memset(VPCM, 0, sizeof VPCM);
    size_t at = RATE / 4, n = at;
    while (!bys_tx_done(&tx) && at < sizeof VPCM / sizeof VPCM[0]) { n = bys_tx_fill(&tx, VPCM + at, 4096 < sizeof VPCM / sizeof VPCM[0] - at ? 4096 : sizeof VPCM / sizeof VPCM[0] - at); at += n; }
    for (size_t k = 0; k < sizeof VPCM / sizeof VPCM[0]; k += 480) bys_demod_push(&VD, VPCM + k, 480);
    total++;
    int good = cap.frames == 1 && cap.headers == 0 && cap.clashes == 0 && cap.band == band;
    if (good && is_ack) {
      const vec_sack_t *v = &V_SACKS[i];
      uint8_t data[8]; from_hex(v->data, data);
      good = cap.res.kind == BYS_KIND_ACK && cap.res.receipt == (unsigned)v->receipt && cap.res.heard == (unsigned)(data[0] >> 4) && !memcmp(cap.res.hash4, data + 1, 4);
      ok_sacks += good;
    } else if (good) {
      const vec_risk_t *v = &V_RISKS[i - V_NSACKS];
      good = cap.res.kind == BYS_KIND_RISK && cap.res.reason == v->reason && cap.res.source == v->source && cap.res.hops == v->hops && !memcmp(cap.res.own, v->own, 3) && !memcmp(cap.res.intruder, v->intruder, 3);
      ok_risk += good;
    }
    if (!good) printf("  vector %zu (%s, band %d) not decoded: frames %d headers %d clashes %d\n", i, is_ack ? "ack" : "risk", band, cap.frames, cap.headers, cap.clashes);
    CHECK(good);
  }
  printf("  %d session ack vectors and %d risk vectors decoded from audio (of %d)\n", ok_sacks, ok_risk, total);
}

int main(void) {
  printf("bysound session layer: host tests\n");
  txc_init();
  test_vectors();
  test_sender_ack();
  test_sender_timeout_stop();
  test_sender_prelisten();
  test_sender_risk();
  test_receiver();
  test_adapt();
  test_audio_vectors();
  test_noise_scale();
  test_air_ack();
  test_air_calibration();
  test_air_second_sender();
  test_air_clash();
  test_air_hops();
  test_air_prelisten();
  test_air_confirm();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
