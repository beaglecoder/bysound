# bysound (C)

bysound v4 in C99: a data-over-sound modem (text to near-inaudible multi-tone FSK, Reed-Solomon + CRC). No `malloc`, no platform
dependencies; every piece of state is a struct you place yourself. It is an ESP-IDF component and a plain static library on the host.

The public API is in [`include/bysound.h`](include/bysound.h) (modem) and [`include/bysound_session.h`](include/bysound_session.h) (session layer, see below).

## Build and test

```sh
make test       # unit tests, golden audio, demodulator tests (FFT and Goertzel engines), session tests, under ASan/UBSan
make cli        # command-line tool -> build/bysound
make cli-test   # encode -> decode round trip for every band, rates, robust profile, and a real recording
make sizes      # sizeof(bys_demod_t) per engine, rate and option set
make size       # code size per object
```

Everything compiles with `-std=c99 -Wall -Wextra -Werror -pedantic`.

## Command line

```sh
build/bysound bands                                   # band table
build/bysound encode "hello" -o hello.wav             # top band, normal profile, 48 kHz
build/bysound encode "hello" -o h.wav --band mid --profile robust --rate 44100 --repeat 2
build/bysound decode hello.wav
# [  2.27 s] band=top profile=normal corrected=0 text=hello
build/bysound ack "hello" -o ack.wav --heard top     # the ack a receiver would play for "hello" (mid band); --receipt for a receipt (high)
build/bysound decode ack.wav
# [  0.87 s] band=mid profile=normal corrected=0 ack id=2cf24dba heard=top/normal
```

`decode` reads 16-bit PCM WAV at 16000, 44100 or 48000 Hz (mono, or the first channel of a multichannel file), listens on every
band that fits under Nyquist with both profiles, and feeds the streaming demodulator in 1024-sample chunks.

| band | range (Hz) | tones | symbol | raw rate | robust | max text in 30 s (normal / robust) |
|---|---|---|---|---|---|---|
| top | 18000-19453 | 32 | 32 ms | 250 bit/s | yes | 700 / 335 |
| high | 17250-19453 | 48 | 32 ms | 375 bit/s | yes | 1052 / 520 |
| upper | 16500-18703 | 48 | 32 ms | 375 bit/s | yes | 1052 / 520 |
| mid | 15000-17203 | 48 | 32 ms | 375 bit/s | yes | 1052 / 520 |
| audible | 1875-4828 | 64 | 40 ms | 400 bit/s | no | 1122 / - |
| xhigh | 16500-19453 | 64 | 32 ms | 500 bit/s | no | 1408 / - |
| wide | 15000-19453 | 96 | 32 ms | 750 bit/s | no | 2114 / - |

Raw rate is before preamble, header, Reed-Solomon and CRC overhead. Robust doubles the symbol time.

**30 s cap.** One transmission (preamble + header + body + the 350 ms gap) never lasts more than `BYS_MAX_FRAME_SECONDS` (30 s, `maxFrameSeconds` in `protocol-v4.json`).
`bys_frame_init_payload` and `bys_tx_init_payload` return `BYS_E_TOO_LONG` for a longer payload, and `bys_max_payload_bytes(band, profile)` gives the limit.
The last column is in packed bytes: ASCII letters pack 1:1, so it is also the letter count; digit pairs pack 2:1, so a numeric text can be up to twice as long.
`bysound bands` prints this column, and `bysound encode` names the limit for the band when the text is too long. The receiver is unchanged.

## Library API

```c
#include "bysound.h"

/* Encode: text -> int16 PCM, streamed. The text must stay alive while you fill. */
bys_tx_t tx;
bys_tx_cfg_t cfg; bys_tx_cfg_default(&cfg); cfg.sample_rate = 48000;
bys_tx_init_payload(&tx, &cfg, BYS_TOP, BYS_NORMAL, "hello", 5);
int16_t pcm[480];
while (!bys_tx_done(&tx)) { size_t n = bys_tx_fill(&tx, pcm, 480); /* play or store pcm[0..n) */ }

/* Decode: push audio of any chunk size; the callback fires once per verified frame. */
static void on_event(void *user, const bys_demod_event_t *ev) {
  (void)user;
  char text[256];
  if (ev->type == BYS_EV_FRAME && ev->payload && bys_unpack(ev->payload, ev->payload_len, text, sizeof text) >= 0) puts(text);
}
static bys_demod_t demod; static uint8_t packed[255 * 48 + 8];   /* state is static: no malloc */
bys_demod_cfg_t dc; bys_demod_cfg_default(&dc);
dc.sample_rate = 48000; dc.band_mask = 1u << BYS_TOP; dc.out = packed; dc.out_cap = sizeof packed; dc.cb = on_event;
bys_demod_init(&demod, &dc);
bys_demod_push(&demod, pcm, n);   /* call as audio arrives */
```

One demodulator listens to one profile (normal or robust); use two to hear both. Compile-time options
(`BYS_DEMOD_FFT` / `BYS_DEMOD_GOERTZEL`, `BYS_DEMOD_ROBUST`, `BYS_DEMOD_LOW_BANK`, `BYS_DEMOD_MAX_RATE`) change the size of
`bys_demod_t` and must match between the library and your code; under ESP-IDF they come from `menuconfig`.

## Session layer

[`bysound_session.h`](include/bysound_session.h) / [`src/session.c`](src/session.c) port [`docs/SESSION.md`](../docs/SESSION.md): sender and receiver state machines with
pre-listen, channel ladder, ambient check, robust switch, calibration memory, acks and receipts (with the heard-band nibble), the confirm gate,
second-sender detection, risk frames with relay (hops at most 2), the three security levels (`maximum` default) and quarantine as a callback only.
No malloc: `bys_sender_t` (about 1.3 KB) and `bys_receiver_t` (about 0.6 KB) are yours. The session never touches audio or a clock: you pass the time in
and it asks you to play one frame at a time.

```c
static int play(void *u, const bys_session_frame_t *f) {     /* start playing ONE frame; bys_*_play_done() when its last sample is out */
  bys_tx_cfg_t c; bys_tx_cfg_default(&c); c.sample_rate = 48000;
  return bys_session_tx_init(&my_tx, &c, f);                  /* then stream bys_tx_fill(&my_tx, ...) to the speaker */
}
bys_session_io io = { .user = ..., .play = play, .stop = stop, .cal_load = ..., .cal_save = ..., .on_delivered = ..., .on_failed = ... };

bys_sender_cfg_t sc; bys_sender_cfg_default(&sc, text, strlen(text));      /* maximum security, ladder, ambient check on */
bys_sender_start(&snd, &sc, &io, now_ms);
/* every audio block: */ bys_sender_tick(&snd, now_ms);  /* + bys_sender_play_done() at the end of each frame */
/* in the demodulator callback (one demodulator per profile): */ bys_sender_hear(&snd, now_ms, ev);
/* during the pre-listen, every BYS_NOISE_REPORT_MS: */ bys_session_noise_from_demod(&demod, db); bys_sender_noise(&snd, now_ms, db);
```

The receiver is the same shape (`bys_receiver_cfg_default(&rc, text_buf, sizeof text_buf)`, `bys_receiver_start/hear/tick/play_done`),
plus `bys_receiver_confirmed()` (the confirm gate: call it before acting on the data) and `bys_receiver_cancel()`. Security levels:
`bys_security_resolve(&cfg.security, BYS_SEC_STANDARD)`, then override single fields. Calibration memory is two callbacks (`cal_load`, `cal_save`).

What the C port does differently or leaves out (the reference is `web/src/session/`; also in [`docs/SESSION.md`](../docs/SESSION.md) §11.1):

- **Events:** the demodulator already decodes acks, receipts and risk frames (`BYS_EV_FRAME` with `result->kind`). It raises no `BYS_EV_HEADER`
  and does not count them for clashes; `bys_result_t.heard` carries the heard nibble.
- **Ambient check:** `bys_session_noise_from_demod()` converts the demodulator's running per-tone noise floor to the scale of SESSION.md 9.2. It is not the
  mean of the last 0.25 s of tone energies, so it differs from the reference by a few dB (white noise of sigma 0.003 and 0.01 measure within 1 dB of the
  documented -74 and -64 dB in the tests). The thresholds are as uncalibrated as in the reference. The demodulator must listen to the bands of the ladder.
  Without calling `bys_sender_noise` there are no reports and no ambient decision (plain ladder).
- **Output level:** always maximum, as in the protocol; there is no `level` option and no loud-place level change.
- **Reference mode:** the sender is complete; the receiver's resolver is synchronous (`bys_resolve_fn`), so there is no resolver timeout.
- **Other texts:** the short id of another text (risk frames) needs a `scratch` buffer to unpack into; without one it is `000000`.
- **Own speaker:** one frame plays at a time (a risk loop and an ack loop never mix as they can in the web). A half-duplex device that does not feed
  the microphone while it plays is what the windows (2 s) and receipt listens (3 s) are for.
- **Errors:** two codes of its own, `BYS_SERR_PLAY` (`io.play` failed) and `BYS_SERR_NO_SPACE` (the receiver's text buffer is too small).

Tests (`test/test_session.c`, in `make test`): the `session-acks.json`, `v4-acks.json`, `v4-risk.json` and `security-levels.json` vectors (also rendered with
`bys_tx` and decoded by `bys_demod`), the state machines on synthetic events, and simulated air (audio from `bys_tx` mixed per device pair and fed to real
demodulators): ack stops the sender and the receipt stops the receiver, a speaker deaf above 17 kHz (low-pass) makes the ladder fall to mid and the
calibration is learned and used, a second sender makes the receiver fail and warn while the real sender stops and relays, a clash, the relay chain stopping at 2 hops.

## Footprint

`sizeof(bys_demod_t)` on a 64-bit host (32-bit targets are slightly smaller), from `make sizes`:

| engine | normal only, 48 kHz | + robust | + audible band |
|---|---|---|---|
| FFT float32 | 45.5 KB | 66.2 KB | 56.0 KB |
| Goertzel integer | 35.3 KB | 45.8 KB | 45.8 KB |

The transmitter state `bys_tx_t` is 216 bytes. The session layer adds about 11 KB of code on ESP32-S3 (`session.c` 10.3 KB, `sha256.c` 1 KB). Code on ESP32-S3 (`-O2`, all objects): about 21 KB. Host speed (`make bench`, Apple
silicon, one core): the FFT engine decodes the top band alone at roughly 740x real time, Goertzel at about 70x.

## Examples

- [`examples/bysound_cli.c`](examples/bysound_cli.c): the command-line tool above.
- [`../esp32`](../esp32): ESP32-S3 demo with an I2S microphone and amplifier.
