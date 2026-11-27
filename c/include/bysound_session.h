/*
 * bysound session layer in C99 (no malloc): sender and receiver state machines of docs/SESSION.md, driven by the caller.
 *
 *   - Time is passed in (`now_ms`, any monotonic uint32 clock that wraps; differences are taken modulo 2^32).
 *   - Output goes through `bys_session_io.play`: the session asks to play ONE frame (a bys_session_frame_t) and the caller reports
 *     `bys_*_play_done()` when its last sample (gap included) was played. Control loops (acks, receipts, risk) are repeated by the
 *     session, one frame per play call. `bys_session_tx_init()` turns a descriptor into a bys_tx_t.
 *   - Input is what the demodulator reports: `bys_*_hear()` takes the bys_demod_event_t of any demodulator (one per profile).
 *     Frames, acks, receipts and risk frames, header starts and clashes all come from it. Acks and risk frames raise no header
 *     event and are not counted as clashes (the demodulator does that, SESSION.md section 4).
 *   - `bys_*_tick()` must be called often (every audio block is fine): it runs the timers.
 *   - Everything is single-threaded: call all functions of one session from the same task.
 *
 * Differences from the TypeScript reference are listed in c/README.md ("Session layer").
 */
#ifndef BYS_SESSION_H
#define BYS_SESSION_H

#include "bysound.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Constants (docs/SESSION.md section 10), in milliseconds ---------- */
#define BYS_LISTEN_WINDOW_MS      2000
#define BYS_CLONE_GUARD_MS        500
#define BYS_FINAL_LISTEN_MS       1500
#define BYS_FINAL_EXTEND_MS       2500
#define BYS_FINAL_LISTEN_MAX_MS   6000
#define BYS_ACK_MARGIN_MS         500
#define BYS_ACK_RUN_MIN_MS        3000
#define BYS_ACK_RUN_MAX_MS        20000
#define BYS_RECEIPT_LISTEN_MS     3000
#define BYS_ACK_TOTAL_MS          30000
#define BYS_CONTROL_GAP_MS        150
#define BYS_HOP_REPETITIONS       3
#define BYS_CALIBRATED_HOP_FACTOR 2
#define BYS_NOISE_REPORT_MS       250
#define BYS_NOISY_CHANNEL_DB      10.0f
#define BYS_LOUD_PLACE_DB         (-63.0f)
#define BYS_ROBUST_NOISE_DB       (-68.0f)
#define BYS_ROBUST_AFTER_REPETITIONS 6
#define BYS_CALIBRATION_USES      10
#define BYS_SESSION_MAX_CHANNELS  BYS_BAND_COUNT
#define BYS_SESSION_MAX_REPORTS   24 /* noise reports kept per channel during the pre-listen (6 s at 4 per second) */
#define BYS_RISK_MEMORY_SIZE      8
#define BYS_SESSION_SEEN          8  /* incidents reported at most once per session */

/* ---------- Identifiers (SHA-256 of the UTF-8 text) ---------- */
void bys_sha256(const uint8_t *data, size_t n, uint8_t out[32]);
typedef struct { uint8_t fingerprint[BYS_REF_BYTES], ack[4], short_id[3]; } bys_ids_t; /* first 12, 4 and 3 bytes of the hash */
void bys_text_ids(const char *text, size_t n, bys_ids_t *out);

/* Heard nibble of acks (SESSION.md 3.1): 0 = not stated, else robust << 3 | (band id + 1). */
unsigned bys_heard_nibble(bys_band_t band, bys_profile_t profile);
/* 1 and band/profile filled; 0 if not stated (nibble 0); -1 if invalid. */
int bys_heard_parse(unsigned nibble, bys_band_t *band, bys_profile_t *profile);

/* Bands a risk frame is looped on (SESSION.md 3.2). `avoid`: band of what is reported, or -1. Returns how many (1 or 2). */
unsigned bys_risk_bands(bys_band_t work, int avoid, bys_band_t out[2]);
/* The two bands share at least one tone. */
int bys_bands_overlap(bys_band_t a, bys_band_t b);

/* ---------- Security levels (docs/SESSION.md section 7, test-vectors/security-levels.json) ---------- */
typedef enum { BYS_SEC_BASIC = 0, BYS_SEC_STANDARD = 1, BYS_SEC_MAXIMUM = 2 } bys_security_level_t;
typedef struct {
  uint8_t emit_auto;          /* riskSignal.emit == 'auto' */
  uint8_t listen, honor;
  uint8_t listen_until_confirm; /* riskSignal.listenUntil == 'confirm' (else 'delivery') */
  uint32_t duration_ms;       /* receiver's risk loop */
  uint32_t detector_duration_ms; /* sender's risk loop and relays */
  uint32_t quarantine_ms;     /* suggested pause, delivered to on_quarantine; never enforced; also the RiskMemory window */
} bys_risk_signal_t;
typedef struct {
  uint8_t detect_multiple_emitters, listen_while_sending, confirm_watch;
  uint32_t listen_before_ms, scan_jitter_ms, confirm_watch_ms;
  bys_risk_signal_t risk;
} bys_security_t;
/* Fills `out` with the level (default maximum). Override single fields afterwards: explicit values win. */
void bys_security_resolve(bys_security_t *out, bys_security_level_t level);

/* ---------- Errors ---------- */
typedef enum {
  BYS_SERR_NONE = 0,
  BYS_SERR_OTHER_TRANSMISSION,    /* sender, pre-listen: another transmission is sounding */
  BYS_SERR_CLONED_TRANSMISSION,   /* sender: its own data is played by someone else */
  BYS_SERR_OTHER_EMITTER,         /* sender: another transmission heard while sending */
  BYS_SERR_RISK_SIGNAL,           /* a risk frame was heard and honored */
  BYS_SERR_MULTIPLE_TRANSMISSIONS,/* receiver: two texts or a clash */
  BYS_SERR_TIMEOUT,
  BYS_SERR_STOPPED,               /* sender: stop() before the ack */
  BYS_SERR_CANCELLED,             /* receiver: cancel() */
  BYS_SERR_NOT_FOUND,             /* receiver, reference mode: resolver returned nothing */
  BYS_SERR_MISMATCH,              /* receiver, reference mode: text does not match the fingerprint */
  BYS_SERR_NO_RESOLVER,
  BYS_SERR_PLAY,                  /* (C only) io.play failed: the frame could not be rendered */
  BYS_SERR_NO_SPACE,              /* (C only) the receiver's text buffer is too small */
  BYS_SERR_PENDING                /* confirmed() before delivery */
} bys_session_error_t;

/* ---------- Risk frames, alerts, calibration ---------- */
typedef enum { BYS_RISK_OTHER = 1, BYS_RISK_CLONE = 2, BYS_RISK_OVERLAP = 3, BYS_RISK_CANCELLED = 4 } bys_risk_reason_t;
typedef enum { BYS_ROLE_SENDER = 0, BYS_ROLE_RECEIVER = 1 } bys_role_t;

typedef struct {
  uint8_t heard;       /* 1: heard, 0: emitted */
  uint8_t reason, source, hops;
  uint8_t own[3], intruder[3];
  uint8_t bands[2], nbands; /* heard: the band it was heard on; emitted: the bands it is looped on */
  uint8_t matches_mine; /* heard: the frame's `own` is this side's data. Informative only. */
  uint8_t relay;        /* emitted as a relay of a heard alert */
} bys_alert_t;

/* Per device (shared by its sessions): incidents it emitted or relayed, so it ignores its own frames and relays once. */
typedef struct { struct { uint8_t key[8]; uint32_t at; uint8_t used; } e[BYS_RISK_MEMORY_SIZE]; uint8_t next; } bys_risk_memory_t;
void bys_risk_memory_init(bys_risk_memory_t *m);

typedef struct { uint8_t band, profile, uses; } bys_calibration_t;

/* ---------- What the session asks the caller to play ---------- */
typedef struct {
  uint8_t kind;               /* BYS_KIND_PAYLOAD, _REFERENCE, _ACK, _RISK */
  uint8_t band, profile;
  uint8_t receipt, heard;     /* ack: 0 ack / 1 receipt, heard nibble */
  uint8_t set;                /* reference: parity set 0 / 1 */
  uint8_t reason, source, hops; /* risk */
  uint8_t hash4[4], own[3], intruder[3], fp[BYS_REF_BYTES];
  const char *text; size_t text_len; /* payload: the sender's text (must stay alive while playing) */
  uint16_t gap_ms;            /* silence to append: 350 for data, 150 for control frames */
} bys_session_frame_t;
/* Initializes `tx` for the descriptor (sets the gap). cfg == NULL: defaults. Returns BYS_OK or a bysound error. */
int bys_session_tx_init(bys_tx_t *tx, const bys_tx_cfg_t *cfg, const bys_session_frame_t *f);

/* ---------- Caller hooks ---------- */
typedef struct {
  uint8_t band, profile;
  uint8_t stated;       /* the ack said where it was read (else band/profile are those of the last repetition) */
  uint32_t ms;          /* from the first repetition to the ack */
  uint32_t repetitions;
  uint8_t saved;        /* a calibration was saved */
  bys_calibration_t calibration;
} bys_delivered_t;
typedef struct {
  uint8_t present;      /* an ambient decision exists (there were noise reports) */
  float noise_db[BYS_BAND_COUNT]; /* median per channel of the chain; NAN if none */
  uint8_t noisy_mask;   /* bit (1 << band): skipped, NOISY_CHANNEL_DB above the quietest */
  uint8_t loud;
} bys_ambient_t;

typedef struct {
  void *user;
  /* Start playing one frame. Return < 0 if it cannot be rendered. The caller reports bys_*_play_done() at its end. */
  int  (*play)(void *user, const bys_session_frame_t *frame);
  /* Silence the speaker now (a frame in flight is dropped; do not call play_done for it). */
  void (*stop)(void *user);
  /* Optional. Uniform random 32 bits (pre-listen jitter). NULL: an internal xorshift. */
  uint32_t (*random)(void *user);
  /* Optional calibration memory. load returns 1 and fills `c` if one is stored. save(NULL) forgets it. */
  int  (*cal_load)(void *user, bys_calibration_t *c);
  void (*cal_save)(void *user, const bys_calibration_t *c);
  /* Optional notifications. */
  void (*on_alert)(void *user, const bys_alert_t *alert);
  void (*on_quarantine)(void *user, uint32_t duration_ms, uint8_t reason, uint8_t source); /* a suggestion only */
  void (*on_state)(void *user, int state);
  void (*on_repetition)(void *user, uint32_t index, bys_band_t band, bys_profile_t profile); /* sender */
  void (*on_ambient)(void *user, const bys_ambient_t *a, const uint8_t *route, unsigned n);    /* sender */
  void (*on_delivered)(void *user, const bys_delivered_t *d);                                  /* sender: acked */
  void (*on_received)(void *user, const char *text, size_t len, bys_band_t band, int via_reference); /* receiver */
  void (*on_receipt)(void *user, int heard);                                                   /* receiver: receipt heard, or acking ended */
  void (*on_failed)(void *user, bys_session_error_t err, const bys_alert_t *alert);             /* both; alert may be NULL */
  void (*on_finished)(void *user);                                                             /* nothing more to play or hear */
} bys_session_io;

/* ---------- Sender ---------- */
typedef enum { BYS_SS_PRE_LISTEN = 0, BYS_SS_SENDING, BYS_SS_WINDOW, BYS_SS_RECEIPT, BYS_SS_ALERTING, BYS_SS_DONE } bys_sender_state_t;

typedef struct {
  const char *text; size_t text_len;  /* must stay alive until the session is done */
  uint8_t reference;                  /* 1: send only the fingerprint (receiver resolves it); default 0 */
  int8_t band;                        /* fixed band (no ladder, no calibration), or -1 */
  uint8_t channels[BYS_SESSION_MAX_CHANNELS], nchannels; /* ladder from the least audible down; nchannels 0: top, high, upper, mid */
  int8_t profile;                     /* fixed BYS_NORMAL / BYS_ROBUST, or -1 (automatic) */
  uint32_t timeout_ms;                /* from the first frame; default 60000 */
  uint8_t ambient;                    /* measure noise in the pre-listen (needs bys_sender_noise); default 1 */
  bys_security_t security;            /* default: maximum */
  bys_risk_memory_t *memory;          /* shared per device; NULL: one private to this session */
  char *scratch; size_t scratch_cap;  /* optional: unpack buffer to read the short id of another text (else it is 000000) */
} bys_sender_cfg_t;
void bys_sender_cfg_default(bys_sender_cfg_t *cfg, const char *text, size_t text_len);

typedef struct { /* internal state of the shared part */
  const bys_session_io *io;
  bys_security_t sec;
  bys_risk_memory_t *mem, own_mem;
  uint32_t rng;
  uint8_t playing, nseen, seen[BYS_SESSION_SEEN][8];
  struct { uint8_t on, idx, n, bands[2]; uint32_t start, dur; bys_session_frame_t proto; } loop;
  char *scratch; size_t scratch_cap;
} bys_core_t;

typedef struct { uint8_t kind; uint16_t len; uint32_t crc; uint8_t fp[BYS_REF_BYTES]; } bys_content_t;

typedef struct {
  bys_core_t core;
  bys_sender_cfg_t cfg;
  bys_ids_t ids;
  bys_content_t own;
  uint8_t state, pending, busy, band, profile, have_last_end, last_band;
  uint32_t changes;
  /* plan */
  uint8_t route[BYS_SESSION_MAX_CHANNELS], route_len, cal_valid, start_robust; /* 0 none, 1 calibration, 2 noise */
  uint32_t every;
  bys_calibration_t cal;
  /* robust ladder */
  uint8_t ladder_auto, ladder_robust, ladder_reason; /* reason 0 none, 1 noise, 2 repetitions */
  uint32_t ladder_normal;
  /* ambient */
  bys_ambient_t ambient;
  float reports[BYS_SESSION_MAX_CHANNELS][BYS_SESSION_MAX_REPORTS];
  uint8_t nreports[BYS_SESSION_MAX_CHANNELS];
  uint8_t hop;
  uint32_t on_channel, sent, pre_end, first_at, last_end, window_end;
} bys_sender_t;

/* Starts the session (pre-listen or first repetition). Returns BYS_OK, BYS_E_ARG (missing or empty text, bad band, profile or
 * channel) or BYS_E_TOO_LONG (payload mode: the text does not fit one transmission on a band the session may play). */
int  bys_sender_start(bys_sender_t *s, const bys_sender_cfg_t *cfg, const bys_session_io *io, uint32_t now_ms);
void bys_sender_hear(bys_sender_t *s, uint32_t now_ms, const bys_demod_event_t *ev);
/* Noise report (every BYS_NOISE_REPORT_MS during the pre-listen): dB per band relative to a full-scale grid tone, NAN if unknown. */
void bys_sender_noise(bys_sender_t *s, uint32_t now_ms, const float db[BYS_BAND_COUNT]);
void bys_sender_tick(bys_sender_t *s, uint32_t now_ms);
void bys_sender_play_done(bys_sender_t *s, uint32_t now_ms);
/* Stops everything now (risk loop included). Before the ack, on_failed(STOPPED). */
void bys_sender_stop(bys_sender_t *s, uint32_t now_ms);
bys_sender_state_t bys_sender_state(const bys_sender_t *s);

/* ---------- Receiver ---------- */
typedef enum { BYS_RS_LISTENING = 0, BYS_RS_FINAL_LISTEN, BYS_RS_DELIVERED, BYS_RS_CLOSED, BYS_RS_FAILED } bys_receiver_state_t;
typedef enum { BYS_WATCH_CLEAN = 0, BYS_WATCH_EXPIRED, BYS_WATCH_OFF } bys_watch_t;
typedef struct { bys_watch_t watch; uint32_t watched_ms, uncovered_ms; } bys_confirmation_t;

/* Reference mode: returns the text for a fingerprint, or NULL. bysound checks that SHA-256(text) starts with it. Synchronous. */
typedef const char *(*bys_resolve_fn)(void *user, const uint8_t fingerprint[BYS_REF_BYTES]);

typedef struct {
  char *text; size_t text_cap;        /* the delivered text (NUL terminated) lives here until the session is over */
  char *scratch; size_t scratch_cap;  /* optional: unpack buffer to read the short id of another text (else 000000) */
  uint32_t timeout_ms;                /* listening for the first frame; default 60000 */
  uint8_t ack;                        /* acknowledge by sound after delivery; default 1 */
  bys_resolve_fn resolve; void *resolve_user;
  bys_security_t security;            /* default: maximum */
  bys_risk_memory_t *memory;
} bys_receiver_cfg_t;
void bys_receiver_cfg_default(bys_receiver_cfg_t *cfg, char *text, size_t text_cap);

typedef struct {
  bys_core_t core;
  bys_receiver_cfg_t cfg;
  bys_ids_t ids;
  bys_content_t first;
  uint8_t state, have_first, first_band, first_profile, first_short[3], has_text, first_reference;
  uint8_t closed;       /* 0 no, 1 confirmed, 2 cancelled */
  uint8_t watching, watch_expired, acking, alerting, ack_phase; /* ack_phase: 0 run (playing), 1 receipt listen */
  uint8_t has_conflict, has_confirmation, delivered, finished, receipt_settled, fail_err;
  bys_session_error_t conflict_err;
  bys_alert_t conflict_alert; /* the alert that closed the gate (valid when conflict_has_alert) */
  uint8_t conflict_has_alert;
  bys_confirmation_t confirmation;
  uint32_t start_at, first_at, final_end, text_len, watch_start, watch_end, watch_end_at, ack_until, ack_run_ms, ack_listen_until;
} bys_receiver_t;

int  bys_receiver_start(bys_receiver_t *r, const bys_receiver_cfg_t *cfg, const bys_session_io *io, uint32_t now_ms);
void bys_receiver_hear(bys_receiver_t *r, uint32_t now_ms, const bys_demod_event_t *ev);
void bys_receiver_tick(bys_receiver_t *r, uint32_t now_ms);
void bys_receiver_play_done(bys_receiver_t *r, uint32_t now_ms);
/* The confirm gate: call before acting on the data. Returns 0 and fills `out`, or the error (a bys_session_error_t):
 * MULTIPLE_TRANSMISSIONS / RISK_SIGNAL seen while watching, CANCELLED, the failure that stopped delivery, or PENDING. */
int  bys_receiver_confirmed(bys_receiver_t *r, uint32_t now_ms, bys_confirmation_t *out);
/* The person left: stops watching and acking. A risk frame already sounding finishes. */
void bys_receiver_cancel(bys_receiver_t *r, uint32_t now_ms);
bys_receiver_state_t bys_receiver_state(const bys_receiver_t *r);
/* Short id (3 bytes) of the received text, or NULL before the first frame. */
const uint8_t *bys_receiver_short_id(const bys_receiver_t *r);

/* ---------- Ambient helper ---------- */
/*
 * Converts the demodulator's band floors to the scale of SESSION.md 9.2: dB relative to a full-scale grid tone in the analysis
 * window. `out[b]` is NAN for bands not in the demodulator's mask or before the first step. Note: the demodulator reports its running
 * per-tone noise floor, not the mean of the last 0.25 s of tone energies, so the numbers track the reference within a few dB and,
 * like it, are not field-calibrated. Call it every BYS_NOISE_REPORT_MS during the pre-listen and pass the result to bys_sender_noise().
 */
void bys_session_noise_from_demod(const bys_demod_t *d, float out[BYS_BAND_COUNT]);

#ifdef __cplusplus
}
#endif
#endif
