/*
 * bysound: bysound v4 in C99 for microcontrollers (ESP32-S3, C3, C6). Codec, frame, transmitter, symbol-to-byte decoding and
 * streaming demodulator. No malloc: the caller provides all state. Normative source: protocol-v4.json (repo root).
 *
 * State sizes (64-bit host; on 32-bit ESP32 they are somewhat smaller): see test_sizes in `make test`.
 */
#ifndef BYS_H
#define BYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Constants (protocol-v4.json; test_constants compares them with the JSON) ---------- */
#define BYS_VERSION            4
#define BYS_SPACING_MHZ        46875u /* 46.875 Hz in millihertz */
#define BYS_TONES              16
#define BYS_RAMP_MS            3
#define BYS_GAP_MS             350
#define BYS_HOP_MS             4
#define BYS_PREAMBLE_THRESHOLD_PERMILLE 320
#define BYS_HEADER_BYTES       4
#define BYS_MAGIC_PAYLOAD      86
#define BYS_MAGIC_ACK          65
#define BYS_MAGIC_REFERENCE    82
#define BYS_MAGIC_RISK         88
#define BYS_REF_BYTES          12
#define BYS_REF_DATA           14
#define BYS_REF_PARITY         16
#define BYS_MOTHER_PARITY      32
#define BYS_MOTHER_BYTES       (BYS_REF_DATA + BYS_MOTHER_PARITY)
#define BYS_BLOCK_DATA         48
#define BYS_BLOCK_PARITY       12
#define BYS_BLOCK_HEADER       2
#define BYS_BLOCK_BYTES        (BYS_BLOCK_HEADER + BYS_BLOCK_DATA + BYS_BLOCK_PARITY)
#define BYS_MAX_BLOCKS         255
#define BYS_MAX_FRAME_SECONDS  30 /* hard cap: preamble + header + body + gap of one payload transmission */
#define BYS_RISK_LENGTH        10
#define BYS_RISK_PARITY        8
#define BYS_RISK_BODY          (BYS_RISK_LENGTH + BYS_RISK_PARITY)
#define BYS_RISK_MAX_HOPS      2
#define BYS_ACK_LENGTH         7
#define BYS_ACK_PARITY         8
#define BYS_ACK_BODY           (BYS_ACK_LENGTH + BYS_ACK_PARITY)
#define BYS_PREAMBLE_SYMBOLS   8
#define BYS_MAX_GROUPS         6
#define BYS_RS_MAX_NSYM        32

/* Errors (negative). */
#define BYS_OK          0
#define BYS_E_ARG      (-1)  /* invalid argument */
#define BYS_E_PROFILE  (-2)  /* robust exists only on top, high, upper and mid */
#define BYS_E_RATE     (-3)  /* the sample rate is too low for the band */
#define BYS_E_TOO_LONG (-4)  /* more than 255 blocks, or one transmission longer than BYS_MAX_FRAME_SECONDS */
#define BYS_E_HEADER   (-5)  /* header: CRC-8, version, band or type do not check out */
#define BYS_E_RS       (-6)  /* Reed-Solomon could not correct */
#define BYS_E_CRC      (-7)  /* content CRC-16 or CRC-32 does not check out */
#define BYS_E_FORMAT   (-8)  /* length or fields outside what the type allows */
#define BYS_E_SPACE    (-9)  /* the caller's buffer is too small */
#define BYS_E_INCOMPLETE (-10) /* missing symbols */

/* Bands: the value is the band identifier carried in the header. `high` is the API's `ultrasonic`. */
typedef enum { BYS_TOP = 0, BYS_HIGH = 1, BYS_UPPER = 2, BYS_MID = 3, BYS_AUDIBLE = 4, BYS_XHIGH = 5, BYS_WIDE = 6, BYS_BAND_COUNT = 7 } bys_band_t;
typedef enum { BYS_NORMAL = 0, BYS_ROBUST = 1 } bys_profile_t;
/* Frame type = magic byte of the header. */
typedef enum { BYS_KIND_PAYLOAD = BYS_MAGIC_PAYLOAD, BYS_KIND_ACK = BYS_MAGIC_ACK, BYS_KIND_REFERENCE = BYS_MAGIC_REFERENCE, BYS_KIND_RISK = BYS_MAGIC_RISK } bys_kind_t;

typedef struct {
  uint16_t bin;            /* first grid bin: frequency = bin * 46.875 Hz */
  uint8_t groups;          /* simultaneous tones (nibbles per symbol) */
  uint16_t amp_permille;   /* amplitude of each tone, in thousandths of full scale */
  uint32_t symbol_us;      /* symbol duration (already doubled in robust) */
  uint32_t window_start_us;/* start of the analysis window (for the receiver) */
} bys_band_info_t;

/* Band data for a profile. */
int bys_band_info(bys_band_t band, bys_profile_t profile, bys_band_info_t *out);
/* Nibbles of row k (0..7) of the preamble for the band and profile; writes `groups` nibbles. */
int bys_preamble_symbol(bys_band_t band, bys_profile_t profile, unsigned k, uint8_t *nib);

/* ---------- CRC ---------- */
uint8_t  bys_crc8(const uint8_t *d, size_t n);          /* poly 0x2F, init 0xFF (header) */
uint16_t bys_crc16_update(uint16_t crc, uint8_t b);     /* CCITT 0x1021; start at 0xFFFF */
uint16_t bys_crc16(const uint8_t *d, size_t n);
uint32_t bys_crc32_update(uint32_t crc, uint8_t b);     /* start at 0xFFFFFFFF and finish with ^ 0xFFFFFFFF */
uint32_t bys_crc32(const uint8_t *d, size_t n);

/* ---------- Reed-Solomon GF(256), 0x11D, roots alpha^0.. (systematic) ---------- */
/* parity[nsym] of data[n]. n + nsym <= 255, nsym <= BYS_RS_MAX_NSYM. */
void bys_rs_encode(const uint8_t *data, size_t n, uint8_t *parity, size_t nsym);
/*
 * Corrects cw[n] in place (n includes the parity). `erasures`: positions known to be bad (index into cw). Corrects if
 * 2*errors + erasures <= nsym. Returns the number of bytes corrected or BYS_E_RS (cw is left intact). Stack scratch: about 330 bytes.
 */
int bys_rs_decode(uint8_t *cw, size_t n, size_t nsym, const uint8_t *erasures, size_t nerr);

/* ---------- Text packing (the one in v4-reference.mjs) ---------- */
/* Iterator: digit pairs in one byte (0x80+n), ASCII 0x20..0x7E as is, everything else 0xFF,length,UTF-8 in runs of <=255. */
typedef struct { const uint8_t *s; uint32_t n, i; uint16_t run; uint8_t stage; } bys_packer_t;
void bys_packer_init(bys_packer_t *p, const char *utf8, size_t n);
int  bys_packer_next(bys_packer_t *p); /* byte, or -1 at the end */
/* All at once: bytes written or BYS_E_SPACE. */
int  bys_pack(const char *utf8, size_t n, uint8_t *out, size_t cap);
/* Inverse. Writes UTF-8 with a trailing NUL if there is room; returns the length or an error (BYS_E_FORMAT if invalid). */
int  bys_unpack(const uint8_t *packed, size_t n, char *out, size_t cap);

/* ---------- v4 frame: symbol generator ---------- */
/*
 * A symbol = `groups` nibbles (one tone per nibble). A frame is: 8 preamble symbols, header (4 bytes, zero-padded to a whole
 * symbol) and body (same). It is generated symbol by symbol without storing the frame. The payload text must stay alive
 * while it is being sent.
 */
typedef struct {
  uint8_t band, profile, kind, groups;
  uint16_t sym_total, sym_k;
  uint8_t hdr_syms, half, cur, buf_len, buf_pos, blk, total, crc_i;
  uint16_t body_left;
  uint8_t hdr[BYS_HEADER_BYTES];
  uint32_t crc;
  bys_packer_t pk;
  uint8_t buf[BYS_BLOCK_BYTES]; /* current block (or the whole body for reference, ack and risk) */
} bys_frame_t;

/* Payload frame: 48-byte blocks with RS(+12) and CRC-32. */
int bys_frame_init_payload(bys_frame_t *f, bys_band_t band, bys_profile_t profile, const char *utf8, size_t n);
/* Reference: fp[12] (the first 12 bytes of the payload's SHA-256) and parity set `set` (0 = A, 1 = B). */
/* Largest packed payload (bytes after text packing, before the CRC-32) that fits in BYS_MAX_FRAME_SECONDS, or an error (BYS_E_ARG, BYS_E_PROFILE). */
int bys_max_payload_bytes(bys_band_t band, bys_profile_t profile);
int bys_frame_init_reference(bys_frame_t *f, bys_band_t band, bys_profile_t profile, unsigned set, const uint8_t fp[BYS_REF_BYTES]);
/* Ack (receipt: 0 ack, 1 receipt) with the first 4 bytes of the payload's SHA-256. Always the normal profile. */
int bys_frame_init_ack(bys_frame_t *f, bys_band_t band, int receipt, const uint8_t hash4[4]);
/* Risk: reason, source (0 sender, 1 receiver), hops, short 3-byte fingerprints. Always the normal profile. */
int bys_frame_init_risk(bys_frame_t *f, bys_band_t band, unsigned reason, unsigned source, unsigned hops, const uint8_t own[3], const uint8_t intruder[3]);
size_t bys_frame_symbols(const bys_frame_t *f);  /* total symbols, preamble included */
/* Writes the `groups` nibbles of the next symbol. 1 if it did, 0 if the frame is over. */
int bys_frame_next(bys_frame_t *f, uint8_t *nib);

/* ---------- Decoding: symbols to bytes ---------- */
/*
 * Fed with the symbols AFTER the preamble (the demodulator detects it separately; bys_preamble_symbol gives its pattern).
 * Each symbol is `groups` nibbles. A weak symbol can simply be passed through: RS correction treats it as an error.
 */
typedef struct {
  uint8_t kind, band, set, reason, source, hops, receipt, corrected;
  uint8_t fingerprint[BYS_REF_BYTES];
  uint8_t hash4[4], own[3], intruder[3];
  uint16_t payload_len; /* valid packed bytes in the output buffer */
} bys_result_t;

typedef struct {
  uint8_t band, profile, groups, state, hdr_n, buf_n, blk, total, kind, set, length, half, cur;
  uint16_t body_n;
  uint32_t out_n, out_cap;
  uint8_t *out;
  uint8_t buf[BYS_BLOCK_BYTES];
  uint8_t hdr[BYS_HEADER_BYTES];
  bys_result_t res;
} bys_rx_t;

#define BYS_RX_MORE 0 /* more symbols needed */
#define BYS_RX_DONE 1 /* frame complete and verified: see bys_rx_result */

/* `out`/`out_cap`: caller's buffer for the packed bytes of a payload frame (up to 255 * 48 + 4). */
void bys_rx_init(bys_rx_t *rx, bys_band_t band, bys_profile_t profile, uint8_t *out, size_t out_cap);
/* BYS_RX_MORE, BYS_RX_DONE or an error (BYS_E_HEADER, BYS_E_RS, BYS_E_CRC, BYS_E_FORMAT, BYS_E_SPACE). After an error it must be re-initialized. */
int bys_rx_push(bys_rx_t *rx, const uint8_t *nib);
/* End of the symbols (the gap between frames). A payload frame does not know the length of its last block and closes it here. */
int bys_rx_finish(bys_rx_t *rx);
/* 1 if the last block, closed at `len` of the bytes received so far, passes RS (with the expected index and total) and gives a
 * payload whose CRC-32 checks out: bys_rx_finish would then close the frame. For a demodulator that tries each length as it arrives. */
int bys_rx_last_fits(const bys_rx_t *rx, size_t len);
const bys_result_t *bys_rx_result(const bys_rx_t *rx);
/* Type and length of the header once read (0 if not yet). */
int bys_rx_header_ready(const bys_rx_t *rx);

/* Standalone header: validates CRC-8, version and band. */
int bys_header_parse(const uint8_t hdr[BYS_HEADER_BYTES], bys_band_t band, bys_profile_t profile, uint8_t *magic, uint8_t *length, uint8_t *set);
/* One block of a payload frame (length 15..62), corrected in place. Returns the index and fills total and the data length. */
int bys_block_decode(uint8_t *block, size_t len, const uint8_t *erasures, size_t nerr, uint8_t *index, uint8_t *total, size_t *chunk_len);
/* Mother code RS(46,14): joins the parity of the A and/or B frames (NULL if missing). fp_out[12] if the CRC-16 checks out. */
int bys_mother_decode(const uint8_t data14[BYS_REF_DATA], const uint8_t *parity_a, const uint8_t *parity_b, uint8_t *fp_out, uint8_t *corrected);

/* ---------- Transmitter: streaming synthesis to int16 PCM ---------- */
typedef struct {
  uint32_t sample_rate; /* 44100 or 48000 (16000 builds and works for low bands such as audible) */
  float level;          /* 0..1, default 1.0: transmission is always at maximum */
  uint16_t gap_ms;      /* silence at the end of the frame, default 350 */
} bys_tx_cfg_t;
void bys_tx_cfg_default(bys_tx_cfg_t *cfg);

typedef struct {
  bys_frame_t frame;
  uint32_t den, e_step;          /* 1000*rate (each tone's phase step is computed exactly per symbol; 2^32 = one cycle) and ramp step */
  uint32_t acc, sym_a;           /* symbol clock: remainder of the count and duration*rate in us*samples */
  uint32_t ramp, gap_n, total;   /* samples */
  uint32_t n, len;               /* position and length of the current symbol (or of the gap) */
  uint32_t phase[BYS_MAX_GROUPS], step[BYS_MAX_GROUPS];
  uint16_t amp, bin;             /* Q15 amplitude with level and peak cap already applied */
  uint8_t state;                 /* 0 symbols, 1 gap, 2 done */
  uint8_t nib[BYS_MAX_GROUPS];
} bys_tx_t;

/* cfg == NULL: defaults. All four return BYS_OK or an error. */
int bys_tx_init_payload(bys_tx_t *tx, const bys_tx_cfg_t *cfg, bys_band_t band, bys_profile_t profile, const char *utf8, size_t n);
int bys_tx_init_reference(bys_tx_t *tx, const bys_tx_cfg_t *cfg, bys_band_t band, bys_profile_t profile, unsigned set, const uint8_t fp[BYS_REF_BYTES]);
int bys_tx_init_ack(bys_tx_t *tx, const bys_tx_cfg_t *cfg, bys_band_t band, int receipt, const uint8_t hash4[4]);
int bys_tx_init_risk(bys_tx_t *tx, const bys_tx_cfg_t *cfg, bys_band_t band, unsigned reason, unsigned source, unsigned hops, const uint8_t own[3], const uint8_t intruder[3]);
/* Writes up to n samples; returns how many (fewer than n only at the end, gap included). */
size_t bys_tx_fill(bys_tx_t *tx, int16_t *out, size_t n);
int bys_tx_done(const bys_tx_t *tx);
uint32_t bys_tx_total_samples(const bys_tx_t *tx); /* frame + gap */

/* ---------- Streaming demodulator (preamble, timing and symbols from audio) ---------- */
/*
 * bys_demod_push() takes int16 PCM and calls the callback for each frame (payload, reference, ack or risk) it completes. No malloc:
 * the caller provides all state (bys_demod_t, of the size sizeof gives). A demodulator serves ONE profile (normal or robust); to
 * hear both, use two on the same audio (like the two receivers in the TS). It listens to all bands in `band_mask` at once.
 *
 * Per-tone energy engine, chosen at compile time (both make the same decisions; see the docs in demod.c):
 *   BYS_DEMOD_FFT      radix-2 FFT in float32 (never double). For the ESP32-S3 (single-precision FPU) and the host.
 *   BYS_DEMOD_GOERTZEL fixed-point Goertzel (integers only per sample and per window). For C3/C6/S2, no FPU.
 * If neither is defined: FFT on the host, ESP32 and ESP32-S3; Goertzel on the other ESP-IDF chips.
 * At 44.1 kHz (or any rate where the 46.875 Hz grid does not fall on the bins of a power-of-2 FFT) the FFT engine uses
 * float Goertzel, like the TS.
 *
 * These macros change the struct size: they must have the same value in the lib and in its user.
 */
#ifdef ESP_PLATFORM
#  include "sdkconfig.h" /* CONFIG_IDF_TARGET_* and the Kconfig options of this component */
#  if defined(CONFIG_BYSOUND_DEMOD_ENGINE_FFT) && !defined(BYS_DEMOD_FFT) && !defined(BYS_DEMOD_GOERTZEL)
#    define BYS_DEMOD_FFT 1
#  elif defined(CONFIG_BYSOUND_DEMOD_ENGINE_GOERTZEL) && !defined(BYS_DEMOD_FFT) && !defined(BYS_DEMOD_GOERTZEL)
#    define BYS_DEMOD_GOERTZEL 1
#  endif
#  if defined(CONFIG_BYSOUND_DEMOD_ROBUST) && !defined(BYS_DEMOD_ROBUST)
#    define BYS_DEMOD_ROBUST 1
#  endif
#  if defined(CONFIG_BYSOUND_DEMOD_LOW_BANK) && !defined(BYS_DEMOD_LOW_BANK)
#    define BYS_DEMOD_LOW_BANK 1
#  endif
#  if defined(CONFIG_BYSOUND_DEMOD_MAX_RATE) && !defined(BYS_DEMOD_MAX_RATE)
#    define BYS_DEMOD_MAX_RATE CONFIG_BYSOUND_DEMOD_MAX_RATE
#  endif
#endif
#if !defined(BYS_DEMOD_FFT) && !defined(BYS_DEMOD_GOERTZEL)
#  if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32P4)
#    define BYS_DEMOD_GOERTZEL 1
#  else
#    define BYS_DEMOD_FFT 1
#  endif
#endif
#if defined(BYS_DEMOD_FFT) && defined(BYS_DEMOD_GOERTZEL)
#  error "BYS_DEMOD_FFT and BYS_DEMOD_GOERTZEL are mutually exclusive"
#endif
#ifndef BYS_DEMOD_MAX_RATE
#define BYS_DEMOD_MAX_RATE 48000 /* highest sample rate that will be used: sizes the buffers */
#endif
#ifndef BYS_DEMOD_ROBUST
#define BYS_DEMOD_ROBUST 0       /* 1: the state also supports the robust profile (double window: about 20 KB more) */
#endif
#ifndef BYS_DEMOD_LOW_BANK
#define BYS_DEMOD_LOW_BANK 0     /* 1: supports the audible band (another 64 bins of history) */
#endif
#define BYS_DEMOD_SCALE      (BYS_DEMOD_ROBUST ? 2u : 1u)
#define BYS_DEMOD_WIN_MAX    ((BYS_DEMOD_MAX_RATE * 1000u * BYS_DEMOD_SCALE + 46874u) / 46875u) /* window = 1/46.875 s */
#define BYS_DEMOD_RING       (4u * BYS_DEMOD_WIN_MAX) /* recent audio: 4 windows (see demod.c) */
#define BYS_DEMOD_HIST       80u  /* search steps with stored energies (the preamble spans 63) */
#define BYS_DEMOD_HIGH_TONES 96u  /* bins 320..415: top, high, upper, mid, xhigh and wide */
#define BYS_DEMOD_BANK_TONES (BYS_DEMOD_HIGH_TONES + (BYS_DEMOD_LOW_BANK ? 64u : 0u))
#define BYS_DEMOD_CAND       16u  /* preamble candidates per plateau */
#define BYS_DEMOD_TAPER_MAX  (BYS_DEMOD_WIN_MAX / 20u + 3u)

typedef enum { BYS_EV_FRAME = 1, BYS_EV_HEADER = 2, BYS_EV_CLASH = 3 } bys_demod_event_type_t;
typedef struct {
  int type;                    /* BYS_EV_FRAME: complete frame; BYS_EV_HEADER: header read (band); BYS_EV_CLASH: two senders at once */
  uint8_t band, profile;
  const bys_result_t *result;  /* FRAME: the result (type, fingerprint, ack, risk) */
  const uint8_t *payload;      /* payload FRAME: packed bytes (bys_unpack turns them into text); valid only during the callback */
  size_t payload_len;
  uint16_t score_permille;     /* FRAME: preamble match, 0..1000 */
} bys_demod_event_t;
typedef void (*bys_demod_cb_t)(void *user, const bys_demod_event_t *ev);

typedef struct {
  uint32_t sample_rate;   /* 8000..BYS_DEMOD_MAX_RATE */
  bys_profile_t profile;
  uint8_t band_mask;      /* bit (1 << bys_band_t) of each band to listen to; default top, high, upper and mid */
  uint8_t hop_mult;       /* search step = hop_mult * 4 ms (default 1; 2 halves the Goertzel cost) */
  uint8_t *out;           /* caller's buffer for the packed bytes of a payload frame (255 * 48 + 4 bytes at most) */
  size_t out_cap;
  bys_demod_cb_t cb;
  void *user;
} bys_demod_cfg_t;

typedef struct { uint32_t frames, clashes, failures, headers; uint64_t samples; } bys_demod_stats_t;
/* Level of a band at the last step: strongest tone against the band's noise floor, and the highest SNR since bys_demod_levels_reset. */
typedef struct { float peak_db, floor_db, snr_db, hold_snr_db; } bys_demod_level_t;

typedef struct {
  bys_rx_t rx;
  int64_t frame_start, search_from;
  float frame_score, echo;
  int32_t cand_idx[BYS_DEMOD_CAND];
  float cand_score[BYS_DEMOD_CAND];
  uint32_t sym_us, win_start, end_off, span; /* band timings in samples (sym_us in microseconds) */
  uint16_t pool_off, bank_off, sym_next, body_read, probe_tried;
  uint8_t band, state, groups, tones, offset, enabled, magic, length, set, ncand, hdr_syms, probe_blk;
  uint8_t pre[BYS_PREAMBLE_SYMBOLS][BYS_MAX_GROUPS];
  uint8_t hdr_hard[2 * BYS_HEADER_BYTES];
  uint8_t hdr_prob[2 * BYS_HEADER_BYTES][16];
  float hold;
} bys_demod_reader_t;

typedef struct {
  uint32_t rate, win, hop, stride; /* stride: FFT bin per grid bin (0: no FFT, Goertzel) */
  uint8_t profile, hop_mult, nreaders, fft_ok, out_owner;
  uint8_t *out;
  size_t out_cap;
  bys_demod_cb_t cb;
  void *user;
  int64_t total, next_hop_end, hops;
  uint32_t wp, taper_n;
  uint32_t self_echo, margin, hdr_near, hdr_wait, overlap_from;
  float hop_s;
  bys_demod_stats_t stats;
  struct { int64_t at; uint32_t span; uint8_t band; } starts[8];
  struct { int64_t at, noted; } hfails[4];
  uint8_t nstarts, nhfails;
  uint8_t bank_used[2];
  uint16_t bank_lo[2], bank_hi[2];                     /* bins needed from each bank (only those are computed) */
  int16_t ring[BYS_DEMOD_RING];
  uint16_t hist[BYS_DEMOD_HIST][BYS_DEMOD_BANK_TONES]; /* per-tone energies in bfloat16 */
  float pool[2 * 400];                                  /* per band: noise floor and echo tail */
  float scratch[5 * BYS_DEMOD_HIGH_TONES];
  bys_demod_reader_t rd[BYS_BAND_COUNT];
#if defined(BYS_DEMOD_FFT)
  float taper[BYS_DEMOD_TAPER_MAX];
  float re[BYS_DEMOD_WIN_MAX], im[BYS_DEMOD_WIN_MAX];
  float twc[BYS_DEMOD_WIN_MAX / 2], tws[BYS_DEMOD_WIN_MAX / 2];
  float gz[BYS_DEMOD_BANK_TONES];                       /* Goertzel coefficients (only if the grid does not fall on bins) */
#else
  int32_t taper[BYS_DEMOD_TAPER_MAX];                   /* Q15 */
  int16_t win_buf[BYS_DEMOD_WIN_MAX];
  int32_t gz[BYS_DEMOD_BANK_TONES];                     /* 2 cos(w) in Q28 */
#endif
} bys_demod_t;

void bys_demod_cfg_default(bys_demod_cfg_t *cfg);
/* BYS_OK, BYS_E_ARG (rate, mask, profile, or audible band without BYS_DEMOD_LOW_BANK), BYS_E_PROFILE (robust without BYS_DEMOD_ROBUST) or BYS_E_RATE. */
int bys_demod_init(bys_demod_t *d, const bys_demod_cfg_t *cfg);
void bys_demod_reset(bys_demod_t *d); /* starts over (discards audio and partial frames); leaves the counters alone */
/* Processes n samples. Callbacks fire from here, on the calling thread. */
void bys_demod_push(bys_demod_t *d, const int16_t *pcm, size_t n);
const bys_demod_stats_t *bys_demod_stats(const bys_demod_t *d);
/* Progress of the frame in flight, 0..1000, or -1 if none. */
int bys_demod_progress(const bys_demod_t *d);
/* Band level (the band must be in the mask). 1 if there is data, 0 if no step has run yet. */
int bys_demod_level(const bys_demod_t *d, bys_band_t band, bys_demod_level_t *out);
void bys_demod_levels_reset(bys_demod_t *d);

#ifdef __cplusplus
}
#endif
#endif
