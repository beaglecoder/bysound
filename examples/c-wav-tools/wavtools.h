/* Shared by batch-decode and noise-test: 16-bit PCM WAV I/O and "decode every frame in this audio" on top of bysound.h.
 * (c/examples/bysound_cli.c does the same inside its own file; this header is the reusable version of that code.) */
#ifndef WAVTOOLS_H
#define WAVTOOLS_H

#include "bysound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const BAND_NAMES[BYS_BAND_COUNT] = { "top", "high", "upper", "mid", "audible", "xhigh", "wide" };
static const char *const PROFILE_NAMES[2] = { "normal", "robust" };

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v & 0xffff); wr16(p + 2, v >> 16); }

/* Reads 16-bit PCM (first channel). Returns malloc'd samples or NULL (message on stderr unless quiet). */
static int16_t *wav_read(const char *path, size_t *n_out, uint32_t *rate_out, int quiet) {
  FILE *f = fopen(path, "rb");
  if (!f) { if (!quiet) perror(path); return NULL; }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = len > 0 ? malloc((size_t)len) : NULL;
  if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); return NULL; }
  fclose(f);
  int16_t *out = NULL;
  uint32_t rate = 0, ch = 0, bits = 0, fmt = 0;
  if (len >= 12 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4)) {
    size_t pos = 12;
    while (pos + 8 <= (size_t)len) {
      uint32_t sz = rd32(buf + pos + 4);
      const uint8_t *body = buf + pos + 8;
      size_t avail = (size_t)len - pos - 8;
      if (!memcmp(buf + pos, "fmt ", 4) && sz >= 16 && avail >= 16) {
        fmt = rd16(body); ch = rd16(body + 2); rate = rd32(body + 4); bits = rd16(body + 14);
        if (fmt == 0xFFFE && sz >= 26 && avail >= 26) fmt = rd16(body + 24);
      } else if (!memcmp(buf + pos, "data", 4)) {
        if (fmt != 1 || bits != 16 || ch < 1 || (rate != 16000 && rate != 44100 && rate != 48000)) break;
        if (sz > avail) sz = (uint32_t)avail;
        size_t frames = sz / (2u * ch);
        out = malloc((frames + 1) * sizeof(int16_t));
        if (!out) break;
        for (size_t i = 0; i < frames; i++) out[i] = (int16_t)rd16(body + 2 * ch * i);
        *n_out = frames; *rate_out = rate;
        break;
      }
      pos += 8 + (size_t)sz + (sz & 1u);
    }
  }
  if (!out && !quiet) fprintf(stderr, "%s: not a 16-bit PCM WAV at 16000/44100/48000 Hz\n", path);
  free(buf);
  return out;
}

static int wav_write(const char *path, const int16_t *pcm, size_t n, uint32_t rate) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return -1; }
  uint8_t h[44];
  memcpy(h, "RIFF", 4); wr32(h + 4, (uint32_t)(36 + 2 * n)); memcpy(h + 8, "WAVEfmt ", 8);
  wr32(h + 16, 16); wr16(h + 20, 1); wr16(h + 22, 1); wr32(h + 24, rate); wr32(h + 28, rate * 2); wr16(h + 32, 2); wr16(h + 34, 16);
  memcpy(h + 36, "data", 4); wr32(h + 40, (uint32_t)(2 * n));
  int ok = fwrite(h, 1, 44, f) == 44;
  for (size_t i = 0; i < n && ok; i++) { uint8_t b[2]; wr16(b, (uint16_t)pcm[i]); ok = fwrite(b, 1, 2, f) == 2; }
  if (fclose(f) != 0 || !ok) { fprintf(stderr, "%s: write failed\n", path); return -1; }
  return 0;
}

/* ---------- decoding every frame of a buffer ---------- */
#define WT_MAX_FRAMES 16
#define WT_TEXT_MAX 4096
typedef struct { int band, profile; unsigned corrected; int kind; char text[WT_TEXT_MAX]; double at_s; } wt_frame_t;
typedef struct { wt_frame_t frames[WT_MAX_FRAMES]; int count; } wt_result_t;

static bys_demod_t wt_demod[2]; /* one per profile; large, so static */
static uint8_t wt_payload[2][255 * 48 + 8];
typedef struct { int profile; wt_result_t *res; } wt_ctx_t;

static void wt_on_event(void *user, const bys_demod_event_t *ev) {
  wt_ctx_t *c = user;
  if (ev->type != BYS_EV_FRAME || c->res->count >= WT_MAX_FRAMES) return;
  wt_frame_t *fr = &c->res->frames[c->res->count++];
  fr->band = ev->band; fr->profile = ev->profile; fr->corrected = ev->result->corrected; fr->kind = ev->result->kind;
  fr->at_s = (double)bys_demod_stats(&wt_demod[c->profile])->samples / (double)wt_demod[c->profile].rate;
  fr->text[0] = 0;
  if (ev->result->kind == BYS_KIND_PAYLOAD && ev->payload) {
    if (bys_unpack(ev->payload, ev->payload_len, fr->text, sizeof fr->text) < 0) strcpy(fr->text, "<unpack error>");
  }
}

/* Listens on every band that fits under Nyquist, both profiles, like `bysound decode`. Returns the number of frames (0 if none). */
static int wt_decode(const int16_t *pcm, size_t n, uint32_t rate, wt_result_t *res) {
  static wt_ctx_t ctx[2];
  int active[2] = { 0, 0 };
  res->count = 0;
  for (int p = 0; p < 2; p++) {
    bys_demod_cfg_t cfg;
    bys_demod_cfg_default(&cfg);
    cfg.sample_rate = rate; cfg.profile = (bys_profile_t)p; cfg.band_mask = 0;
    for (int b = 0; b < BYS_BAND_COUNT; b++) {
      bys_band_info_t bi;
      if (bys_band_info((bys_band_t)b, (bys_profile_t)p, &bi) == BYS_OK && ((uint64_t)bi.bin + bi.groups * 16u - 1u) * 93750u < (uint64_t)rate * 1000u)
        cfg.band_mask |= (uint8_t)(1u << b);
    }
    if (!cfg.band_mask) continue;
    cfg.out = wt_payload[p]; cfg.out_cap = sizeof wt_payload[p]; cfg.cb = wt_on_event;
    ctx[p].profile = p; ctx[p].res = res; cfg.user = &ctx[p];
    if (bys_demod_init(&wt_demod[p], &cfg) != BYS_OK) continue;
    active[p] = 1;
  }
  static int16_t silence[1024];
  for (size_t i = 0; i < n; i += 1024) { size_t c = n - i < 1024 ? n - i : 1024; for (int p = 0; p < 2; p++) if (active[p]) bys_demod_push(&wt_demod[p], pcm + i, c); }
  for (size_t i = 0; i < rate / 2; i += 1024) for (int p = 0; p < 2; p++) if (active[p]) bys_demod_push(&wt_demod[p], silence, 1024);
  return res->count;
}

#endif
