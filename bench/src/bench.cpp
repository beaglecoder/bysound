// Benchmark harness: bysound v4 (C99 lib in ../c) vs ggwave (third_party/ggwave), same simulated channel, same seeds.
//
//   bench info  <outdir>                      deterministic tables: airtime, max payload, PAPR, occupied band, resampler check
//   bench jobs  [plan]                         number of jobs in a plan
//   bench run   <plan> <shard> <nshards> <out.csv>
//   bench trial <plan> "<config>" <size> <condition> <snr|clean> <reps> <trial> [noise_snr]   replay one trial verbosely
//   bench scan  <size> <n> [inaudible]          noiseless delivery scan
//   bench dither <plan>...                      ggwave DT clean-point failures, re-run with noise at +60 dB SNR
//
// Plans: snr, size, cond, rate44, mtfixed, idle, all (everything except the idle and inaudible plans), quick (smoke test),
// inaudible (bysound top vs ggwave DT/MT moved to 18 kHz; same seeds as snr/cond/size) and inaudible-idle.
// Every trial is seeded from (plan cell without the system/config, trial index), so both systems see the same payload,
// the same lead silence and the same noise realization.
#include "bysound.h"
#include "ggwave/ggwave.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <time.h>
#include <vector>

static const double PI = 3.14159265358979323846;

// ---------------------------------------------------------------- RNG (splitmix64 seeding + xoshiro256**)
static uint64_t splitmix(uint64_t &x) {
  uint64_t z = (x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
struct Rng {
  uint64_t s[4];
  bool has = false;
  double spare = 0;
  explicit Rng(uint64_t seed) { for (int i = 0; i < 4; i++) s[i] = splitmix(seed); }
  static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  uint64_t next() {
    uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl(s[3], 45);
    return r;
  }
  double unif() { return ((next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
  double gauss() {
    if (has) { has = false; return spare; }
    double u = unif(), v = unif(), r = std::sqrt(-2.0 * std::log(u));
    spare = r * std::sin(2 * PI * v); has = true;
    return r * std::cos(2 * PI * v);
  }
};
static uint64_t fnv64(const std::string &s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
  return h;
}

// ---------------------------------------------------------------- systems and configurations
enum Sys { BYSOUND, GGWAVE };
struct Config {
  Sys sys;
  std::string name;  // e.g. "bysound top/normal", "ggwave ultrasound-fast"
  int band = 0, profile = 0;            // bysound
  int proto = 0; bool fixed = false;    // ggwave (fixed: fixed-length mode, payloadLength = payload size)
  int freq = 0;                         // ggwave: 0 = library defaults; > 0 = DT/MT moved to start at this FFT bin (see gg_protocols)
};
static std::vector<Config> bys_configs() {
  static const char *bn[] = { "top", "high", "upper", "mid", "audible", "xhigh", "wide" };
  std::vector<Config> v;
  for (int b = 0; b < 7; b++) { Config c; c.sys = BYSOUND; c.band = b; c.profile = 0; c.name = std::string("bysound ") + bn[b] + "/normal"; v.push_back(c); }
  for (int b = 0; b < 4; b++) { Config c; c.sys = BYSOUND; c.band = b; c.profile = 1; c.name = std::string("bysound ") + bn[b] + "/robust"; v.push_back(c); }
  return v;
}
static std::vector<Config> gg_configs(bool include_mt_fixed) {
  struct P { int id; const char *n; };
  static const P ps[] = {
    { GGWAVE_PROTOCOL_AUDIBLE_NORMAL, "audible-normal" }, { GGWAVE_PROTOCOL_AUDIBLE_FAST, "audible-fast" }, { GGWAVE_PROTOCOL_AUDIBLE_FASTEST, "audible-fastest" },
    { GGWAVE_PROTOCOL_ULTRASOUND_NORMAL, "ultrasound-normal" }, { GGWAVE_PROTOCOL_ULTRASOUND_FAST, "ultrasound-fast" }, { GGWAVE_PROTOCOL_ULTRASOUND_FASTEST, "ultrasound-fastest" },
    { GGWAVE_PROTOCOL_DT_NORMAL, "dt-normal" }, { GGWAVE_PROTOCOL_DT_FAST, "dt-fast" }, { GGWAVE_PROTOCOL_DT_FASTEST, "dt-fastest" },
  };
  std::vector<Config> v;
  for (auto &p : ps) { Config c; c.sys = GGWAVE; c.proto = p.id; c.name = std::string("ggwave ") + p.n; v.push_back(c); }
  if (include_mt_fixed) {
    static const P mt[] = { { GGWAVE_PROTOCOL_MT_NORMAL, "mt-normal" }, { GGWAVE_PROTOCOL_MT_FAST, "mt-fast" }, { GGWAVE_PROTOCOL_MT_FASTEST, "mt-fastest" } };
    for (auto &p : mt) { Config c; c.sys = GGWAVE; c.proto = p.id; c.fixed = true; c.name = std::string("ggwave ") + p.n + " (fixed-len)"; v.push_back(c); }
  }
  return v;
}

// ggwave has no built-in protocol above 18 kHz (ultrasound = 15.0-19.4 kHz). Its 2-tone DT protocols (2 x 16 bins) and 1-tone MT
// protocols (16 bins) fit in bysound top's span when moved to start at bin 384 = 18000 Hz / 46.875 Hz: DT 18.0-19.5 kHz, MT
// 18.0-18.75 kHz. Done only through ggwave's public C API (ggwave_tx/rxProtocolSetFreqStart, ggwave_rxToggleProtocol); every
// other parameter keeps its library default. DT's start/end markers are placed from the protocol's own freqStart (bitFreq), so
// they move with it; the receiver's marker search and fixed-length normalisation use the enabled rx protocols (minFreqStart).
static const int GG_BIN_18K = 384;
static std::vector<Config> gg_inaudible_configs() {
  struct P { int id; const char *n; bool fixed; };
  static const P ps[] = {
    { GGWAVE_PROTOCOL_DT_NORMAL, "dt-normal", false }, { GGWAVE_PROTOCOL_DT_FAST, "dt-fast", false }, { GGWAVE_PROTOCOL_DT_FASTEST, "dt-fastest", false },
    { GGWAVE_PROTOCOL_MT_NORMAL, "mt-normal", true }, { GGWAVE_PROTOCOL_MT_FAST, "mt-fast", true }, { GGWAVE_PROTOCOL_MT_FASTEST, "mt-fastest", true },
  };
  std::vector<Config> v;
  for (auto &p : ps) {
    Config c; c.sys = GGWAVE; c.proto = p.id; c.fixed = p.fixed; c.freq = GG_BIN_18K;
    c.name = std::string("ggwave ") + p.n + " @18k" + (p.fixed ? " (fixed-len)" : "");
    v.push_back(c);
  }
  return v;
}
// Sets ggwave's global protocol tables, which an instance copies when it is created. freq = 0: library defaults (every protocol
// enabled on rx, DT/MT at bin 24). freq > 0: DT and MT start at bin `freq` on tx and rx, and the receiver has only those six enabled.
static void gg_protocols(int freq) {
  for (int id = GGWAVE_PROTOCOL_AUDIBLE_NORMAL; id <= GGWAVE_PROTOCOL_MT_FASTEST; id++) {
    bool dtmt = id >= GGWAVE_PROTOCOL_DT_NORMAL;
    ggwave_rxToggleProtocol((ggwave_ProtocolId)id, freq == 0 || dtmt);
    if (dtmt) {
      ggwave_txProtocolSetFreqStart((ggwave_ProtocolId)id, freq ? freq : 24);
      ggwave_rxProtocolSetFreqStart((ggwave_ProtocolId)id, freq ? freq : 24);
    }
  }
}
static std::vector<Config> bys_top_configs() {
  std::vector<Config> v;
  for (auto &c : bys_configs()) if (c.band == 0) v.push_back(c);
  return v;
}

// ---------------------------------------------------------------- payloads
// ASCII payloads use printable ASCII without digits: bysound packs digit pairs into one byte, which would make it look faster
// than its raw byte capacity. The UTF-8 case is exactly 48 bytes of mixed multi-byte text.
static std::string make_payload(const std::string &label, int trial) {
  Rng r(fnv64("payload|" + label + "|" + std::to_string(trial)));
  if (label == "utf8") {
    static const char *words[] = { "caf\xc3\xa9", "\xc3\xb1" "and\xc3\xba", "Gr\xc3\xb6\xc3\x9f" "e", "\xe6\x9d\xb1\xe4\xba\xac", "\xf0\x9f\x99\x82", "\xce\xb1\xce\xb2\xce\xb3",
                                   "\xd0\xbc\xd0\xb8\xd1\x80", "na\xc3\xafve", "\xe2\x82\xac", "\xc3\xa5ngstr\xc3\xb6m", "\xe2\x9c\x93", "pi\xc3\xb1" "a" };
    std::string s;
    for (;;) {
      std::string w = words[r.next() % (sizeof words / sizeof *words)];
      if (s.size() + w.size() + 1 > 48) break;
      if (!s.empty()) s += ' ';
      s += w;
    }
    while (s.size() < 48) s += (char)('a' + r.next() % 26);
    return s;
  }
  static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz .,;:!?-_()'/@#%&*+=<>";
  size_t n = (size_t)std::atoi(label.c_str());
  std::string s(n, ' ');
  for (size_t i = 0; i < n; i++) s[i] = alpha[r.next() % (sizeof alpha - 1)];
  return s;
}

// ---------------------------------------------------------------- encoders
struct Wave {
  std::vector<float> x;   // one message as emitted by the library's TX API (float, full scale = 1)
  size_t active = 0;      // sound-on samples (bysound: frame without its 350 ms trailing gap; ggwave: whole waveform)
  bool ok = false;
  const char *why = nullptr;  // when !ok: "exceeds 30 s cap" (bysound, BYS_E_TOO_LONG) or "cannot send" (ggwave limits)
};
static int g_tx = -1;
static int g_tx_fixed_len = 0, g_tx_freq = 0;
static int gg_tx_instance(int fixed_len, int freq) {
  if (g_tx >= 0 && g_tx_fixed_len == fixed_len && g_tx_freq == freq) return g_tx;
  if (g_tx >= 0) ggwave_free(g_tx);
  ggwave_Parameters p = ggwave_getDefaultParameters();
  p.payloadLength = fixed_len > 0 ? fixed_len : -1;
  p.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_F32;
  p.operatingMode = GGWAVE_OPERATING_MODE_TX;
  gg_protocols(freq);
  g_tx = ggwave_init(p);
  gg_protocols(0);
  g_tx_fixed_len = fixed_len;
  g_tx_freq = freq;
  return g_tx;
}
static Wave encode(const Config &c, const std::string &msg, unsigned rate) {
  Wave w;
  if (c.sys == BYSOUND) {
    bys_tx_cfg_t cfg; bys_tx_cfg_default(&cfg); cfg.sample_rate = rate;
    bys_tx_t tx;
    int ri = bys_tx_init_payload(&tx, &cfg, (bys_band_t)c.band, (bys_profile_t)c.profile, msg.data(), msg.size());
    if (ri != BYS_OK) { w.why = ri == BYS_E_TOO_LONG ? "exceeds 30 s cap" : "cannot send"; return w; }
    uint32_t total = bys_tx_total_samples(&tx);
    std::vector<int16_t> pcm(total + 4096);
    size_t n = 0;
    while (!bys_tx_done(&tx)) { size_t k = bys_tx_fill(&tx, pcm.data() + n, 4096); n += k; if (!k) break; }
    w.x.resize(n);
    for (size_t i = 0; i < n; i++) w.x[i] = pcm[i] / 32768.0f;
    w.active = n - tx.gap_n;
    w.ok = true;
    return w;
  }
  if (rate != 48000) return w;
  // ggwave silently truncates longer payloads to 140 bytes (variable length) and cannot prepare a fixed-length instance above
  // 64 bytes: both are reported as "cannot send" rather than encoded.
  if (msg.size() > (c.fixed ? 64u : 140u) || msg.empty()) { w.why = "cannot send"; return w; }
  int inst = gg_tx_instance(c.fixed ? (int)msg.size() : 0, c.freq);
  int nbytes = ggwave_encode(inst, msg.data(), (int)msg.size(), (ggwave_ProtocolId)c.proto, 10 /* GGWave::kDefaultVolume */, nullptr, 1);
  if (nbytes <= 0) return w;
  w.x.resize((size_t)nbytes / sizeof(float));
  int got = ggwave_encode(inst, msg.data(), (int)msg.size(), (ggwave_ProtocolId)c.proto, 10, w.x.data(), 0);
  if (got <= 0) { w.x.clear(); return w; }
  w.x.resize((size_t)got / sizeof(float));
  w.active = w.x.size();
  w.ok = true;
  return w;
}

// ---------------------------------------------------------------- channel
struct Cond {
  std::string name;
  bool echo = false;
  double ppm = 0;
  int noise = 0;  // 0 white, 1 pink, 2 babble
};
static Cond cond_of(const std::string &n) {
  Cond c; c.name = n;
  if (n == "awgn") {}
  else if (n == "echo") c.echo = true;
  else if (n == "pink") c.noise = 1;
  else if (n == "babble") c.noise = 2;
  else if (n == "drift+100") c.ppm = 100;
  else if (n == "drift-100") c.ppm = -100;
  else if (n == "combo") { c.echo = true; c.ppm = 100; c.noise = 1; }
  else { fprintf(stderr, "unknown condition %s\n", n.c_str()); exit(2); }
  return c;
}

// Windowed-sinc interpolation (64 taps, Blackman window, 2048 fractional phases): clock drift without the treble loss of
// linear interpolation (which would unfairly attenuate the 15-20 kHz modes).
static const int RS_HALF = 32, RS_PHASES = 2048;
static std::vector<float> rs_table;
static void rs_init() {
  if (!rs_table.empty()) return;
  rs_table.resize((size_t)(RS_PHASES + 1) * 2 * RS_HALF);
  for (int ph = 0; ph <= RS_PHASES; ph++) {
    double f = (double)ph / RS_PHASES;
    for (int k = -RS_HALF + 1; k <= RS_HALF; k++) {
      double t = k - f;  // distance from the interpolation point
      double s = std::fabs(t) < 1e-12 ? 1.0 : std::sin(PI * t) / (PI * t);
      double u = (t + RS_HALF) / (2.0 * RS_HALF);  // 0..1 across the window
      double win = (u <= 0 || u >= 1) ? 0 : 0.42 - 0.5 * std::cos(2 * PI * u) + 0.08 * std::cos(4 * PI * u);
      rs_table[(size_t)ph * 2 * RS_HALF + (size_t)(k + RS_HALF - 1)] = (float)(s * win);
    }
  }
}
// The receiver's clock runs (1 + ppm 1e-6) times the sender's: output sample i reads input position i * (1 + ppm 1e-6).
static std::vector<float> resample(const std::vector<float> &in, double ppm) {
  rs_init();
  double ratio = 1.0 + ppm * 1e-6;
  size_t m = (size_t)((double)(in.size() - 1) / ratio);
  std::vector<float> out(m);
  for (size_t i = 0; i < m; i++) {
    double p = (double)i * ratio;
    long j = (long)p;
    double f = p - (double)j;
    int ph = (int)std::lround(f * RS_PHASES);
    const float *h = &rs_table[(size_t)ph * 2 * RS_HALF];
    double acc = 0;
    for (int k = -RS_HALF + 1; k <= RS_HALF; k++) {
      long q = j + k;
      if (q >= 0 && q < (long)in.size()) acc += in[(size_t)q] * h[k + RS_HALF - 1];
    }
    out[i] = (float)acc;
  }
  return out;
}

static void gen_noise(int type, size_t n, Rng &r, std::vector<double> &out, unsigned rate) {
  out.assign(n, 0.0);
  if (type == 0) { for (size_t i = 0; i < n; i++) out[i] = r.gauss(); return; }
  if (type == 1) {  // pink: Paul Kellett's refined filter on white Gaussian noise
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (size_t i = 0; i < n; i++) {
      double w = r.gauss();
      b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
      b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
      out[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362; b6 = w * 0.115926;
    }
    return;
  }
  // babble-like: 6 independent "talkers"; each is white noise high-passed at 100 Hz and low-passed twice at 800 Hz
  // (-12 dB/octave above ~1 kHz, roughly a long-term speech spectrum), times a random ~4 Hz syllabic envelope.
  const int K = 6;
  double ahp = std::exp(-2 * PI * 100.0 / rate), alp = std::exp(-2 * PI * 800.0 / rate), aenv = std::exp(-2 * PI * 4.0 / rate);
  for (int k = 0; k < K; k++) {
    double hp_x = 0, hp_y = 0, l1 = 0, l2 = 0, e1 = 0, e2 = 0;
    for (size_t i = 0; i < n; i++) {
      double w = r.gauss();
      double hp = ahp * (hp_y + w - hp_x); hp_x = w; hp_y = hp;
      l1 = alp * l1 + (1 - alp) * hp; l2 = alp * l2 + (1 - alp) * l1;
      double ew = r.gauss();
      e1 = aenv * e1 + (1 - aenv) * ew; e2 = aenv * e2 + (1 - aenv) * e1;
      double env = std::fabs(e2);  // absolute scale does not matter: the realisation is normalised to the target power
      out[i] += l2 * env;
    }
  }
}

static const double REF_RMS = 0.01;  // received signal RMS (sound-on part) = -40 dBFS before noise; leaves 30 dB of headroom for noise

struct Received {
  std::vector<int16_t> pcm;
  size_t lead = 0;       // samples before the first message starts
  double clip_frac = 0;  // fraction of samples clipped at int16
};
// One trial: lead silence + reps x message (+ 350 ms gap between ggwave repeats; bysound laps already end in their 350 ms gap)
// + 1 s tail; optional echo; optional drift; gain to REF_RMS; noise at `snr_db` (full band, 0..fs/2) unless clean.
static Received channel(const Wave &w, Sys sys, int reps, unsigned rate, const Cond &c, double snr_db, bool clean, uint64_t seed) {
  Rng r(seed);
  Received out;
  size_t lead = (size_t)((0.25 + r.unif()) * rate);
  size_t gap = sys == GGWAVE && reps > 1 ? (size_t)(0.35 * rate) : 0;
  size_t tail = rate;
  std::vector<float> x(lead, 0.0f);
  for (int k = 0; k < reps; k++) {
    if (k) x.insert(x.end(), gap, 0.0f);
    x.insert(x.end(), w.x.begin(), w.x.end());
  }
  x.insert(x.end(), tail, 0.0f);
  if (c.echo) {  // direct path + 0.5 at 8 ms + 0.25 at 23 ms
    size_t d1 = (size_t)std::lround(0.008 * rate), d2 = (size_t)std::lround(0.023 * rate);
    std::vector<float> y(x);
    for (size_t i = 0; i < x.size(); i++) {
      if (i >= d1) y[i] += 0.5f * x[i - d1];
      if (i >= d2) y[i] += 0.25f * x[i - d2];
    }
    x.swap(y);
  }
  if (c.ppm != 0) { x = resample(x, c.ppm); lead = (size_t)std::lround(lead / (1.0 + c.ppm * 1e-6)); }
  // signal power over the sound-on samples only (lead, tail, and inter-message gaps excluded)
  double e = 0;
  for (float v : x) e += (double)v * v;
  double active = (double)w.active * reps / (1.0 + c.ppm * 1e-6);
  double ps = e / active;
  double g = REF_RMS / std::sqrt(ps);
  std::vector<double> noise;
  double sigma = 0;
  if (!clean) {
    double pn = REF_RMS * REF_RMS / std::pow(10.0, snr_db / 10.0);
    gen_noise(c.noise, x.size(), r, noise, rate);
    double en = 0;
    if (c.noise == 0) en = 1.0;  // unit-variance white Gaussian: use the nominal variance
    else { for (double v : noise) en += v * v; en /= (double)noise.size(); }  // coloured: normalise this realisation
    sigma = std::sqrt(pn / en);
  }
  out.pcm.resize(x.size());
  size_t clipped = 0;
  for (size_t i = 0; i < x.size(); i++) {
    double v = x[i] * g + (clean ? 0.0 : noise[i] * sigma);
    double s = std::nearbyint(v * 32768.0);
    if (s > 32767) { s = 32767; clipped++; } else if (s < -32768) { s = -32768; clipped++; }
    out.pcm[i] = (int16_t)s;
  }
  out.lead = lead;
  out.clip_frac = (double)clipped / (double)x.size();
  return out;
}

// ---------------------------------------------------------------- receivers
struct RxResult {
  int n_correct = 0, n_wrong = 0, n_failed = 0;  // n_failed: decoder said "detected but could not decode" (ggwave -1)
  long first_ok_sample = -1;                      // samples pushed when the first correct payload came out
  double cpu_s = 0;
};
static int g_verbose = 0;
static double g_snr_override = NAN;  // replay only: run the trial with its condition's noise at this SNR instead of the cell's (same seed)
static void dump(const char *tag, const char *d, size_t n) {
  printf("    %s (%zu bytes): \"", tag, n);
  for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)d[i]; if (c >= 0x20 && c < 0x7f && c != '"') putchar(c); else printf("\\x%02x", c); }
  printf("\"\n");
}
static double cpu_now() { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static const size_t CHUNK = 1024;

struct BysCtx { const std::string *expect; RxResult *res; long pushed; };
static void bys_cb(void *user, const bys_demod_event_t *e) {
  BysCtx *c = (BysCtx *)user;
  if (g_verbose) printf("  bysound event type %d (1 frame, 2 header, 3 clash) band %d profile %d at sample %ld\n", e->type, e->band, e->profile, c->pushed);
  if (e->type != BYS_EV_FRAME) return;
  if (e->result->kind != BYS_KIND_PAYLOAD || !e->payload) { c->res->n_wrong++; return; }  // ack/reference/risk out of noise counts as false
  static char text[300000];
  int n = bys_unpack(e->payload, e->payload_len, text, sizeof text);
  if (g_verbose) dump("decoded", text, n > 0 ? (size_t)n : 0);
  if (c->expect && n >= 0 && (size_t)n == c->expect->size() && !memcmp(text, c->expect->data(), (size_t)n)) {
    if (c->res->n_correct++ == 0) c->res->first_ok_sample = c->pushed;
  } else c->res->n_wrong++;
}
static bys_demod_t *g_demod;
static uint8_t g_demod_out[255 * 48 + 8];
// bysound receiver listens to every channel its profile has (normal: 7 channels; robust: top, high, upper, mid): it does not
// know in advance which one the sender uses, like ggwave's receiver, which by default tries every protocol.
static RxResult rx_bys(const std::vector<int16_t> &pcm, unsigned rate, int profile, const std::string *expect, uint8_t band_mask = 0) {
  RxResult res;
  if (!g_demod) g_demod = (bys_demod_t *)calloc(1, sizeof(bys_demod_t));
  BysCtx ctx{ expect, &res, 0 };
  double t0 = cpu_now();
  bys_demod_cfg_t cfg; bys_demod_cfg_default(&cfg);
  cfg.sample_rate = rate; cfg.profile = (bys_profile_t)profile; cfg.band_mask = band_mask ? band_mask : profile ? 0x0f : 0x7f;
  cfg.out = g_demod_out; cfg.out_cap = sizeof g_demod_out; cfg.cb = bys_cb; cfg.user = &ctx;
  int r = bys_demod_init(g_demod, &cfg);
  if (r != BYS_OK) { fprintf(stderr, "bys_demod_init failed %d\n", r); exit(3); }
  for (size_t i = 0; i < pcm.size(); i += CHUNK) {
    size_t k = std::min(CHUNK, pcm.size() - i);
    ctx.pushed = (long)(i + k);
    bys_demod_push(g_demod, pcm.data() + i, k);
  }
  if (g_verbose) { const bys_demod_stats_t *st = bys_demod_stats(g_demod); printf("  bysound stats: frames %u headers %u failures %u clashes %u\n", st->frames, st->headers, st->failures, st->clashes); }
  res.cpu_s = cpu_now() - t0;
  return res;
}
// ggwave receiver: library defaults (48 kHz, 1024 samples per frame, every protocol enabled, variable-length payloads);
// input format set to int16 because that is what the channel produces. freq > 0: only DT/MT, moved to that bin (gg_protocols).
static RxResult rx_gg(const std::vector<int16_t> &pcm, int fixed_len, const std::string *expect, int freq = 0) {
  RxResult res;
  double t0 = cpu_now();
  ggwave_Parameters p = ggwave_getDefaultParameters();
  p.payloadLength = fixed_len > 0 ? fixed_len : -1;
  p.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
  p.operatingMode = GGWAVE_OPERATING_MODE_RX;
  gg_protocols(freq);
  int inst = ggwave_init(p);
  gg_protocols(0);
  if (inst < 0) { fprintf(stderr, "ggwave_init failed\n"); exit(3); }
  static char buf[1024];
  for (size_t i = 0; i < pcm.size(); i += CHUNK) {
    size_t k = std::min(CHUNK, pcm.size() - i);
    int n = ggwave_ndecode(inst, pcm.data() + i, (int)(k * sizeof(int16_t)), buf, sizeof buf);
    if (g_verbose && n != 0) { printf("  ggwave ndecode -> %d at sample %zu\n", n, i + k); if (n > 0) dump("decoded", buf, (size_t)n); }
    if (n == -1) res.n_failed++;
    else if (n > 0) {
      if (expect && (size_t)n == expect->size() && !memcmp(buf, expect->data(), (size_t)n)) {
        if (res.n_correct++ == 0) res.first_ok_sample = (long)(i + k);
      } else res.n_wrong++;
    }
  }
  ggwave_free(inst);
  res.cpu_s = cpu_now() - t0;
  return res;
}

// ---------------------------------------------------------------- plans
struct Cell {
  std::string plan;
  std::string seed_plan;  // plan name used in the seed key (empty = plan): the inaudible plan reuses the snr/cond/size seeds
  Config cfg;
  unsigned rate;
  std::string size;   // payload label
  std::string cond;
  double snr;         // dB; ignored when clean
  bool clean;
  int reps;
  int trials;
};
static std::vector<double> grid(double a, double b, double step) { std::vector<double> v; for (double s = a; s <= b + 1e-9; s += step) v.push_back(s); return v; }

static std::vector<Cell> make_plan(const std::string &plan) {
  std::vector<Cell> cells;
  auto all = bys_configs();
  auto gg = gg_configs(false);
  all.insert(all.end(), gg.begin(), gg.end());
  std::string seed_plan;
  auto add = [&](const std::string &pl, const Config &c, unsigned rate, const std::string &size, const std::string &cond, const std::vector<double> &snrs, bool with_clean, int reps, int trials) {
    for (double s : snrs) cells.push_back(Cell{ pl, seed_plan, c, rate, size, cond, s, false, reps, trials });
    if (with_clean) cells.push_back(Cell{ pl, seed_plan, c, rate, size, cond, 0, true, reps, trials });
  };
  const bool everything = plan == "all";
  // 1. main curve: AWGN, 32-byte ASCII, 48 kHz, single and double transmission
  if (everything || plan == "snr") {
    auto g = grid(-36, 4, 2);
    for (auto &c : all) for (int reps = 1; reps <= 2; reps++) add("snr", c, 48000, "32", "awgn", g, true, reps, 50);
  }
  // 2. payload size: AWGN, single transmission
  if (everything || plan == "size") {
    auto g = grid(-32, 4, 4);
    for (auto &c : all) for (const char *sz : { "8", "64", "128", "utf8" }) add("size", c, 48000, sz, "awgn", g, true, 1, 30);
    auto gl = std::vector<double>{ -24, -20, -16, -12, -8, 0 };
    // bysound's encoder refuses what would last more than 30 s on that configuration (BYS_E_TOO_LONG): those cells are not run
    // and airtime.csv reports them as "exceeds 30 s cap".
    for (auto &c : bys_configs()) for (const char *sz : { "512", "2048" }) {
      if (!encode(c, make_payload(sz, 0), 48000).ok) continue;
      add("size", c, 48000, sz, "awgn", gl, true, 1, sz[0] == '5' ? 20 : 10);
    }
  }
  // 3. impairments: echo, coloured noise, clock drift and a combination, 32 bytes, single and double transmission
  if (everything || plan == "cond") {
    auto g = grid(-32, 4, 4);
    for (const char *cd : { "echo", "pink", "babble", "drift+100", "drift-100", "combo" })
      for (auto &c : all) for (int reps = 1; reps <= 2; reps++) add("cond", c, 48000, "32", cd, g, true, reps, 30);
  }
  // 4. bysound at 44.1 kHz (ggwave's API resamples internally; not run here)
  if (everything || plan == "rate44") {
    auto g = grid(-32, 4, 4);
    for (auto &c : bys_configs()) add("rate44", c, 44100, "32", "awgn", g, true, 1, 30);
  }
  // 5. ggwave mono-tone protocols, which exist only in fixed-length mode (receiver must know the payload length in advance)
  if (everything || plan == "mtfixed") {
    auto g = grid(-32, 4, 4);
    for (auto &c : gg_configs(true)) if (c.fixed) for (const char *sz : { "8", "32", "64" }) add("mtfixed", c, 48000, sz, "awgn", g, true, 1, 30);
  }
  // 6. inaudible channels: bysound top (normal, robust) vs ggwave DT/MT moved to 18 kHz. Same grids, trial counts and seeds as the
  // snr (1 transmission), cond (1 transmission) and size plans, so the bysound rows reproduce those plans exactly.
  if (plan == "inaudible") {
    auto cfgs = bys_top_configs();
    auto gi = gg_inaudible_configs();
    cfgs.insert(cfgs.end(), gi.begin(), gi.end());
    seed_plan = "snr";
    for (auto &c : cfgs) add("inaudible-snr", c, 48000, "32", "awgn", grid(-36, 4, 2), true, 1, 50);
    seed_plan = "cond";
    for (const char *cd : { "echo", "pink", "babble", "drift+100", "drift-100", "combo" })
      for (auto &c : cfgs) add("inaudible-cond", c, 48000, "32", cd, grid(-32, 4, 4), true, 1, 30);
    seed_plan = "size";
    for (auto &c : cfgs) for (const char *sz : { "8", "32", "64", "128", "140" }) {
      if (!encode(c, make_payload(sz, 0), 48000).ok) continue;  // MT fixed-length mode stops at 64 B
      add("inaudible-size", c, 48000, sz, "awgn", grid(-32, 4, 4), true, 1, 30);
    }
    seed_plan.clear();
  }
  if (plan == "quick") {
    for (auto &c : all) add("quick", c, 48000, "32", "awgn", { -12, -4 }, true, 1, 2);
  }
  return cells;
}

struct Job { size_t cell; int trial; };
static std::vector<Job> jobs_of(const std::vector<Cell> &cells) {
  std::vector<Job> j;
  for (size_t i = 0; i < cells.size(); i++) for (int t = 0; t < cells[i].trials; t++) j.push_back(Job{ i, t });
  return j;
}

static std::string fmt(double v, int prec = 6) { char b[64]; snprintf(b, sizeof b, "%.*g", prec, v); return b; }

static int run_trial(FILE *f, const Cell &c, int t);
static void run_plan(const std::string &plan, int shard, int nshards, const char *outpath) {
  auto cells = make_plan(plan);
  auto jobs = jobs_of(cells);
  FILE *f = fopen(outpath, "w");
  if (!f) { perror(outpath); exit(1); }
  fprintf(f, "plan,system,config,rate,size,payload_bytes,condition,snr_db,clean,reps,trial,success,n_correct,n_wrong,n_failed,air_s,active_s,latency_s,audio_s,cpu_s,clip_frac\n");
  size_t done = 0;
  for (size_t k = (size_t)shard; k < jobs.size(); k += (size_t)nshards) {
    run_trial(f, cells[jobs[k].cell], jobs[k].trial);
    if (++done % 200 == 0) fflush(f);
  }
  fclose(f);
}

static int run_trial(FILE *f, const Cell &c, int t) {
  {
    std::string msg = make_payload(c.size, t);
    Wave w = encode(c.cfg, msg, c.rate);
    // same seed for both systems: the key leaves out the system and configuration
    std::string key = (c.seed_plan.empty() ? c.plan : c.seed_plan) + "|" + std::to_string(c.rate) + "|" + c.size + "|" + c.cond + "|" + (c.clean ? std::string("clean") : fmt(c.snr)) + "|" + std::to_string(c.reps) + "|" + std::to_string(t);
    double air = 0, act = 0;
    int success = 0;
    RxResult rr;
    Received rx;
    if (w.ok) {
      bool ov = !std::isnan(g_snr_override);
      rx = channel(w, c.cfg.sys, c.reps, c.rate, cond_of(c.cond), ov ? g_snr_override : c.snr, ov ? false : c.clean, fnv64(key));
      size_t gap = c.cfg.sys == GGWAVE && c.reps > 1 ? (size_t)(0.35 * c.rate) * (size_t)(c.reps - 1) : 0;
      air = (double)(w.x.size() * (size_t)c.reps + gap) / c.rate;
      act = (double)w.active * c.reps / c.rate;
      if (c.cfg.sys == BYSOUND) rr = rx_bys(rx.pcm, c.rate, c.cfg.profile, &msg);
      else rr = rx_gg(rx.pcm, c.cfg.fixed ? (int)msg.size() : 0, &msg, c.cfg.freq);
      success = rr.n_correct > 0;
    }
    double lat = rr.first_ok_sample >= 0 ? (double)((long)rr.first_ok_sample - (long)rx.lead) / c.rate : -1;
    fprintf(f, "%s,%s,%s,%u,%s,%zu,%s,%s,%d,%d,%d,%d,%d,%d,%d,%s,%s,%s,%s,%s,%s\n", c.plan.c_str(), c.cfg.sys == BYSOUND ? "bysound" : "ggwave",
            c.cfg.name.c_str(), c.rate, c.size.c_str(), msg.size(), c.cond.c_str(), c.clean ? "" : fmt(c.snr).c_str(), c.clean ? 1 : 0, c.reps, t,
            w.ok ? success : -1, rr.n_correct, rr.n_wrong, rr.n_failed, fmt(air).c_str(), fmt(act).c_str(), fmt(lat).c_str(),
            fmt((double)rx.pcm.size() / c.rate).c_str(), fmt(rr.cpu_s).c_str(), fmt(rx.clip_frac, 3).c_str());
    if (g_verbose) { printf("  lead %zu samples, %zu samples total, clip %.4f\n", rx.lead, rx.pcm.size(), rx.clip_frac); dump("sent", msg.data(), msg.size()); }
    return w.ok ? success : -1;
  }
}

// ggwave DT failures at the noiseless point: every clean-point trial of a ggwave DT configuration in the given plans is run as in the
// plan and, if it fails, again with the condition's noise at +60 dB SNR (-100 dBFS RMS for a -40 dBFS signal, about the int16 quantisation noise) and the same seed.
static void dither_check(int nplans, char **plans) {
  FILE *nul = fopen("/dev/null", "w");
  for (int i = 0; i < nplans; i++) {
    std::map<std::string, std::vector<int>> per;  // config -> clean trials, failures, failures decoded with +60 dB noise
    std::vector<std::string> order;
    for (auto &c : make_plan(plans[i])) {
      if (!c.clean || c.cfg.sys != GGWAVE || c.cfg.name.find(" dt-") == std::string::npos) continue;
      if (!per.count(c.cfg.name)) { per[c.cfg.name] = { 0, 0, 0 }; order.push_back(c.cfg.name); }
      auto &v = per[c.cfg.name];
      for (int t = 0; t < c.trials; t++) {
        g_snr_override = NAN;
        int s0 = run_trial(nul, c, t);
        if (s0 < 0) continue;
        v[0]++;
        if (s0) continue;
        v[1]++;
        g_snr_override = 60;
        v[2] += run_trial(nul, c, t) == 1;
        g_snr_override = NAN;
      }
    }
    for (auto &n : order) printf("%-26s plan %-9s: %d clean trials, %d failed, %d of those decoded with noise at +60 dB SNR\n", n.c_str(), plans[i], per[n][0], per[n][1], per[n][2]);
  }
  fclose(nul);
}

// Replays one trial of a plan verbosely (same seed as in the CSV): bench trial <plan> "<config>" <size> <condition> <snr|clean> <reps> <trial>
// [noise_snr]: with the extra argument, the same trial (payload, lead silence, seed) is run with the condition's noise at noise_snr dB,
// e.g. a clean-point trial at +60 dB (-100 dBFS RMS, about the int16 quantisation noise) to check whether its failure needs exact digital silence.
static void replay(const char *plan, const char *config, const char *size, const char *cond, const char *snr, int reps, int trial) {
  // the inaudible plan holds three sub-plans: "inaudible-snr", "inaudible-cond" or "inaudible-size" (the CSV's plan column) selects one
  std::string pl(plan), base = pl.rfind("inaudible-", 0) == 0 ? "inaudible" : pl;
  for (auto &c : make_plan(base)) {
    if (base != pl && c.plan != pl) continue;
    if (c.cfg.name != config || c.size != size || c.cond != cond || c.reps != reps) continue;
    if (!strcmp(snr, "clean") ? !c.clean : (c.clean || fmt(c.snr) != fmt(atof(snr)))) continue;
    g_verbose = 1;
    printf("replaying %s | %s | %s | %s | %s | reps %d | trial %d\n", plan, config, size, cond, snr, reps, trial);
    run_trial(stdout, c, trial);
    return;
  }
  fprintf(stderr, "no such cell\n");
  exit(2);
}

// Minimal bysound repro with no channel at all: the encoder's int16 output, `lead` zero samples before it and 1 s of zeros after,
// pushed into a fresh demodulator in `chunk`-sample pieces. Also decodes the same frame at the symbol level (bys_frame_next ->
// bys_rx_push, no audio) to separate the codec from the demodulator.
static void repro_bys(int band, int profile, const char *size, int trial, int lead, int chunk) {
  std::string msg = make_payload(size, trial);
  dump("payload", msg.data(), msg.size());
  bys_tx_cfg_t cfg; bys_tx_cfg_default(&cfg); cfg.sample_rate = 48000;
  bys_tx_t tx;
  int r = bys_tx_init_payload(&tx, &cfg, (bys_band_t)band, (bys_profile_t)profile, msg.data(), msg.size());
  printf("  bys_tx_init_payload -> %d, %u samples\n", r, bys_tx_total_samples(&tx));
  std::vector<int16_t> pcm((size_t)lead, 0);
  int16_t buf[4096];
  while (!bys_tx_done(&tx)) { size_t k = bys_tx_fill(&tx, buf, 4096); pcm.insert(pcm.end(), buf, buf + k); if (!k) break; }
  pcm.insert(pcm.end(), 48000, 0);
  // symbol level
  {
    bys_frame_t fr;
    bys_frame_init_payload(&fr, (bys_band_t)band, (bys_profile_t)profile, msg.data(), msg.size());
    static uint8_t out[255 * 48 + 8];
    bys_rx_t rx; bys_rx_init(&rx, (bys_band_t)band, (bys_profile_t)profile, out, sizeof out);
    uint8_t nib[BYS_MAX_GROUPS];
    unsigned k = 0; int st = BYS_RX_MORE;
    while (bys_frame_next(&fr, nib)) { if (k++ < BYS_PREAMBLE_SYMBOLS) continue; if (st == BYS_RX_MORE) st = bys_rx_push(&rx, nib); }
    if (st == BYS_RX_MORE) st = bys_rx_finish(&rx);
    static char text[300000];
    int n = st == BYS_RX_DONE ? bys_unpack(out, bys_rx_result(&rx)->payload_len, text, sizeof text) : -1;
    printf("  symbol-level decode (no audio): status %d, %s\n", st, n == (int)msg.size() && !memcmp(text, msg.data(), (size_t)n) ? "payload matches" : "payload MISMATCH");
  }
  g_verbose = 1;
  if (!g_demod) g_demod = (bys_demod_t *)calloc(1, sizeof(bys_demod_t));
  RxResult res;
  BysCtx ctx{ &msg, &res, 0 };
  bys_demod_cfg_t dc; bys_demod_cfg_default(&dc);
  dc.profile = (bys_profile_t)profile; dc.band_mask = (uint8_t)(1u << band);
  dc.out = g_demod_out; dc.out_cap = sizeof g_demod_out; dc.cb = bys_cb; dc.user = &ctx;
  if (bys_demod_init(g_demod, &dc) != BYS_OK) { printf("  demod init failed\n"); return; }
  for (size_t i = 0; i < pcm.size(); i += (size_t)chunk) { size_t k = std::min((size_t)chunk, pcm.size() - i); ctx.pushed = (long)(i + k); bys_demod_push(g_demod, pcm.data() + i, k); }
  const bys_demod_stats_t *st = bys_demod_stats(g_demod);
  printf("  audio decode (only band %d listened): correct %d, wrong %d; demod stats frames %u headers %u failures %u\n", band, res.n_correct, res.n_wrong, st->frames, st->headers, st->failures);
}

// Symbol-level round trip (no audio): bys_frame_next -> bys_rx_push -> bys_unpack, for n random payloads of one size.
static int codec_ok(int band, int profile, const std::string &msg, int *status) {
  bys_frame_t fr;
  if (bys_frame_init_payload(&fr, (bys_band_t)band, (bys_profile_t)profile, msg.data(), msg.size()) != BYS_OK) { *status = -100; return 0; }
  static uint8_t out[255 * 48 + 8];
  bys_rx_t rx; bys_rx_init(&rx, (bys_band_t)band, (bys_profile_t)profile, out, sizeof out);
  uint8_t nib[BYS_MAX_GROUPS];
  unsigned k = 0; int st = BYS_RX_MORE;
  unsigned fail_at = 0;
  while (bys_frame_next(&fr, nib)) { if (k++ < BYS_PREAMBLE_SYMBOLS) continue; if (st == BYS_RX_MORE) { st = bys_rx_push(&rx, nib); if (st < 0) fail_at = k; } }
  if (st == BYS_RX_MORE) { st = bys_rx_finish(&rx); if (st < 0) fail_at = k + 1; }
  *status = st;
  if (st < 0 && g_verbose) printf("    round trip failed with %d at symbol %u of %u (%u = in bys_rx_finish)\n", st, fail_at, k, k + 1);
  static char text[300000];
  int n = st == BYS_RX_DONE ? bys_unpack(out, bys_rx_result(&rx)->payload_len, text, sizeof text) : -1;
  return n == (int)msg.size() && !memcmp(text, msg.data(), (size_t)n);
}
// Payloads of bug 1 (fixed): the last block's final Reed-Solomon parity byte is 0x00. Then the block minus that byte is also a valid
// codeword (c(x) = x c'(x) keeps the roots alpha^0..alpha^11). bys_rx_finish used to stop at the first length RS accepted, closing
// the block one byte short and failing the CRC-32; it now also requires the payload's CRC-32. The scan counts these payloads.
static int last_parity_zero(const std::string &msg) {
  static uint8_t packed[300000];
  int n = bys_pack(msg.data(), msg.size(), packed, sizeof packed - 4);
  if (n < 0) return -1;
  uint32_t crc = bys_crc32(packed, (size_t)n);
  packed[n] = (uint8_t)(crc >> 24); packed[n + 1] = (uint8_t)(crc >> 16); packed[n + 2] = (uint8_t)(crc >> 8); packed[n + 3] = (uint8_t)crc;
  size_t total = (size_t)n + 4, blocks = (total + BYS_BLOCK_DATA - 1) / BYS_BLOCK_DATA, last = total - (blocks - 1) * BYS_BLOCK_DATA;
  uint8_t blk[BYS_BLOCK_BYTES], par[BYS_BLOCK_PARITY];
  blk[0] = (uint8_t)(blocks - 1); blk[1] = (uint8_t)blocks;
  memcpy(blk + 2, packed + (blocks - 1) * BYS_BLOCK_DATA, last);
  bys_rs_encode(blk, 2 + last, par, BYS_BLOCK_PARITY);
  return par[BYS_BLOCK_PARITY - 1] == 0;
}
static void codec_scan(const char *size, int n) {
  for (auto &c : bys_configs()) {
    int fails = 0, predicted = 0, agree = 0; std::string which;
    for (int t = 0; t < n; t++) {
      int st; std::string msg = make_payload(size, t);
      if (t == 0) { bys_frame_t fr; if (bys_frame_init_payload(&fr, (bys_band_t)c.band, (bys_profile_t)c.profile, msg.data(), msg.size()) == BYS_E_TOO_LONG) { fails = -1; break; } }
      int bad = !codec_ok(c.band, c.profile, msg, &st), pz = last_parity_zero(msg) == 1;
      predicted += pz; agree += bad == pz;
      if (bad) { fails++; if (which.size() < 80) which += " " + std::to_string(t) + "(status " + std::to_string(st) + ")"; }
    }
    if (fails < 0) { printf("%-22s %s B: exceeds 30 s cap (not run)\n", c.name.c_str(), size); continue; }
    printf("%-22s %s B: %d of %d payloads fail the symbol-level round trip%s%s | last parity byte 0x00 in %d; prediction matches %d of %d\n",
           c.name.c_str(), size, fails, n, fails ? ":" : "", which.c_str(), predicted, agree, n);
  }
}

// Fraction of random payloads that a noiseless channel fails to deliver, per configuration of both systems (encoder output at
// full scale straight into the receiver, 12345 zero samples before it and 1 s after, 1024-sample chunks).
// inaudible: bysound top only and ggwave DT/MT moved to 18 kHz (receiver restricted to them).
static void scan_bys(const char *size, int n, bool inaudible) {
  auto all = inaudible ? bys_top_configs() : bys_configs();
  auto gg = inaudible ? gg_inaudible_configs() : gg_configs(false);
  all.insert(all.end(), gg.begin(), gg.end());
  for (auto &c : all) {
    int fails = 0, wrong = 0; std::string which;
    if (!encode(c, make_payload(size, 0), 48000).ok) { printf("%-26s %s B: cannot send\n", c.name.c_str(), size); continue; }
    for (int t = 0; t < n; t++) {
      std::string msg = make_payload(size, t);
      Wave w = encode(c, msg, 48000);
      if (!w.ok) break;
      std::vector<int16_t> pcm(12345, 0);
      for (float v : w.x) pcm.push_back((int16_t)std::lrint(v * 32768.0f));
      pcm.insert(pcm.end(), 48000, 0);
      RxResult rr = c.sys == BYSOUND ? rx_bys(pcm, 48000, c.profile, &msg) : rx_gg(pcm, c.fixed ? (int)msg.size() : 0, &msg, c.freq);
      wrong += rr.n_wrong;
      if (!rr.n_correct) { fails++; if (which.size() < 60) which += " " + std::to_string(t); }
    }
    printf("%-26s %s B: %d of %d payloads not delivered without noise, %d wrong payloads accepted%s%s\n", c.name.c_str(), size, fails, n, wrong, fails ? "; trials:" : "", which.c_str());
  }
}

// ---------------------------------------------------------------- noise-only false detections
// Each receiver listens to 10 segments x 60 s of each noise type at -20 dBFS RMS (no transmission at all).
// inaudible: bysound listening to top only, and ggwave restricted to DT/MT at 18 kHz (variable length, and fixed length 32 B).
static void run_idle(int shard, int nshards, const char *outpath, bool inaudible) {
  FILE *f = fopen(outpath, "w");
  if (!f) { perror(outpath); exit(1); }
  fprintf(f, "receiver,noise,segment,seconds,detections,failed,cpu_s\n");
  const char *rxs[] = { "bysound normal (7 channels)", "bysound robust (4 channels)", "ggwave (all protocols; variable length)" };
  const char *rxi18[] = { "bysound normal (top only)", "bysound robust (top only)", "ggwave DT/MT @18k only (variable length)", "ggwave DT/MT @18k only (fixed length 32 B)" };
  const char *nn[] = { "white", "pink", "babble" };
  int job = 0, nrx = inaudible ? 4 : 3;
  for (int rxi = 0; rxi < nrx; rxi++) for (int nt = 0; nt < 3; nt++) for (int seg = 0; seg < 10; seg++, job++) {
    if (job % nshards != shard) continue;
    Rng r(fnv64("idle|" + std::string(nn[nt]) + "|" + std::to_string(seg)));
    size_t n = 60 * 48000;
    std::vector<double> noise;
    gen_noise(nt, n, r, noise, 48000);
    double en = 0; for (double v : noise) en += v * v; en /= (double)n;
    double g = 0.1 / std::sqrt(en);
    std::vector<int16_t> pcm(n);
    for (size_t i = 0; i < n; i++) { double s = std::nearbyint(noise[i] * g * 32768.0); pcm[i] = (int16_t)std::max(-32768.0, std::min(32767.0, s)); }
    RxResult rr;
    if (!inaudible) rr = rxi < 2 ? rx_bys(pcm, 48000, rxi, nullptr) : rx_gg(pcm, 0, nullptr);
    else rr = rxi < 2 ? rx_bys(pcm, 48000, rxi, nullptr, 0x01) : rx_gg(pcm, rxi == 3 ? 32 : 0, nullptr, GG_BIN_18K);
    fprintf(f, "%s,%s,%d,60,%d,%d,%s\n", inaudible ? rxi18[rxi] : rxs[rxi], nn[nt], seg, rr.n_wrong + rr.n_correct, rr.n_failed, fmt(rr.cpu_s).c_str());
  }
  fclose(f);
}

// ---------------------------------------------------------------- info: deterministic tables
static void fft(std::vector<double> &re, std::vector<double> &im) {
  size_t n = re.size();
  for (size_t i = 1, j = 0; i < n; i++) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); } }
  for (size_t len = 2; len <= n; len <<= 1) {
    double a = -2 * PI / (double)len;
    for (size_t i = 0; i < n; i += len) for (size_t k = 0; k < len / 2; k++) {
      double wr = std::cos(a * (double)k), wi = std::sin(a * (double)k);
      double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi, xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
      re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi; re[i + k] += xr; im[i + k] += xi;
    }
  }
}
// Welch PSD (4096-point Hann, 50 % overlap), one-sided, bins 0..2048.
static std::vector<double> psd(const float *x, size_t n) {
  const size_t N = 4096;
  std::vector<double> acc(N / 2 + 1, 0.0), re(N), im(N);
  for (size_t s = 0; s + N <= n; s += N / 2) {
    for (size_t i = 0; i < N; i++) { re[i] = x[s + i] * (0.5 - 0.5 * std::cos(2 * PI * (double)i / N)); im[i] = 0; }
    fft(re, im);
    for (size_t k = 0; k <= N / 2; k++) acc[k] += re[k] * re[k] + im[k] * im[k];
  }
  return acc;
}

static void info(const char *outdir) {
  std::string od(outdir);
  // airtime per configuration and payload size (no channel)
  FILE *f = fopen((od + "/airtime.csv").c_str(), "w");
  fprintf(f, "system,config,rate,size,payload_bytes,encoded,air_s,active_s,throughput_Bps,throughput_active_Bps,papr_db,status\n");
  auto all = bys_configs();
  auto gg = gg_configs(true);
  all.insert(all.end(), gg.begin(), gg.end());
  auto gi = gg_inaudible_configs();
  all.insert(all.end(), gi.begin(), gi.end());
  for (auto &c : all) for (unsigned rate : { 48000u, 44100u }) {
    if (rate == 44100 && c.sys == GGWAVE) continue;
    for (const char *sz : { "8", "16", "32", "48", "64", "96", "128", "140", "utf8", "256", "512", "1024", "2048" }) {
      std::string msg = make_payload(sz, 0);
      Wave w = encode(c, msg, rate);
      double peak = 0, e = 0;
      for (float v : w.x) { peak = std::max(peak, (double)std::fabs(v)); e += (double)v * v; }
      double papr = w.ok && e > 0 ? 10 * std::log10(peak * peak / (e / (double)w.active)) : 0;
      double air = (double)w.x.size() / rate, act = (double)w.active / rate;
      fprintf(f, "%s,%s,%u,%s,%zu,%d,%s,%s,%s,%s,%s,%s\n", c.sys == BYSOUND ? "bysound" : "ggwave", c.name.c_str(), rate, sz, msg.size(), w.ok ? 1 : 0,
              w.ok ? fmt(air).c_str() : "", w.ok ? fmt(act).c_str() : "", w.ok ? fmt(msg.size() / air).c_str() : "", w.ok ? fmt(msg.size() / act).c_str() : "",
              w.ok ? fmt(papr, 4).c_str() : "", w.ok ? "ok" : w.why ? w.why : "cannot send");
    }
  }
  fclose(f);

  // maximum payload of one transmission: largest ASCII size the encoder accepts (binary search over 1..20000). bysound: bounded by
  // its 30 s cap, per channel and profile; ggwave: its own 140 B (variable length) or 64 B (fixed length) limit.
  f = fopen((od + "/max_payload.csv").c_str(), "w");
  fprintf(f, "system,config,max_payload_bytes,air_s_at_max\n");
  for (auto &c : all) {
    int lo = 0, hi = 20000;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; Wave w = encode(c, make_payload(std::to_string(mid), 0), 48000); if (w.ok) lo = mid; else hi = mid - 1; }
    Wave w = encode(c, make_payload(std::to_string(lo), 0), 48000);
    fprintf(f, "%s,%s,%d,%s\n", c.sys == BYSOUND ? "bysound" : "ggwave", c.name.c_str(), lo, fmt((double)w.x.size() / 48000).c_str());
  }
  fclose(f);

  // occupied band (99 % of the signal power) and the in-band SNR offset for each noise type:
  // SNR_inband = SNR_fullband + 10 log10(P_sig_in_band / P_sig) - 10 log10(P_noise_in_band / P_noise)
  f = fopen((od + "/bands.csv").c_str(), "w");
  // frac_18k_19k5: share of the signal power in 18.0-19.5 kHz, taken as the 32 tone slots of bysound top (centres 18000 + 46.875 k Hz,
  // k = 0..31, each slot +-23.4 Hz: 17976.6-19476.6 Hz), the inaudible-channel check.
  fprintf(f, "system,config,rate,band_lo_hz,band_hi_hz,sig_frac,offset_white_db,offset_pink_db,offset_babble_db,frac_18k_19k5\n");
  std::vector<std::vector<double>> npsd[2];
  for (unsigned rate : { 48000u, 44100u }) {
    int ri = rate == 48000 ? 0 : 1;
    for (int nt = 0; nt < 3; nt++) {
      Rng r(fnv64("band-noise|" + std::to_string(nt) + "|" + std::to_string(rate)));
      std::vector<double> nz; gen_noise(nt, (size_t)rate * 60, r, nz, rate);
      std::vector<float> nf(nz.begin(), nz.end());
      npsd[ri].push_back(psd(nf.data(), nf.size()));
    }
  }
  for (auto &c : all) for (unsigned rate : { 48000u, 44100u }) {
    if (rate == 44100 && c.sys == GGWAVE) continue;
    int ri = rate == 48000 ? 0 : 1;
    Wave w = encode(c, make_payload("32", 0), rate);
    if (!w.ok) continue;
    std::vector<float> x(w.x.begin(), w.x.begin() + (long)w.active);
    x.resize(std::max(x.size(), (size_t)8192), 0.0f);
    auto p = psd(x.data(), x.size());
    double tot = 0; for (double v : p) tot += v;
    size_t lo = 0, hi = p.size() - 1; double cl = 0, ch = 0;
    while (lo < hi && cl + p[lo] < 0.005 * tot) cl += p[lo++];
    while (hi > lo && ch + p[hi] < 0.005 * tot) ch += p[hi--];
    double sin_ = 0; for (size_t k = lo; k <= hi; k++) sin_ += p[k];
    double hz = (double)rate / 4096;
    fprintf(f, "%s,%s,%u,%.0f,%.0f,%.4f", c.sys == BYSOUND ? "bysound" : "ggwave", c.name.c_str(), rate, lo * hz, hi * hz, sin_ / tot);
    for (int nt = 0; nt < 3; nt++) {
      auto &q = npsd[ri][(size_t)nt];
      double qt = 0, qi = 0; for (size_t k = 0; k < q.size(); k++) { qt += q[k]; if (k >= lo && k <= hi) qi += q[k]; }
      fprintf(f, ",%.2f", 10 * std::log10(sin_ / tot) - 10 * std::log10(qi / qt));
    }
    double s18 = 0; for (size_t k = 0; k < p.size(); k++) { double fk = (double)k * hz; if (fk >= 18000.0 - 46.875 / 2 && fk < 18000.0 + 31.5 * 46.875) s18 += p[k]; }
    fprintf(f, ",%.4f\n", s18 / tot);
  }
  fclose(f);

  // resampler check: a 19.5 kHz and a 1 kHz sine through +-100 ppm; worst amplitude error over the run
  f = fopen((od + "/resampler_check.txt").c_str(), "w");
  for (double fr : { 1000.0, 15000.0, 19500.0 }) for (double ppm : { -100.0, 100.0 }) {
    std::vector<float> s(48000 * 4);
    for (size_t i = 0; i < s.size(); i++) s[i] = (float)std::sin(2 * PI * fr * (double)i / 48000);
    auto o = resample(s, ppm);
    double worst = 0;
    for (size_t b = 48000; b + 4800 < o.size() - 48000; b += 480) {  // RMS over 100 ms blocks, away from the edges
      double e = 0; for (size_t i = b; i < b + 4800; i++) e += (double)o[i] * o[i];
      worst = std::max(worst, std::fabs(10 * std::log10(e / 4800 / 0.5)));
    }
    fprintf(f, "sine %.0f Hz, drift %+.0f ppm: worst 100 ms RMS deviation %.4f dB\n", fr, ppm, worst);
  }
  fprintf(f, "sizeof(bys_demod_t) = %zu bytes (FFT engine, robust + audible enabled, max rate %u)\n", sizeof(bys_demod_t), (unsigned)BYS_DEMOD_MAX_RATE);
  fclose(f);
}

int main(int argc, char **argv) {
  ggwave_setLogFile(getenv("BENCH_GGWAVE_LOG") ? stderr : nullptr);  // BENCH_GGWAVE_LOG=1: ggwave's own log on stderr (for replays)
  if (argc >= 2 && !strcmp(argv[1], "info") && argc == 3) { info(argv[2]); return 0; }
  if ((argc == 9 || argc == 10) && !strcmp(argv[1], "trial")) {
    if (argc == 10) g_snr_override = atof(argv[9]);
    replay(argv[2], argv[3], argv[4], argv[5], argv[6], atoi(argv[7]), atoi(argv[8])); return 0; }
  if (argc >= 6 && !strcmp(argv[1], "repro")) { repro_bys(atoi(argv[2]), atoi(argv[3]), argv[4], atoi(argv[5]), argc > 6 ? atoi(argv[6]) : 0, argc > 7 ? atoi(argv[7]) : 1024); return 0; }
  if (argc == 4 && !strcmp(argv[1], "codec")) { g_verbose = 1; codec_scan(argv[2], atoi(argv[3])); return 0; }
  if (argc >= 3 && !strcmp(argv[1], "dither")) { dither_check(argc - 2, argv + 2); return 0; }
  if ((argc == 4 || argc == 5) && !strcmp(argv[1], "scan")) { scan_bys(argv[2], atoi(argv[3]), argc == 5 && !strcmp(argv[4], "inaudible")); return 0; }
  if (argc >= 2 && !strcmp(argv[1], "jobs")) {
    std::string plan = argc >= 3 ? argv[2] : "all";
    if (plan == "idle") { printf("90\n"); return 0; }
    if (plan == "inaudible-idle") { printf("120\n"); return 0; }
    printf("%zu\n", jobs_of(make_plan(plan)).size());
    return 0;
  }
  if (argc == 6 && !strcmp(argv[1], "run")) {
    std::string plan = argv[2];
    int shard = atoi(argv[3]), ns = atoi(argv[4]);
    if (plan == "idle" || plan == "inaudible-idle") run_idle(shard, ns, argv[5], plan == "inaudible-idle");
    else run_plan(plan, shard, ns, argv[5]);
    return 0;
  }
  fprintf(stderr, "usage: bench info <outdir> | bench jobs [plan] | bench run <plan> <shard> <nshards> <out.csv> | bench scan <size> <n> [inaudible]\n");
  return 2;
}
