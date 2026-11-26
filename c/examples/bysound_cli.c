/*
 * bysound: command-line tool for bysound v4 (data over sound). File based and portable (C99, no platform audio API).
 *
 *   bysound encode "text" -o out.wav [--band B] [--profile normal|robust] [--rate HZ] [--repeat N]
 *   bysound ack "text" -o ack.wav [--receipt] [--heard BAND[/robust]] [--band B] [--rate HZ]
 *   bysound decode in.wav
 *   bysound bands
 */
#define _POSIX_C_SOURCE 200112L
#include "bysound.h"
#include "bysound_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const BAND_NAMES[BYS_BAND_COUNT] = { "top", "high", "upper", "mid", "audible", "xhigh", "wide" };
static const char *const PROFILE_NAMES[2] = { "normal", "robust" };

static int band_from_name(const char *s) {
  for (int i = 0; i < BYS_BAND_COUNT; i++)
    if (!strcmp(s, BAND_NAMES[i])) return i;
  return -1;
}

static int usage(void) {
  fputs("usage:\n"
        "  bysound encode \"text\" -o out.wav [--band top|high|upper|mid|audible|xhigh|wide]\n"
        "                                [--profile normal|robust] [--rate 16000|44100|48000] [--repeat N]\n"
        "  bysound ack \"text\" -o ack.wav [--receipt] [--heard BAND[/robust]] [--band mid|high|...] [--rate 16000|44100|48000]\n"
        "                                (an ack, or a receipt, for the text; --heard: where it was read, SESSION.md 3.1)\n"
        "  bysound decode in.wav\n"
        "  bysound bands\n",
        stderr);
  return 2;
}

/* ---------- WAV I/O ---------- */
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

static int write_wav(const char *path, const int16_t *pcm, size_t n, uint32_t rate) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return -1; }
  uint8_t h[44];
  memcpy(h, "RIFF", 4); put32(h + 4, (uint32_t)(36 + 2 * n)); memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, rate); put32(h + 28, rate * 2); put16(h + 32, 2); put16(h + 34, 16);
  memcpy(h + 36, "data", 4); put32(h + 40, (uint32_t)(2 * n));
  uint8_t *b = malloc(2 * n + 1);
  if (!b) { fclose(f); return -1; }
  for (size_t i = 0; i < n; i++) put16(b + 2 * i, (uint16_t)pcm[i]); /* explicit little endian, any host */
  int ok = fwrite(h, 1, 44, f) == 44 && fwrite(b, 2, n, f) == n;
  free(b);
  if (fclose(f) != 0 || !ok) { fprintf(stderr, "%s: write failed\n", path); return -1; }
  return 0;
}

/* Reads a 16-bit PCM WAV (mono, or the first channel of a multichannel file). Walks RIFF chunks. Returns malloc'd samples. */
static int16_t *read_wav(const char *path, size_t *n_out, uint32_t *rate_out) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return NULL; }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = len > 0 ? malloc((size_t)len) : NULL;
  if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "%s: cannot read\n", path); fclose(f); free(buf); return NULL; }
  fclose(f);
  if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { fprintf(stderr, "%s: not a RIFF/WAVE file\n", path); free(buf); return NULL; }
  uint32_t rate = 0, ch = 0, bits = 0, fmt = 0;
  size_t pos = 12;
  int16_t *out = NULL;
  while (pos + 8 <= (size_t)len) {
    uint32_t sz = get32(buf + pos + 4);
    const uint8_t *body = buf + pos + 8;
    size_t avail = (size_t)len - pos - 8;
    if (!memcmp(buf + pos, "fmt ", 4) && sz >= 16 && avail >= 16) {
      fmt = get16(body); ch = get16(body + 2); rate = get32(body + 4); bits = get16(body + 14);
      if (fmt == 0xFFFE && sz >= 26 && avail >= 26) fmt = get16(body + 24); /* WAVE_FORMAT_EXTENSIBLE: sub-format code */
    } else if (!memcmp(buf + pos, "data", 4)) {
      if (fmt != 1 || bits != 16 || ch < 1) { fprintf(stderr, "%s: only 16-bit PCM is supported\n", path); break; }
      if (rate != 16000 && rate != 44100 && rate != 48000) { fprintf(stderr, "%s: sample rate %u not supported (16000, 44100, 48000)\n", path, rate); break; }
      if (sz > avail) sz = (uint32_t)avail; /* streamed files may carry a bogus size */
      size_t frames = sz / (2u * ch);
      out = malloc((frames + 1) * sizeof(int16_t));
      if (!out) break;
      for (size_t i = 0; i < frames; i++) out[i] = (int16_t)get16(body + 2 * ch * i);
      *n_out = frames;
      *rate_out = rate;
      break;
    }
    pos += 8 + (size_t)sz + (sz & 1u);
  }
  if (!out && fmt == 0) fprintf(stderr, "%s: no fmt chunk\n", path);
  else if (!out && fmt != 0 && rate == 0) fprintf(stderr, "%s: no data chunk\n", path);
  free(buf);
  return out;
}

/* ---------- encode ---------- */
static int cmd_encode(int argc, char **argv) {
  const char *text = NULL, *out = NULL;
  int band = BYS_TOP, profile = BYS_NORMAL, repeat = 1;
  uint32_t rate = 48000;
  for (int i = 0; i < argc; i++) {
    const char *a = argv[i];
    const int has = i + 1 < argc;
    if (!strcmp(a, "-o") && has) out = argv[++i];
    else if (!strcmp(a, "--band") && has) { band = band_from_name(argv[++i]); if (band < 0) { fprintf(stderr, "unknown band '%s'\n", argv[i]); return 2; } }
    else if (!strcmp(a, "--profile") && has) {
      const char *p = argv[++i];
      if (!strcmp(p, "normal")) profile = BYS_NORMAL; else if (!strcmp(p, "robust")) profile = BYS_ROBUST; else { fprintf(stderr, "unknown profile '%s'\n", p); return 2; }
    } else if (!strcmp(a, "--rate") && has) rate = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (!strcmp(a, "--repeat") && has) repeat = atoi(argv[++i]);
    else if (a[0] != '-' && !text) text = a;
    else return usage();
  }
  if (!text || !out || repeat < 1) return usage();

  bys_tx_cfg_t cfg;
  bys_tx_cfg_default(&cfg);
  cfg.sample_rate = rate;
  bys_tx_t tx;
  int r = bys_tx_init_payload(&tx, &cfg, (bys_band_t)band, (bys_profile_t)profile, text, strlen(text));
  if (r != BYS_OK) {
    if (r == BYS_E_TOO_LONG) {
      fprintf(stderr, "text too long: one transmission cannot last more than %d s; the maximum for %s/%s is %d packed bytes (= %d ASCII letters; digit pairs pack 2:1, so numeric text fits up to twice as many characters)\n",
              BYS_MAX_FRAME_SECONDS, BAND_NAMES[band], PROFILE_NAMES[profile], bys_max_payload_bytes((bys_band_t)band, (bys_profile_t)profile), bys_max_payload_bytes((bys_band_t)band, (bys_profile_t)profile));
      return 1;
    }
    fprintf(stderr, "cannot encode: error %d (%s)\n", r, r == BYS_E_PROFILE ? "robust exists only for top, high, upper and mid" : r == BYS_E_RATE ? "sample rate too low for this band" : r == BYS_E_TOO_LONG ? "text too long" : "see bysound.h");
    return 1;
  }
  const size_t lead = rate / 5, one = bys_tx_total_samples(&tx), total = lead + one * (size_t)repeat;
  int16_t *pcm = calloc(total, sizeof(int16_t));
  if (!pcm) return 1;
  for (int k = 0; k < repeat; k++) {
    if (k) bys_tx_init_payload(&tx, &cfg, (bys_band_t)band, (bys_profile_t)profile, text, strlen(text));
    size_t at = lead + one * (size_t)k;
    while (!bys_tx_done(&tx) && at < total) at += bys_tx_fill(&tx, pcm + at, total - at < 4096 ? total - at : 4096);
  }
  r = write_wav(out, pcm, total, rate);
  if (r == 0) printf("%s: %s/%s, %u Hz, %.2f s, %d frame(s)\n", out, BAND_NAMES[band], PROFILE_NAMES[profile], rate, (double)total / rate, repeat);
  free(pcm);
  return r ? 1 : 0;
}

/* ---------- ack ---------- */
static int cmd_ack(int argc, char **argv) {
  const char *text = NULL, *out = NULL;
  int band = -1, receipt = 0, heard_band = -1, heard_robust = 0;
  uint32_t rate = 48000;
  for (int i = 0; i < argc; i++) {
    const char *a = argv[i];
    const int has = i + 1 < argc;
    if (!strcmp(a, "-o") && has) out = argv[++i];
    else if (!strcmp(a, "--receipt")) receipt = 1;
    else if (!strcmp(a, "--band") && has) { band = band_from_name(argv[++i]); if (band < 0) { fprintf(stderr, "unknown band '%s'\n", argv[i]); return 2; } }
    else if (!strcmp(a, "--heard") && has) {
      char name[16];
      const char *v = argv[++i], *slash = strchr(v, '/');
      size_t n = slash ? (size_t)(slash - v) : strlen(v);
      if (n >= sizeof name) n = sizeof name - 1;
      memcpy(name, v, n); name[n] = 0;
      heard_band = band_from_name(name);
      if (heard_band < 0 || (slash && strcmp(slash + 1, "robust") && strcmp(slash + 1, "normal"))) { fprintf(stderr, "bad --heard '%s'\n", v); return 2; }
      heard_robust = slash && !strcmp(slash + 1, "robust");
    } else if (!strcmp(a, "--rate") && has) rate = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (a[0] != '-' && !text) text = a;
    else return usage();
  }
  if (!text || !out) return usage();
  if (band < 0) band = receipt ? BYS_HIGH : BYS_MID; /* acks travel on mid, receipts on high */
  bys_ids_t ids;
  bys_text_ids(text, strlen(text), &ids);
  bys_tx_cfg_t cfg;
  bys_tx_cfg_default(&cfg);
  cfg.sample_rate = rate;
  bys_tx_t tx;
  const unsigned nib = heard_band < 0 ? 0u : bys_heard_nibble((bys_band_t)heard_band, (bys_profile_t)heard_robust);
  int r = bys_tx_init_ack_heard(&tx, &cfg, (bys_band_t)band, receipt, nib, ids.ack);
  if (r != BYS_OK) { fprintf(stderr, "cannot encode: error %d\n", r); return 1; }
  const size_t lead = rate / 5, total = lead + bys_tx_total_samples(&tx);
  int16_t *pcm = calloc(total, sizeof(int16_t));
  if (!pcm) return 1;
  for (size_t at = lead; !bys_tx_done(&tx) && at < total;) at += bys_tx_fill(&tx, pcm + at, total - at < 4096 ? total - at : 4096);
  r = write_wav(out, pcm, total, rate);
  if (r == 0) printf("%s: %s id=%02x%02x%02x%02x on %s, %u Hz, %.2f s\n", out, receipt ? "receipt" : "ack", ids.ack[0], ids.ack[1], ids.ack[2], ids.ack[3], BAND_NAMES[band], rate, (double)total / rate);
  free(pcm);
  return r ? 1 : 0;
}

/* ---------- decode ---------- */
#define CHUNK 1024
#define ALL_BANDS ((1u << BYS_BAND_COUNT) - 1u)
#define MAX_TEXT (2 * (255 * 48 + 4) + 16)

static bys_demod_t demod[2]; /* one demodulator per profile; both hear the same audio */
static uint8_t payload_buf[2][255 * 48 + 8];
static char text_buf[MAX_TEXT];
static unsigned frames_seen;

typedef struct { const bys_demod_t *d; } ctx_t;

static void on_event(void *user, const bys_demod_event_t *ev) {
  const bys_demod_t *d = ((ctx_t *)user)->d;
  if (ev->type != BYS_EV_FRAME) return;
  frames_seen++;
  printf("[%6.2f s] band=%s profile=%s corrected=%u", (double)bys_demod_stats(d)->samples / d->rate, BAND_NAMES[ev->band], PROFILE_NAMES[ev->profile], ev->result->corrected);
  if (ev->result->kind == BYS_KIND_PAYLOAD && ev->payload) {
    int n = bys_unpack(ev->payload, ev->payload_len, text_buf, sizeof text_buf);
    if (n >= 0) printf(" text=%s\n", text_buf); else printf(" text=<unpack error %d>\n", n);
  } else if (ev->result->kind == BYS_KIND_ACK) {
    const bys_result_t *r = ev->result;
    bys_band_t hb = BYS_TOP;
    bys_profile_t hp = BYS_NORMAL;
    printf(" %s id=%02x%02x%02x%02x", r->receipt ? "receipt" : "ack", r->hash4[0], r->hash4[1], r->hash4[2], r->hash4[3]);
    if (bys_heard_parse(r->heard, &hb, &hp) == 1) printf(" heard=%s/%s", BAND_NAMES[hb], PROFILE_NAMES[hp]);
    putchar('\n');
  } else if (ev->result->kind == BYS_KIND_RISK) {
    const bys_result_t *r = ev->result;
    printf(" risk reason=%u source=%s hops=%u own=%02x%02x%02x intruder=%02x%02x%02x\n", r->reason, r->source ? "receiver" : "sender", r->hops,
           r->own[0], r->own[1], r->own[2], r->intruder[0], r->intruder[1], r->intruder[2]);
  } else {
    printf(" kind=%c (not a text payload)\n", ev->result->kind);
  }
}

static int cmd_decode(int argc, char **argv) {
  if (argc != 1) return usage();
  size_t n = 0;
  uint32_t rate = 0;
  int16_t *pcm = read_wav(argv[0], &n, &rate);
  if (!pcm) return 1;
  /* Listen on every band that fits under Nyquist; robust only exists for the first four. */
  static ctx_t ctx[2];
  int active[2] = { 0, 0 };
  for (int p = 0; p < 2; p++) {
    bys_demod_cfg_t cfg;
    bys_demod_cfg_default(&cfg);
    cfg.sample_rate = rate;
    cfg.profile = (bys_profile_t)p;
    cfg.band_mask = 0;
    for (int b = 0; b < BYS_BAND_COUNT; b++) {
      bys_band_info_t bi;
      if (bys_band_info((bys_band_t)b, (bys_profile_t)p, &bi) == BYS_OK && ((uint64_t)bi.bin + bi.groups * 16u - 1u) * 93750u < (uint64_t)rate * 1000u)
        cfg.band_mask |= (uint8_t)(1u << b);
    }
    if (!cfg.band_mask) continue; /* e.g. no robust band fits at 16 kHz */
    cfg.out = payload_buf[p];
    cfg.out_cap = sizeof payload_buf[p];
    cfg.cb = on_event;
    cfg.user = &ctx[p];
    ctx[p].d = &demod[p];
    int r = bys_demod_init(&demod[p], &cfg);
    if (r != BYS_OK) { fprintf(stderr, "demodulator init failed: %d\n", r); free(pcm); return 1; }
    active[p] = 1;
  }
  for (size_t i = 0; i < n; i += CHUNK) {
    size_t c = n - i < CHUNK ? n - i : CHUNK;
    for (int p = 0; p < 2; p++) if (active[p]) bys_demod_push(&demod[p], pcm + i, c);
  }
  /* trailing silence so a frame that ends at the very end of the file is flushed */
  static int16_t silence[CHUNK];
  for (size_t i = 0; i < rate / 2; i += CHUNK) for (int p = 0; p < 2; p++) if (active[p]) bys_demod_push(&demod[p], silence, CHUNK);
  printf("%u frame(s) decoded from %s (%u Hz, %.2f s)\n", frames_seen, argv[0], rate, (double)n / rate);
  free(pcm);
  return frames_seen ? 0 : 1;
}

/* ---------- bands ---------- */
static int cmd_bands(void) {
  printf("band     range (Hz)       tones  symbol  raw bit rate  robust           max text in %d s (packed bytes: normal / robust)\n", BYS_MAX_FRAME_SECONDS);
  for (int b = 0; b < BYS_BAND_COUNT; b++) {
    bys_band_info_t bi;
    bys_band_info((bys_band_t)b, BYS_NORMAL, &bi);
    const double lo = bi.bin * 46.875, hi = (bi.bin + bi.groups * 16 - 1) * 46.875;
    const double bps = bi.groups * 4.0 / (bi.symbol_us / 1e6);
    const int mn = bys_max_payload_bytes((bys_band_t)b, BYS_NORMAL), mr = bys_max_payload_bytes((bys_band_t)b, BYS_ROBUST);
    char rob[16];
    if (mr > 0) snprintf(rob, sizeof rob, "%d", mr); else snprintf(rob, sizeof rob, "-");
    printf("%-8s %6.0f-%6.0f   %5d  %3u ms  %7.0f bit/s  %-16s %d / %s\n", BAND_NAMES[b], lo, hi, bi.groups * 16, (unsigned)(bi.symbol_us / 1000), bps, b <= BYS_MID ? "yes (2x symbol)" : "no", mn, rob);
  }
  puts("(raw rate before preamble, header, Reed-Solomon and CRC overhead; tone spacing 46.875 Hz, 4 bits per tone group)");
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) return usage();
  if (!strcmp(argv[1], "encode")) return cmd_encode(argc - 2, argv + 2);
  if (!strcmp(argv[1], "ack")) return cmd_ack(argc - 2, argv + 2);
  if (!strcmp(argv[1], "decode")) return cmd_decode(argc - 2, argv + 2);
  if (!strcmp(argv[1], "bands")) return cmd_bands();
  return usage();
}
