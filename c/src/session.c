/* bysound session layer (docs/SESSION.md): sender and receiver state machines. See bysound_session.h for the integration model. */
#include "bysound_session.h"
#include <math.h>
#include <string.h>

/* ---------- small helpers ---------- */
static int due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
static uint32_t since(uint32_t now, uint32_t at) { return now - at; }
static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void bys_text_ids(const char *text, size_t n, bys_ids_t *out) {
  uint8_t h[32];
  bys_sha256((const uint8_t *)text, n, h);
  memcpy(out->fingerprint, h, BYS_REF_BYTES);
  memcpy(out->ack, h, 4);
  memcpy(out->short_id, h, 3);
}

unsigned bys_heard_nibble(bys_band_t band, bys_profile_t profile) { return ((profile == BYS_ROBUST) ? 8u : 0u) | ((unsigned)band + 1u); }
int bys_heard_parse(unsigned nibble, bys_band_t *band, bys_profile_t *profile) {
  if (nibble == 0) return 0;
  if (nibble > 15 || (nibble & 7) == 0) return -1;
  if (band) *band = (bys_band_t)((nibble & 7) - 1);
  if (profile) *profile = (nibble & 8) ? BYS_ROBUST : BYS_NORMAL;
  return 1;
}

int bys_bands_overlap(bys_band_t a, bys_band_t b) {
  bys_band_info_t x, y;
  bys_band_info(a, BYS_NORMAL, &x);
  bys_band_info(b, BYS_NORMAL, &y);
  return x.bin < y.bin + y.groups * BYS_TONES && y.bin < x.bin + x.groups * BYS_TONES;
}

unsigned bys_risk_bands(bys_band_t work, int avoid, bys_band_t out[2]) {
  bys_band_t base[2];
  unsigned nb = 0;
  base[nb++] = BYS_MID;
  if (work != BYS_AUDIBLE && work != BYS_MID) base[nb++] = work;
  if (avoid < 0 || avoid == BYS_AUDIBLE) { memcpy(out, base, sizeof base); return nb; }
  unsigned n = 0;
  for (unsigned i = 0; i < nb; i++) if (!bys_bands_overlap(base[i], (bys_band_t)avoid)) out[n++] = base[i];
  if (n) return n;
  const bys_band_t spare[2] = { BYS_MID, BYS_TOP };
  for (unsigned i = 0; i < 2; i++) if (!bys_bands_overlap(spare[i], (bys_band_t)avoid)) out[n++] = spare[i];
  if (n) return n;
  memcpy(out, base, sizeof base);
  return nb;
}

/* ---------- security levels ---------- */
void bys_security_resolve(bys_security_t *o, bys_security_level_t level) {
  memset(o, 0, sizeof *o);
  o->confirm_watch_ms = 120000;
  switch (level) {
    case BYS_SEC_BASIC:
      o->listen_before_ms = 1000; o->scan_jitter_ms = 600;
      o->risk.duration_ms = 4000; o->risk.detector_duration_ms = 4000; o->risk.quarantine_ms = 30000;
      break;
    case BYS_SEC_STANDARD:
      o->detect_multiple_emitters = o->listen_while_sending = o->confirm_watch = 1;
      o->listen_before_ms = 2000; o->scan_jitter_ms = 600;
      o->risk.emit_auto = o->risk.listen = o->risk.honor = o->risk.listen_until_confirm = 1;
      o->risk.duration_ms = 4000; o->risk.detector_duration_ms = 15000; o->risk.quarantine_ms = 30000;
      break;
    default: /* maximum */
      o->detect_multiple_emitters = o->listen_while_sending = o->confirm_watch = 1;
      o->listen_before_ms = 3000; o->scan_jitter_ms = 1200;
      o->risk.emit_auto = o->risk.listen = o->risk.honor = o->risk.listen_until_confirm = 1;
      o->risk.duration_ms = 6000; o->risk.detector_duration_ms = 30000; o->risk.quarantine_ms = 60000;
      break;
  }
}

void bys_risk_memory_init(bys_risk_memory_t *m) { memset(m, 0, sizeof *m); }
static void mem_note(bys_risk_memory_t *m, const uint8_t key[8], uint32_t now) {
  unsigned i = 0;
  for (; i < BYS_RISK_MEMORY_SIZE; i++) if (m->e[i].used && !memcmp(m->e[i].key, key, 8)) break;
  if (i == BYS_RISK_MEMORY_SIZE) { i = m->next; m->next = (uint8_t)((m->next + 1) % BYS_RISK_MEMORY_SIZE); }
  memcpy(m->e[i].key, key, 8); m->e[i].at = now; m->e[i].used = 1;
}
static int mem_is_own(const bys_risk_memory_t *m, const uint8_t key[8], uint32_t now, uint32_t window_ms) {
  for (unsigned i = 0; i < BYS_RISK_MEMORY_SIZE; i++) if (m->e[i].used && !memcmp(m->e[i].key, key, 8)) return since(now, m->e[i].at) < window_ms;
  return 0;
}

/* ---------- frames to play ---------- */
int bys_session_tx_init(bys_tx_t *tx, const bys_tx_cfg_t *cfgin, const bys_session_frame_t *f) {
  bys_tx_cfg_t cfg;
  if (cfgin) cfg = *cfgin; else bys_tx_cfg_default(&cfg);
  cfg.gap_ms = f->gap_ms;
  switch (f->kind) {
    case BYS_KIND_PAYLOAD: return bys_tx_init_payload(tx, &cfg, (bys_band_t)f->band, (bys_profile_t)f->profile, f->text, f->text_len);
    case BYS_KIND_REFERENCE: return bys_tx_init_reference(tx, &cfg, (bys_band_t)f->band, (bys_profile_t)f->profile, f->set, f->fp);
    case BYS_KIND_ACK: return bys_tx_init_ack_heard(tx, &cfg, (bys_band_t)f->band, f->receipt, f->heard, f->hash4);
    case BYS_KIND_RISK: return bys_tx_init_risk(tx, &cfg, (bys_band_t)f->band, f->reason, f->source, f->hops, f->own, f->intruder);
    default: return BYS_E_ARG;
  }
}

static void ack_frame(bys_session_frame_t *f, int receipt, unsigned heard, const uint8_t hash4[4]) {
  memset(f, 0, sizeof *f);
  f->kind = BYS_KIND_ACK; f->band = receipt ? BYS_HIGH : BYS_MID; f->receipt = (uint8_t)receipt; f->heard = (uint8_t)heard;
  memcpy(f->hash4, hash4, 4);
  f->gap_ms = BYS_CONTROL_GAP_MS;
}
static void risk_frame(bys_session_frame_t *f, unsigned reason, unsigned source, unsigned hops, const uint8_t own[3], const uint8_t intruder[3]) {
  memset(f, 0, sizeof *f);
  f->kind = BYS_KIND_RISK; f->reason = (uint8_t)reason; f->source = (uint8_t)source; f->hops = (uint8_t)hops;
  memcpy(f->own, own, 3); memcpy(f->intruder, intruder, 3);
  f->gap_ms = BYS_CONTROL_GAP_MS;
}

/* ---------- shared core: playback, loops, risk memory, seen incidents ---------- */
static void core_init(bys_core_t *c, const bys_session_io *io, const bys_security_t *sec, bys_risk_memory_t *mem, char *scratch, size_t cap, uint32_t now) {
  memset(c, 0, sizeof *c);
  c->io = io; c->sec = *sec;
  bys_risk_memory_init(&c->own_mem);
  c->mem = mem ? mem : &c->own_mem;
  c->scratch = scratch; c->scratch_cap = cap;
  c->rng = now | 1u;
}
static uint32_t core_rand(bys_core_t *c) {
  if (c->io->random) return c->io->random(c->io->user);
  c->rng ^= c->rng << 13; c->rng ^= c->rng >> 17; c->rng ^= c->rng << 5;
  return c->rng;
}
static int core_play(bys_core_t *c, const bys_session_frame_t *f) {
  c->playing = 1;
  int r = c->io->play ? c->io->play(c->io->user, f) : -1;
  if (r < 0) c->playing = 0;
  return r;
}
/* Silences whatever plays and cancels any loop. */
static void core_silence(bys_core_t *c) {
  if (c->playing && c->io->stop) c->io->stop(c->io->user);
  c->playing = 0;
  c->loop.on = 0;
}
static int loop_play(bys_core_t *c) {
  bys_session_frame_t f = c->loop.proto;
  f.band = c->loop.bands[c->loop.idx % c->loop.n];
  c->loop.idx++;
  return core_play(c, &f);
}
/* Loops `proto` back to back on `bands` (alternating), whole frames only, for at least dur_ms. */
static int loop_start(bys_core_t *c, const bys_session_frame_t *proto, const bys_band_t *bands, unsigned n, uint32_t dur_ms, uint32_t now) {
  c->loop.on = 1; c->loop.idx = 0; c->loop.n = (uint8_t)n; c->loop.start = now; c->loop.dur = dur_ms; c->loop.proto = *proto;
  for (unsigned i = 0; i < n; i++) c->loop.bands[i] = (uint8_t)bands[i];
  int r = loop_play(c);
  if (r < 0) c->loop.on = 0;
  return r;
}
/* The current frame ended. 1: the loop went on with the next frame; 0: not a loop, or it is over (loop.on is then 0). */
static int loop_done(bys_core_t *c, uint32_t now) {
  c->playing = 0;
  if (!c->loop.on) return 0;
  if (since(now, c->loop.start) < c->loop.dur && loop_play(c) >= 0) return 1;
  c->loop.on = 0;
  return 0;
}

static void risk_key(uint8_t key[8], unsigned reason, unsigned source, const uint8_t own[3], const uint8_t intruder[3]) {
  key[0] = (uint8_t)reason; key[1] = (uint8_t)source; memcpy(key + 2, own, 3); memcpy(key + 5, intruder, 3);
}
static int seen_has(const bys_core_t *c, const uint8_t key[8]) {
  for (unsigned i = 0; i < c->nseen; i++) if (!memcmp(c->seen[i], key, 8)) return 1;
  return 0;
}
static void seen_add(bys_core_t *c, const uint8_t key[8]) {
  if (c->nseen < BYS_SESSION_SEEN) memcpy(c->seen[c->nseen++], key, 8);
}
static void quarantine(bys_core_t *c, unsigned reason, unsigned source) {
  if (c->sec.risk.quarantine_ms > 0 && c->io->on_quarantine) c->io->on_quarantine(c->io->user, c->sec.risk.quarantine_ms, (uint8_t)reason, (uint8_t)source);
}
static void notify_alert(bys_core_t *c, const bys_alert_t *a) { if (c->io->on_alert) c->io->on_alert(c->io->user, a); }

/* ---------- what a frame carries ---------- */
static int content_of(const bys_demod_event_t *ev, bys_content_t *c) {
  const bys_result_t *r = ev->result;
  memset(c, 0, sizeof *c);
  if (!r) return 0;
  c->kind = r->kind;
  if (r->kind == BYS_KIND_PAYLOAD) {
    if (!ev->payload) return 0;
    c->len = (uint16_t)ev->payload_len;
    c->crc = bys_crc32(ev->payload, ev->payload_len);
    return 1;
  }
  if (r->kind == BYS_KIND_REFERENCE) { memcpy(c->fp, r->fingerprint, BYS_REF_BYTES); return 1; }
  return 0;
}
static int content_same(const bys_content_t *a, const bys_content_t *b) {
  if (a->kind != b->kind) return 0;
  return a->kind == BYS_KIND_PAYLOAD ? a->len == b->len && a->crc == b->crc : !memcmp(a->fp, b->fp, BYS_REF_BYTES);
}
/* Short id of the text of a heard frame: needs the scratch buffer for payloads, else 000000. */
static void content_short(const bys_core_t *c, const bys_demod_event_t *ev, uint8_t out[3]) {
  memset(out, 0, 3);
  if (ev->result->kind == BYS_KIND_REFERENCE) { memcpy(out, ev->result->fingerprint, 3); return; }
  if (c->scratch && c->scratch_cap) {
    int n = bys_unpack(ev->payload, ev->payload_len, c->scratch, c->scratch_cap);
    if (n >= 0) { uint8_t h[32]; bys_sha256((const uint8_t *)c->scratch, (size_t)n, h); memcpy(out, h, 3); }
  }
}

/* Longest repetition (frame + gap) on the default ladder: top, robust (SESSION.md 6.4). */
static uint32_t longest_rep_ms(unsigned kind, uint32_t packed_len) {
  uint32_t us = 0;
  if (bys_frame_duration_us((bys_kind_t)kind, BYS_TOP, BYS_ROBUST, packed_len, &us) < 0) return BYS_ACK_RUN_MAX_MS;
  return us / 1000u + BYS_GAP_MS;
}
static uint32_t ack_run_ms(uint32_t longest_ms) {
  return clampu(longest_ms + 2 * BYS_LISTEN_WINDOW_MS + BYS_ACK_MARGIN_MS, BYS_ACK_RUN_MIN_MS, BYS_ACK_RUN_MAX_MS);
}

/* ======================================================================================================================== */
/* Sender                                                                                                                   */
/* ======================================================================================================================== */
static const uint8_t DEFAULT_CHANNELS[4] = { BYS_TOP, BYS_HIGH, BYS_UPPER, BYS_MID };

void bys_sender_cfg_default(bys_sender_cfg_t *c, const char *text, size_t n) {
  memset(c, 0, sizeof *c);
  c->text = text; c->text_len = n; c->band = -1; c->profile = -1; c->timeout_ms = 60000; c->ambient = 1;
  bys_security_resolve(&c->security, BYS_SEC_MAXIMUM);
}

static void s_set_state(bys_sender_t *s, int st) {
  if (s->state == st) return;
  s->state = (uint8_t)st;
  if (s->core.io->on_state) s->core.io->on_state(s->core.io->user, st);
}
static void s_end(bys_sender_t *s) {
  core_silence(&s->core);
  s_set_state(s, BYS_SS_DONE);
  if (s->core.io->on_finished) s->core.io->on_finished(s->core.io->user);
}
static void s_fail(bys_sender_t *s, bys_session_error_t code, const bys_alert_t *alert) {
  if (!s->pending) return;
  s->pending = 0;
  if (s->core.io->on_failed) s->core.io->on_failed(s->core.io->user, code, alert);
}
static void s_refuse(bys_sender_t *s, bys_session_error_t code) { s_fail(s, code, NULL); s_end(s); }

static unsigned chain_len(const bys_sender_t *s) { return s->cfg.nchannels ? s->cfg.nchannels : 4; }
static const uint8_t *chain_of(const bys_sender_t *s) { return s->cfg.nchannels ? s->cfg.channels : DEFAULT_CHANNELS; }
static int chain_index(const bys_sender_t *s, unsigned band) {
  for (unsigned i = 0; i < chain_len(s); i++) if (chain_of(s)[i] == band) return (int)i;
  return -1;
}

static void s_begin(bys_sender_t *s, uint32_t now);

/* Packed length of the text. */
static uint32_t packed_len(const char *text, size_t n) {
  bys_packer_t p;
  uint32_t len = 0;
  bys_packer_init(&p, text, n);
  while (bys_packer_next(&p) >= 0) len++;
  return len;
}

/* The text fits one transmission on every band the session may play (the fixed band, or every channel of the chain), in the normal
 * profile or in the fixed one. Checked up front so a later repetition never fails to render. */
static int text_fits(const bys_sender_cfg_t *cfg) {
  const uint32_t len = packed_len(cfg->text, cfg->text_len);
  const unsigned n = cfg->band >= 0 ? 1u : (cfg->nchannels ? cfg->nchannels : 4u);
  for (unsigned i = 0; i < n; i++) {
    const unsigned b = cfg->band >= 0 ? (unsigned)cfg->band : (cfg->nchannels ? cfg->channels[i] : DEFAULT_CHANNELS[i]);
    const bys_profile_t p = (cfg->profile == BYS_ROBUST && b <= BYS_MID) ? BYS_ROBUST : BYS_NORMAL;
    const int max = bys_max_payload_bytes((bys_band_t)b, p);
    if (max < 0 || len > (uint32_t)max) return 0;
  }
  return 1;
}

int bys_sender_start(bys_sender_t *s, const bys_sender_cfg_t *cfg, const bys_session_io *io, uint32_t now) {
  if (!s || !cfg || !io || !cfg->text || cfg->text_len == 0 || cfg->nchannels > BYS_SESSION_MAX_CHANNELS) return BYS_E_ARG;
  if (cfg->band < -1 || cfg->band >= (int)BYS_BAND_COUNT || cfg->profile < -1 || cfg->profile > BYS_ROBUST) return BYS_E_ARG;
  for (unsigned i = 0; i < cfg->nchannels; i++) if (cfg->channels[i] >= BYS_BAND_COUNT) return BYS_E_ARG;
  if (!cfg->reference && !text_fits(cfg)) return BYS_E_TOO_LONG;
  memset(s, 0, sizeof *s);
  s->cfg = *cfg;
  core_init(&s->core, io, &cfg->security, cfg->memory, cfg->scratch, cfg->scratch_cap, now);
  bys_text_ids(cfg->text, cfg->text_len, &s->ids);
  /* own content: packed length and CRC-32 */
  {
    bys_packer_t p;
    uint32_t crc = 0xffffffffu, len = 0;
    int b;
    bys_packer_init(&p, cfg->text, cfg->text_len);
    while ((b = bys_packer_next(&p)) >= 0) { crc = bys_crc32_update(crc, (uint8_t)b); len++; }
    s->own.kind = BYS_KIND_PAYLOAD; s->own.len = (uint16_t)len; s->own.crc = crc ^ 0xffffffffu;
  }
  s->pending = 1;
  s->band = cfg->band >= 0 ? (uint8_t)cfg->band : chain_of(s)[0];
  s->last_band = 0xff;
  const bys_security_t *sec = &s->core.sec;
  const int detect = sec->detect_multiple_emitters;
  s->state = BYS_SS_PRE_LISTEN;
  if (sec->listen_before_ms > 0 && (detect || cfg->ambient)) {
    const uint32_t jitter = (detect && sec->scan_jitter_ms) ? core_rand(&s->core) % sec->scan_jitter_ms : 0;
    s->pre_end = now + sec->listen_before_ms + jitter;
    if (io->on_state) io->on_state(io->user, BYS_SS_PRE_LISTEN);
  } else s_begin(s, now);
  return BYS_OK;
}

bys_sender_state_t bys_sender_state(const bys_sender_t *s) { return (bys_sender_state_t)s->state; }

void bys_sender_noise(bys_sender_t *s, uint32_t now, const float db[BYS_BAND_COUNT]) {
  (void)now;
  if (s->state != BYS_SS_PRE_LISTEN) return;
  for (unsigned i = 0; i < chain_len(s); i++) {
    const float v = db[chain_of(s)[i]];
    if (v == v && s->nreports[i] < BYS_SESSION_MAX_REPORTS) s->reports[i][s->nreports[i]++] = v;
  }
}

/* ---- ambient check (SESSION.md 9.2) ---- */
static void read_ambient(bys_sender_t *s) {
  bys_ambient_t *a = &s->ambient;
  memset(a, 0, sizeof *a);
  for (unsigned b = 0; b < BYS_BAND_COUNT; b++) a->noise_db[b] = NAN;
  float quietest = 0;
  int any = 0;
  for (unsigned i = 0; i < chain_len(s); i++) {
    unsigned n = s->nreports[i];
    if (!n) continue;
    float *v = s->reports[i];
    for (unsigned x = 1; x < n; x++) { float k = v[x]; unsigned y = x; while (y && v[y - 1] > k) { v[y] = v[y - 1]; y--; } v[y] = k; }
    a->noise_db[chain_of(s)[i]] = v[n >> 1];
    if (!any || v[n >> 1] < quietest) quietest = v[n >> 1];
    any = 1;
  }
  if (!any) return;
  a->present = 1;
  for (unsigned i = 0; i < chain_len(s); i++) {
    const float n = a->noise_db[chain_of(s)[i]];
    if (n == n && n > quietest + BYS_NOISY_CHANNEL_DB) a->noisy_mask |= (uint8_t)(1u << chain_of(s)[i]);
  }
  a->loud = quietest >= BYS_LOUD_PLACE_DB;
}

/* ---- plan (SESSION.md 9.1, 9.4) ---- */
static void make_plan(bys_sender_t *s) {
  const bys_sender_cfg_t *o = &s->cfg;
  s->start_robust = 0; s->cal_valid = 0;
  if (o->band >= 0) { s->route[0] = (uint8_t)o->band; s->route_len = 1; s->every = 0xffffffffu; return; }
  const unsigned n = chain_len(s);
  const uint8_t *chain = chain_of(s);
  bys_calibration_t cal;
  if (s->core.io->cal_load && s->core.io->cal_load(s->core.io->user, &cal) && cal.band < BYS_BAND_COUNT && cal.band != BYS_AUDIBLE && cal.uses < BYS_CALIBRATION_USES && chain_index(s, cal.band) >= 0) {
    s->cal = cal; s->cal_valid = 1;
  }
  const unsigned start = s->cal_valid ? (unsigned)chain_index(s, s->cal.band) : 0;
  unsigned rl = 0;
  uint8_t route[BYS_SESSION_MAX_CHANNELS];
  for (unsigned i = start; i < n; i++) route[rl++] = chain[i];
  for (unsigned i = start; i-- > 0;) route[rl++] = chain[i];
  const bys_ambient_t *a = &s->ambient;
  if (a->present) {
    uint8_t kept[BYS_SESSION_MAX_CHANNELS];
    unsigned kl = 0;
    for (unsigned i = 0; i < rl; i++) if (!(a->noisy_mask >> route[i] & 1) || (s->cal_valid && route[i] == s->cal.band)) kept[kl++] = route[i];
    if (kl) { memcpy(route, kept, kl); rl = kl; }
    else { /* the single quietest channel of the chain (no report counts as 0 dB, like the reference) */
      unsigned best = 0;
      float bv = 0;
      for (unsigned i = 0; i < n; i++) { float v = a->noise_db[chain[i]]; if (v != v) v = 0; if (i == 0 || v < bv) { bv = v; best = i; } }
      route[0] = chain[best]; rl = 1;
    }
  }
  memcpy(s->route, route, rl); s->route_len = (uint8_t)rl;
  if (o->profile < 0) {
    if (s->cal_valid && s->cal.profile == BYS_ROBUST) s->start_robust = 1;
    else if (a->present && a->noise_db[s->route[0]] == a->noise_db[s->route[0]] && a->noise_db[s->route[0]] >= BYS_ROBUST_NOISE_DB) s->start_robust = 2;
  }
  s->every = BYS_HOP_REPETITIONS * (s->cal_valid ? BYS_CALIBRATED_HOP_FACTOR : 1u);
}

static unsigned ladder_next(bys_sender_t *s, unsigned band) {
  if (!s->ladder_robust && s->ladder_auto && s->ladder_normal >= BYS_ROBUST_AFTER_REPETITIONS) { s->ladder_robust = 1; s->ladder_reason = 2; }
  const unsigned p = (s->ladder_robust && band <= BYS_MID) ? BYS_ROBUST : BYS_NORMAL;
  if (p == BYS_NORMAL) s->ladder_normal++;
  return p;
}

static void s_play_repetition(bys_sender_t *s, uint32_t now) {
  if (s->on_channel >= s->every && s->route_len > 1) { s->on_channel = 0; s->hop = (uint8_t)((s->hop + 1) % s->route_len); }
  s->on_channel++;
  const unsigned band = s->route[s->hop];
  unsigned profile = ladder_next(s, band);
  /* A text that fits the normal profile may not fit the robust one (twice the airtime): the normal profile is played then. */
  if (profile == BYS_ROBUST && !s->cfg.reference && (int)s->own.len > bys_max_payload_bytes((bys_band_t)band, BYS_ROBUST)) profile = BYS_NORMAL;
  s->band = (uint8_t)band; s->profile = (uint8_t)profile;
  if (band != s->last_band) { s->changes++; s->last_band = (uint8_t)band; }
  if (s->core.io->on_repetition) s->core.io->on_repetition(s->core.io->user, s->sent, (bys_band_t)band, (bys_profile_t)profile);
  bys_session_frame_t f;
  memset(&f, 0, sizeof f);
  f.band = (uint8_t)band; f.profile = (uint8_t)profile; f.gap_ms = BYS_GAP_MS;
  if (s->cfg.reference) { f.kind = BYS_KIND_REFERENCE; f.set = (uint8_t)(s->sent % 2); memcpy(f.fp, s->ids.fingerprint, BYS_REF_BYTES); }
  else { f.kind = BYS_KIND_PAYLOAD; f.text = s->cfg.text; f.text_len = s->cfg.text_len; }
  s->sent++;
  s_set_state(s, BYS_SS_SENDING);
  if (core_play(&s->core, &f) < 0) { s_fail(s, BYS_SERR_PLAY, NULL); s_end(s); }
  (void)now;
}

static void s_begin(bys_sender_t *s, uint32_t now) {
  const bys_sender_cfg_t *o = &s->cfg;
  memset(&s->ambient, 0, sizeof s->ambient);
  for (unsigned b = 0; b < BYS_BAND_COUNT; b++) s->ambient.noise_db[b] = NAN;
  if (o->ambient) read_ambient(s);
  make_plan(s);
  if (s->cal_valid && s->core.io->cal_save) { bys_calibration_t c = s->cal; c.uses++; s->core.io->cal_save(s->core.io->user, &c); }
  s->ladder_auto = o->profile < 0;
  s->ladder_robust = (s->start_robust != 0 || o->profile == BYS_ROBUST);
  s->ladder_reason = s->start_robust == 2 ? 1 : 0;
  s->ladder_normal = 0;
  if (s->core.io->on_ambient) s->core.io->on_ambient(s->core.io->user, &s->ambient, s->route, s->route_len);
  s->first_at = now;
  s_play_repetition(s, now);
}

static void s_silence_data(bys_sender_t *s) { if (s->core.playing && s->core.io->stop) s->core.io->stop(s->core.io->user); s->core.playing = 0; }

/* ---- risk frames (SESSION.md 5.6) ---- */
static void s_emit_risk(bys_sender_t *s, uint32_t now, unsigned reason, unsigned source, unsigned hops, const uint8_t own[3], const uint8_t intruder[3], int avoid, int relay, bys_alert_t *alert) {
  bys_core_t *c = &s->core;
  core_silence(c); /* stops receipts and the payload */
  uint8_t key[8];
  risk_key(key, reason, source, own, intruder);
  mem_note(c->mem, key, now);
  bys_band_t bands[2];
  const unsigned nb = bys_risk_bands((bys_band_t)s->band, avoid, bands);
  memset(alert, 0, sizeof *alert);
  alert->heard = 0; alert->reason = (uint8_t)reason; alert->source = (uint8_t)source; alert->hops = (uint8_t)hops;
  memcpy(alert->own, own, 3); memcpy(alert->intruder, intruder, 3);
  for (unsigned i = 0; i < nb; i++) alert->bands[i] = (uint8_t)bands[i];
  alert->nbands = (uint8_t)nb; alert->matches_mine = 1; alert->relay = (uint8_t)relay;
  notify_alert(c, alert);
  if (!relay) quarantine(c, reason, source);
  s_set_state(s, BYS_SS_ALERTING);
  bys_session_frame_t f;
  risk_frame(&f, reason, source, hops, own, intruder);
  if (loop_start(c, &f, bands, nb, c->sec.risk.detector_duration_ms, now) < 0) s_end(s);
}

/* Another emitter while sending: stop, and (emit auto) loop a risk frame so the receiver and the others stop too. */
static void s_detected(bys_sender_t *s, uint32_t now, unsigned reason, const uint8_t intruder[3], int avoid) {
  s_silence_data(s);
  const bys_session_error_t code = reason == BYS_RISK_CLONE ? BYS_SERR_CLONED_TRANSMISSION : BYS_SERR_OTHER_EMITTER;
  if (!s->core.sec.risk.emit_auto) { s_fail(s, code, NULL); s_end(s); return; }
  bys_alert_t alert;
  s_emit_risk(s, now, reason, BYS_ROLE_SENDER, 0, s->ids.short_id, intruder, avoid, 0, &alert);
  s_fail(s, code, &alert);
}

static void s_heard_risk(bys_sender_t *s, uint32_t now, const bys_result_t *r, unsigned band) {
  bys_core_t *c = &s->core;
  const bys_risk_signal_t *cfg = &c->sec.risk;
  uint8_t key[8];
  risk_key(key, r->reason, r->source, r->own, r->intruder);
  if (!cfg->listen || s->state == BYS_SS_ALERTING || seen_has(c, key) || mem_is_own(c->mem, key, now, cfg->quarantine_ms)) return;
  seen_add(c, key);
  bys_alert_t alert;
  memset(&alert, 0, sizeof alert);
  alert.heard = 1; alert.reason = r->reason; alert.source = r->source; alert.hops = r->hops;
  memcpy(alert.own, r->own, 3); memcpy(alert.intruder, r->intruder, 3);
  alert.bands[0] = (uint8_t)band; alert.nbands = 1; alert.matches_mine = !memcmp(r->own, s->ids.short_id, 3);
  notify_alert(c, &alert);
  quarantine(c, r->reason, r->source);
  if (!cfg->honor) return;
  s_silence_data(s);
  /* Any honored risk frame stops this sender. It is relayed once, one hop further. */
  if (r->hops < BYS_RISK_MAX_HOPS) { bys_alert_t out; s_emit_risk(s, now, r->reason, r->source, (unsigned)r->hops + 1, r->own, r->intruder, (int)band, 1, &out); }
  else s_end(s);
  s_fail(s, BYS_SERR_RISK_SIGNAL, &alert);
}

/* ---- ack and calibration ---- */
static int s_lapped(const bys_sender_t *s) { return s->route_len >= 2 && s->changes > s->route_len; }

static int s_learn(bys_sender_t *s, int stated, bys_band_t hband, bys_profile_t hprofile, bys_calibration_t *saved) {
  if (!s->core.io->cal_save || s->cfg.band >= 0 || !stated || hband == BYS_AUDIBLE) return 0;
  if (s_lapped(s)) return 0;
  const bys_ambient_t *a = &s->ambient;
  if (s->cal_valid && hband != s->cal.band && a->present && (a->loud || (a->noisy_mask >> s->cal.band & 1))) return 0;
  saved->band = (uint8_t)hband;
  saved->profile = (uint8_t)((hprofile == BYS_ROBUST && s->ladder_reason != 0) ? BYS_NORMAL : hprofile);
  saved->uses = 0;
  s->core.io->cal_save(s->core.io->user, saved);
  return 1;
}

static void s_acked(bys_sender_t *s, uint32_t now, unsigned heard_nibble) {
  s_silence_data(s);
  s->pending = 0;
  bys_band_t hb = (bys_band_t)s->band;
  bys_profile_t hp = (bys_profile_t)s->profile;
  const int stated = bys_heard_parse(heard_nibble, &hb, &hp) == 1;
  if (!stated) { hb = (bys_band_t)s->band; hp = (bys_profile_t)s->profile; }
  bys_delivered_t d;
  memset(&d, 0, sizeof d);
  d.band = (uint8_t)hb; d.profile = (uint8_t)hp; d.stated = (uint8_t)stated; d.ms = since(now, s->first_at); d.repetitions = s->sent;
  d.saved = (uint8_t)s_learn(s, stated, hb, hp, &d.calibration);
  if (s->core.io->on_delivered) s->core.io->on_delivered(s->core.io->user, &d);
  s_set_state(s, BYS_SS_RECEIPT);
  bys_session_frame_t f;
  ack_frame(&f, 1, stated ? heard_nibble : 0, s->ids.ack);
  const uint32_t longest = s->cfg.reference ? longest_rep_ms(BYS_KIND_REFERENCE, 0) : longest_rep_ms(BYS_KIND_PAYLOAD, s->own.len);
  const bys_band_t hi = BYS_HIGH;
  if (loop_start(&s->core, &f, &hi, 1, ack_run_ms(longest) + BYS_RECEIPT_LISTEN_MS, now) < 0) s_end(s);
}

void bys_sender_hear(bys_sender_t *s, uint32_t now, const bys_demod_event_t *ev) {
  if (s->state == BYS_SS_DONE || !ev) return;
  const bys_security_t *sec = &s->core.sec;
  if (ev->type == BYS_EV_FRAME && ev->result) {
    const bys_result_t *r = ev->result;
    if (r->kind == BYS_KIND_RISK) { s_heard_risk(s, now, r, ev->band); return; }
    if (r->kind == BYS_KIND_ACK) {
      if ((s->state == BYS_SS_SENDING || s->state == BYS_SS_WINDOW) && !r->receipt && !memcmp(r->hash4, s->ids.ack, 4)) s_acked(s, now, r->heard);
      return;
    }
  }
  if (!sec->detect_multiple_emitters) return;
  if (s->state == BYS_SS_PRE_LISTEN) {
    /* A decoded frame refuses at once; a frame start or a clash refuses at the end of the pre-listen. */
    if (ev->type != BYS_EV_FRAME) { s->busy = 1; return; }
    bys_content_t c;
    if (!content_of(ev, &c)) return;
    int own = c.kind == BYS_KIND_PAYLOAD ? content_same(&c, &s->own) : !memcmp(c.fp, s->ids.fingerprint, BYS_REF_BYTES);
    s_refuse(s, own ? BYS_SERR_CLONED_TRANSMISSION : BYS_SERR_OTHER_TRANSMISSION);
    return;
  }
  if ((s->state != BYS_SS_SENDING && s->state != BYS_SS_WINDOW) || !sec->listen_while_sending || ev->type != BYS_EV_FRAME || !ev->result) return;
  bys_content_t c;
  if (!content_of(ev, &c)) return;
  const int own = c.kind == BYS_KIND_PAYLOAD ? content_same(&c, &s->own) : !memcmp(c.fp, s->ids.fingerprint, BYS_REF_BYTES);
  if (!own) {
    uint8_t sh[3];
    content_short(&s->core, ev, sh);
    s_detected(s, now, BYS_RISK_OTHER, sh, ev->band);
    return;
  }
  /* Our own data while we have been silent for a while, or on a band we are not playing: someone else is playing it. */
  const int silent = !s->core.playing && (!s->have_last_end || since(now, s->last_end) >= BYS_CLONE_GUARD_MS);
  if (silent || (s->core.playing && ev->band != s->band)) s_detected(s, now, BYS_RISK_CLONE, s->ids.short_id, ev->band);
}

void bys_sender_tick(bys_sender_t *s, uint32_t now) {
  switch (s->state) {
    case BYS_SS_PRE_LISTEN:
      if (due(now, s->pre_end)) { if (s->busy) s_refuse(s, BYS_SERR_OTHER_TRANSMISSION); else s_begin(s, now); }
      break;
    case BYS_SS_SENDING:
    case BYS_SS_WINDOW:
      if (since(now, s->first_at) >= s->cfg.timeout_ms) { s_fail(s, BYS_SERR_TIMEOUT, NULL); s_end(s); break; }
      if (s->state == BYS_SS_WINDOW && due(now, s->window_end)) s_play_repetition(s, now);
      break;
    default: break;
  }
}

void bys_sender_play_done(bys_sender_t *s, uint32_t now) {
  if (s->state == BYS_SS_DONE) return;
  if (loop_done(&s->core, now)) return;
  if (s->state == BYS_SS_RECEIPT || s->state == BYS_SS_ALERTING) { s_end(s); return; }
  if (s->state == BYS_SS_SENDING) {
    s->last_end = now; s->have_last_end = 1;
    s->window_end = now + BYS_LISTEN_WINDOW_MS;
    s_set_state(s, BYS_SS_WINDOW);
  }
}

void bys_sender_stop(bys_sender_t *s, uint32_t now) {
  (void)now;
  if (s->state == BYS_SS_DONE) return;
  s_fail(s, BYS_SERR_STOPPED, NULL);
  s_end(s);
}

/* ======================================================================================================================== */
/* Receiver                                                                                                                 */
/* ======================================================================================================================== */
void bys_receiver_cfg_default(bys_receiver_cfg_t *c, char *text, size_t cap) {
  memset(c, 0, sizeof *c);
  c->text = text; c->text_cap = cap; c->timeout_ms = 60000; c->ack = 1;
  bys_security_resolve(&c->security, BYS_SEC_MAXIMUM);
}

static void r_set_state(bys_receiver_t *r, int st) {
  if (r->state == st) return;
  r->state = (uint8_t)st;
  if (r->core.io->on_state) r->core.io->on_state(r->core.io->user, st);
}
static void r_settle_receipt(bys_receiver_t *r, int heard) {
  if (r->receipt_settled) return;
  r->receipt_settled = 1;
  if (r->core.io->on_receipt) r->core.io->on_receipt(r->core.io->user, heard);
}
static void r_maybe_finished(bys_receiver_t *r) {
  const int listening = r->state == BYS_RS_LISTENING || r->state == BYS_RS_FINAL_LISTEN || r->watching || r->acking;
  if (!listening && !r->alerting && !r->finished) {
    core_silence(&r->core);
    r->finished = 1;
    if (r->core.io->on_finished) r->core.io->on_finished(r->core.io->user);
  }
}
static void r_stop_watch(bys_receiver_t *r, uint32_t now) {
  if (!r->watching) return;
  r->watching = 0;
  r->watch_end = now;
}
static void r_end_acks(bys_receiver_t *r, int receipt) {
  if (!r->acking) { if (!receipt) r_settle_receipt(r, 0); return; }
  r->acking = 0;
  core_silence(&r->core);
  r_settle_receipt(r, receipt);
  r_maybe_finished(r);
}

static void r_emit_risk(bys_receiver_t *r, uint32_t now, unsigned reason, const uint8_t intruder[3], int avoid, bys_alert_t *alert) {
  bys_core_t *c = &r->core;
  uint8_t own[3] = { 0, 0, 0 };
  if (r->have_first) memcpy(own, r->first_short, 3);
  uint8_t key[8];
  risk_key(key, reason, BYS_ROLE_RECEIVER, own, intruder);
  mem_note(c->mem, key, now);
  bys_band_t bands[2];
  const unsigned nb = bys_risk_bands(r->have_first ? (bys_band_t)r->first_band : (avoid >= 0 ? (bys_band_t)avoid : BYS_TOP), avoid, bands);
  memset(alert, 0, sizeof *alert);
  alert->reason = (uint8_t)reason; alert->source = BYS_ROLE_RECEIVER;
  memcpy(alert->own, own, 3); memcpy(alert->intruder, intruder, 3);
  for (unsigned i = 0; i < nb; i++) alert->bands[i] = (uint8_t)bands[i];
  alert->nbands = (uint8_t)nb; alert->matches_mine = 1;
  notify_alert(c, alert);
  quarantine(c, reason, BYS_ROLE_RECEIVER);
  r->alerting = 1;
  bys_session_frame_t f;
  risk_frame(&f, reason, BYS_ROLE_RECEIVER, 0, own, intruder);
  if (loop_start(c, &f, bands, nb, c->sec.risk.duration_ms, now) < 0) r->alerting = 0;
}

/* Before delivery: the result fails. reason != 0 also loops a risk frame (emit auto). */
static void r_fail(bys_receiver_t *r, uint32_t now, bys_session_error_t code, unsigned reason, const uint8_t intruder[3], int avoid, const bys_alert_t *heard) {
  if (r->state != BYS_RS_LISTENING && r->state != BYS_RS_FINAL_LISTEN) return;
  core_silence(&r->core);
  bys_alert_t emitted;
  const bys_alert_t *alert = heard;
  if (reason && r->core.sec.risk.emit_auto) { r_emit_risk(r, now, reason, intruder, avoid, &emitted); alert = &emitted; }
  r_set_state(r, BYS_RS_FAILED);
  r->fail_err = (uint8_t)code;
  if (r->core.io->on_failed) r->core.io->on_failed(r->core.io->user, code, alert);
  r_settle_receipt(r, 0);
  r_maybe_finished(r);
}

/* Something showed up during the confirm watch: the gate closes for good. A different transmission also gets a risk frame. */
static void r_conflict(bys_receiver_t *r, uint32_t now, bys_session_error_t code, unsigned reason, const uint8_t intruder[3], int avoid, const bys_alert_t *heard) {
  r_stop_watch(r, now);
  if (r->acking) { r->acking = 0; core_silence(&r->core); r_settle_receipt(r, 0); }
  bys_alert_t emitted;
  const bys_alert_t *alert = heard;
  if (reason && r->core.sec.risk.emit_auto) { r_emit_risk(r, now, reason, intruder, avoid, &emitted); alert = &emitted; }
  r->has_conflict = 1; r->conflict_err = code;
  if (alert) { r->conflict_alert = *alert; r->conflict_has_alert = 1; }
  r_maybe_finished(r);
}

static void r_ack_round(bys_receiver_t *r, uint32_t now) {
  if (!r->acking) return;
  if (due(now, r->ack_until)) { r_end_acks(r, 0); return; }
  r->ack_phase = 0;
  bys_session_frame_t f;
  ack_frame(&f, 0, bys_heard_nibble((bys_band_t)r->first_band, (bys_profile_t)r->first_profile), r->ids.ack);
  const bys_band_t mid = BYS_MID;
  if (loop_start(&r->core, &f, &mid, 1, r->ack_run_ms, now) < 0) r_end_acks(r, 0);
}

static void r_deliver(bys_receiver_t *r, uint32_t now) {
  if ((r->state != BYS_RS_LISTENING && r->state != BYS_RS_FINAL_LISTEN) || !r->has_text) return;
  r_set_state(r, BYS_RS_DELIVERED);
  r->delivered = 1;
  if (r->core.io->on_received) r->core.io->on_received(r->core.io->user, r->cfg.text, r->text_len, (bys_band_t)r->first_band, r->first_reference);
  const bys_security_t *sec = &r->core.sec;
  if (sec->confirm_watch) { r->watching = 1; r->watch_start = now; r->watch_end_at = now + sec->confirm_watch_ms; }
  if (r->cfg.ack) {
    r->acking = 1;
    r->ack_until = now + BYS_ACK_TOTAL_MS;
    r->ack_run_ms = ack_run_ms(r->first_reference ? longest_rep_ms(BYS_KIND_REFERENCE, 0) : longest_rep_ms(BYS_KIND_PAYLOAD, r->first.len));
    r_ack_round(r, now);
  } else r_settle_receipt(r, 0);
  r_maybe_finished(r);
}

int bys_receiver_start(bys_receiver_t *r, const bys_receiver_cfg_t *cfg, const bys_session_io *io, uint32_t now) {
  if (!r || !cfg || !io || !cfg->text || cfg->text_cap < 2) return BYS_E_ARG;
  memset(r, 0, sizeof *r);
  r->cfg = *cfg;
  core_init(&r->core, io, &cfg->security, cfg->memory, cfg->scratch, cfg->scratch_cap, now);
  r->state = BYS_RS_LISTENING;
  r->start_at = now;
  r->cfg.text[0] = 0;
  return BYS_OK;
}
bys_receiver_state_t bys_receiver_state(const bys_receiver_t *r) { return (bys_receiver_state_t)r->state; }
const uint8_t *bys_receiver_short_id(const bys_receiver_t *r) { return r->has_text ? r->first_short : NULL; }

static void r_heard_frame(bys_receiver_t *r, uint32_t now, const bys_demod_event_t *ev) {
  const bys_security_t *sec = &r->core.sec;
  bys_content_t c;
  if (!content_of(ev, &c)) return;
  if (r->state == BYS_RS_LISTENING) {
    r->first = c; r->have_first = 1; r->first_at = now; r->first_band = ev->band; r->first_profile = ev->profile;
    r->first_reference = c.kind == BYS_KIND_REFERENCE;
    if (!r->first_reference) {
      int n = bys_unpack(ev->payload, ev->payload_len, r->cfg.text, r->cfg.text_cap);
      if (n == BYS_E_FORMAT) { r->have_first = 0; return; } /* CRC-32 passed but the bytes are not a valid packing: like the reference, not delivered */
      if (n < 0 || (size_t)n >= r->cfg.text_cap) { r_fail(r, now, BYS_SERR_NO_SPACE, 0, NULL, -1, NULL); return; } /* room for the NUL too */
      r->text_len = (uint32_t)n;
    } else {
      if (!r->cfg.resolve) { r_fail(r, now, BYS_SERR_NO_RESOLVER, 0, NULL, -1, NULL); return; }
      const char *t = r->cfg.resolve(r->cfg.resolve_user, c.fp);
      if (!t) { r_fail(r, now, BYS_SERR_NOT_FOUND, 0, NULL, -1, NULL); return; }
      const size_t n = strlen(t);
      bys_ids_t ids;
      bys_text_ids(t, n, &ids);
      if (memcmp(ids.fingerprint, c.fp, BYS_REF_BYTES)) { r_fail(r, now, BYS_SERR_MISMATCH, 0, NULL, -1, NULL); return; }
      if (n + 1 > r->cfg.text_cap) { r_fail(r, now, BYS_SERR_NO_SPACE, 0, NULL, -1, NULL); return; }
      memcpy(r->cfg.text, t, n + 1);
      r->text_len = (uint32_t)n;
    }
    bys_text_ids(r->cfg.text, r->text_len, &r->ids);
    memcpy(r->first_short, r->ids.short_id, 3);
    r->has_text = 1;
    if (!sec->detect_multiple_emitters) { r_deliver(r, now); return; }
    r_set_state(r, BYS_RS_FINAL_LISTEN);
    r->final_end = now + BYS_FINAL_LISTEN_MS;
    return;
  }
  if (!r->have_first || content_same(&r->first, &c)) return; /* failed before any frame, or the sender repeating */
  uint8_t sh[3];
  content_short(&r->core, ev, sh);
  if (r->state == BYS_RS_FINAL_LISTEN) r_fail(r, now, BYS_SERR_MULTIPLE_TRANSMISSIONS, BYS_RISK_OTHER, sh, ev->band, NULL);
  else if (r->state == BYS_RS_DELIVERED && r->watching) r_conflict(r, now, BYS_SERR_MULTIPLE_TRANSMISSIONS, BYS_RISK_OTHER, sh, ev->band, NULL);
}

static void r_heard_risk(bys_receiver_t *r, uint32_t now, const bys_result_t *x, unsigned band) {
  bys_core_t *c = &r->core;
  const bys_risk_signal_t *cfg = &c->sec.risk;
  uint8_t key[8];
  risk_key(key, x->reason, x->source, x->own, x->intruder);
  if (!cfg->listen || seen_has(c, key) || mem_is_own(c->mem, key, now, cfg->quarantine_ms)) return;
  const int before = r->state == BYS_RS_LISTENING || r->state == BYS_RS_FINAL_LISTEN;
  const int watching_risk = r->state == BYS_RS_DELIVERED && r->watching && cfg->listen_until_confirm;
  if (!before && !watching_risk) return;
  seen_add(c, key);
  bys_alert_t alert;
  memset(&alert, 0, sizeof alert);
  alert.heard = 1; alert.reason = x->reason; alert.source = x->source; alert.hops = x->hops;
  memcpy(alert.own, x->own, 3); memcpy(alert.intruder, x->intruder, 3);
  alert.bands[0] = (uint8_t)band; alert.nbands = 1; alert.matches_mine = r->have_first && !memcmp(x->own, r->first_short, 3);
  notify_alert(c, &alert);
  quarantine(c, x->reason, x->source);
  if (!cfg->honor) return;
  /* The receiver never relays: the sender that hears it does. */
  if (before) r_fail(r, now, BYS_SERR_RISK_SIGNAL, 0, NULL, -1, &alert);
  else r_conflict(r, now, BYS_SERR_RISK_SIGNAL, 0, NULL, (int)band, &alert);
}

void bys_receiver_hear(bys_receiver_t *r, uint32_t now, const bys_demod_event_t *ev) {
  if (!ev) return;
  const bys_security_t *sec = &r->core.sec;
  switch (ev->type) {
    case BYS_EV_FRAME: {
      if (!ev->result) return;
      const bys_result_t *x = ev->result;
      if (x->kind == BYS_KIND_ACK) { if (x->receipt && r->acking && r->has_text && !memcmp(x->hash4, r->ids.ack, 4)) r_end_acks(r, 1); return; }
      if (x->kind == BYS_KIND_RISK) { r_heard_risk(r, now, x, ev->band); return; }
      r_heard_frame(r, now, ev);
      return;
    }
    case BYS_EV_HEADER: {
      /* A frame starting on another band during the final listen: wait for it, up to FINAL_LISTEN_MAX. */
      if (r->state != BYS_RS_FINAL_LISTEN || ev->band == r->first_band) return;
      uint32_t end = now + BYS_FINAL_EXTEND_MS;
      if (!due(end, r->final_end)) end = r->final_end; /* max(final_end, now + extend) */
      const uint32_t cap = r->first_at + BYS_FINAL_LISTEN_MAX_MS;
      if (!due(cap, end)) end = cap; /* min(end, cap) */
      if (due(r->final_end, end)) return; /* end <= final_end */
      r->final_end = end;
      return;
    }
    case BYS_EV_CLASH:
      if (!sec->detect_multiple_emitters || (r->state != BYS_RS_LISTENING && r->state != BYS_RS_FINAL_LISTEN)) return;
      { const uint8_t zero[3] = { 0, 0, 0 }; r_fail(r, now, BYS_SERR_MULTIPLE_TRANSMISSIONS, BYS_RISK_OVERLAP, zero, r->have_first ? (int)r->first_band : -1, NULL); }
      return;
    default: return;
  }
}

void bys_receiver_tick(bys_receiver_t *r, uint32_t now) {
  if (r->state == BYS_RS_LISTENING && since(now, r->start_at) >= r->cfg.timeout_ms) { r_fail(r, now, BYS_SERR_TIMEOUT, 0, NULL, -1, NULL); return; }
  if (r->state == BYS_RS_FINAL_LISTEN && due(now, r->final_end)) r_deliver(r, now);
  if (r->watching && due(now, r->watch_end_at)) { r->watch_expired = 1; r_stop_watch(r, now); r_maybe_finished(r); }
  if (r->acking && r->ack_phase == 1 && due(now, r->ack_listen_until)) r_ack_round(r, now);
}

void bys_receiver_play_done(bys_receiver_t *r, uint32_t now) {
  if (loop_done(&r->core, now)) return;
  if (r->alerting) { r->alerting = 0; r_maybe_finished(r); return; }
  if (r->acking && r->ack_phase == 0) { r->ack_phase = 1; r->ack_listen_until = now + BYS_RECEIPT_LISTEN_MS; }
}

int bys_receiver_confirmed(bys_receiver_t *r, uint32_t now, bys_confirmation_t *out) {
  if (!r->delivered) return r->state == BYS_RS_FAILED ? r->fail_err : BYS_SERR_PENDING;
  if (r->closed == 2) return BYS_SERR_CANCELLED;
  if (r->has_conflict) return r->conflict_err;
  if (r->has_confirmation) { if (out) *out = r->confirmation; return 0; }
  bys_confirmation_t c;
  if (!r->core.sec.confirm_watch) { c.watch = BYS_WATCH_OFF; c.watched_ms = 0; c.uncovered_ms = 0; }
  else if (r->watch_expired) { c.watch = BYS_WATCH_EXPIRED; c.watched_ms = r->watch_end - r->watch_start; c.uncovered_ms = since(now, r->watch_end); }
  else { c.watch = BYS_WATCH_CLEAN; c.watched_ms = (r->watching ? now : r->watch_end) - r->watch_start; c.uncovered_ms = 0; }
  r->closed = 1;
  r->confirmation = c; r->has_confirmation = 1;
  r_stop_watch(r, now);
  r_set_state(r, BYS_RS_CLOSED);
  r_maybe_finished(r);
  if (out) *out = c;
  return 0;
}

void bys_receiver_cancel(bys_receiver_t *r, uint32_t now) {
  if (r->closed) return;
  r->closed = 2;
  r_fail(r, now, BYS_SERR_CANCELLED, 0, NULL, -1, NULL);
  r_stop_watch(r, now);
  r_end_acks(r, 0);
  if (r->state == BYS_RS_DELIVERED) r_set_state(r, BYS_RS_CLOSED);
  r_maybe_finished(r);
}

/* ---------- ambient helper ---------- */
void bys_session_noise_from_demod(const bys_demod_t *d, float out[BYS_BAND_COUNT]) {
  /* int16 energy -> relative to a full-scale grid tone in the window: (32768 * sum(window) / 2)^2, sum of the Tukey(0.1) window = 0.95 * win */
  const float ref_db = 20.0f * log10f(32768.0f * 0.95f * (float)d->win / 2.0f);
  for (unsigned b = 0; b < BYS_BAND_COUNT; b++) {
    bys_demod_level_t lv;
    out[b] = bys_demod_level(d, (bys_band_t)b, &lv) ? lv.floor_db - ref_db : NAN;
  }
}
