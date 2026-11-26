/*
 * noise-test: add white Gaussian noise at a given SNR to a WAV and try to decode it.
 *   noise-test in.wav --snr 6 [--seed 1] [-o noisy.wav]       one SNR
 *   noise-test in.wav --sweep 20:-6:2                          from 20 dB down to -6 dB in steps of 2: where does it stop decoding?
 * SNR = power of the loud part of the file (samples above 5 % of the peak) over the power of the noise, over the whole 0..Nyquist band.
 * Real rooms colour the noise, so use it as a relative figure: compare recordings and bands with each other.
 */
#define _POSIX_C_SOURCE 200809L
#include "wavtools.h"
#include <math.h>

static uint64_t rng_state = 1;
static double uniform(void) { rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL; return ((double)(rng_state >> 11) + 0.5) / 9007199254740992.0; }
static double gauss(void) { return sqrt(-2.0 * log(uniform())) * cos(6.283185307179586 * uniform()); }

static double signal_rms(const int16_t *x, size_t n) {
  int peak = 1;
  for (size_t i = 0; i < n; i++) { int a = abs(x[i]); if (a > peak) peak = a; }
  double sum = 0; size_t k = 0;
  for (size_t i = 0; i < n; i++) if (abs(x[i]) * 20 > peak) { sum += (double)x[i] * x[i]; k++; }
  return k ? sqrt(sum / (double)k) : 0;
}

static void add_noise(const int16_t *in, int16_t *out, size_t n, double sigma, uint64_t seed) {
  rng_state = seed * 2654435761u + 1;
  for (size_t i = 0; i < n; i++) { double v = in[i] + sigma * gauss(); out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }
}

static wt_result_t res;
static int try_one(const int16_t *noisy, size_t n, uint32_t rate, double snr, double rms) {
  int k = wt_decode(noisy, n, rate, &res);
  printf("SNR %6.1f dB (noise rms %7.1f): ", snr, rms / pow(10.0, snr / 20.0));
  if (!k) { puts("nothing decoded"); return 0; }
  for (int j = 0; j < k; j++) printf("%s%s/%s corrected=%u \"%s\"", j ? " | " : "", BAND_NAMES[res.frames[j].band], PROFILE_NAMES[res.frames[j].profile], res.frames[j].corrected, res.frames[j].text);
  putchar('\n');
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 3) { fputs("usage: noise-test in.wav (--snr DB | --sweep FROM:TO:STEP) [--seed N] [-o noisy.wav]\n", stderr); return 2; }
  const char *in = argv[1], *outp = NULL;
  double from = 0, to = 0, step = 1;
  int have = 0;
  uint64_t seed = 1;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--snr") && i + 1 < argc) { from = to = atof(argv[++i]); step = 1; have = 1; }
    else if (!strcmp(argv[i], "--sweep") && i + 1 < argc) { if (sscanf(argv[++i], "%lf:%lf:%lf", &from, &to, &step) != 3 || step <= 0) { fputs("--sweep FROM:TO:STEP\n", stderr); return 2; } have = 1; }
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
  }
  if (!have) { fputs("give --snr or --sweep\n", stderr); return 2; }
  size_t n = 0; uint32_t rate = 0;
  int16_t *pcm = wav_read(in, &n, &rate, 0);
  if (!pcm) return 2;
  int16_t *noisy = malloc(n * sizeof *noisy + 2);
  double rms = signal_rms(pcm, n);
  printf("%s: %u Hz, %.2f s, signal rms %.1f\n", in, rate, (double)n / rate, rms);
  int any = 0;
  double dir = from >= to ? -1 : 1;
  for (double snr = from; dir < 0 ? snr >= to - 1e-9 : snr <= to + 1e-9; snr += dir * step) {
    add_noise(pcm, noisy, n, rms / pow(10.0, snr / 20.0), seed);
    any |= try_one(noisy, n, rate, snr, rms);
    if (outp && from == to) wav_write(outp, noisy, n, rate);
    if (from == to) break;
  }
  free(pcm); free(noisy);
  return any ? 0 : 1;
}
