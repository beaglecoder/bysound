# bysound: incremental changes to near-ultrasonic data-over-sound, measured against ggwave

**Agustin Gandara**<br>
beaglecoder<br>
November 2026

<p align="center"><img src="docs/assets/hero.svg" alt="bysound overview" width="640"></p>

## Abstract

Data-over-sound lets two devices exchange a short message using only a speaker and a microphone. **ggwave**, a widely used open-source implementation, sends bytes with multi-frequency shift keying (MFSK) on a 46.875 Hz grid and protects them with Reed–Solomon (RS) coding. **bysound** keeps that foundation and changes the framing and the receiver: narrower band "channels" (2–6 simultaneous tones) inside 15–19.5 kHz, a per-tone noise floor, a keyed preamble, maximum-likelihood header selection, subtraction of the previous symbol's echo, RS in blocks with an end-to-end CRC-32, and a slower *robust* profile. Byte-compatible implementations of the modem exist in TypeScript, Swift, Kotlin and allocation-free C. A session layer on top of the modem (acknowledgements by sound, detection of a second sender, alert frames, security levels and automatic channel adaptation) is specified and implemented in all four languages (TypeScript as the reference), sharing test vectors.

This report compares the C implementation of bysound with the unmodified ggwave library on the same simulated channel with the same seeds (157,670 trials). In summary, the two detectors perform within 0–3 dB of each other at equal SNR inside each mode's band; most of bysound's advantage against broadband noise comes from narrower channels and a lower peak-to-average power ratio; and ggwave uses less CPU and is the more mature project. In more detail:

- **Detection is similar.** With the signal-to-noise ratio (SNR) measured inside each mode's own band, the two libraries reach 90 % success within 0–3 dB of each other.
- **Narrow channels tolerate more full-band noise.** At equal average signal power, with noise measured over 0–24 kHz, bysound's normal channels reach 90 % success at −14 to −8 dB and its robust profile at −16 to −14 dB, against −8 dB for ggwave's ultrasound and audible protocols. This follows mostly from concentrating the same power in a narrower band, a comparison that favours narrowband modes by construction. At equal peak amplitude the difference is 1–12 dB (up to 15 dB for the robust profile), because fewer simultaneous tones give a lower peak-to-average power ratio. Normalised for energy per message, the advantage is about 3–5 dB.
- **Echo.** With one synthetic two-tap echo and one transmission, bysound tolerates 4–12 dB more noise, depending on the channel. The part of that difference that is specific to echo (4 dB) is consistent with echo subtraction.
- **Message size and integrity.** bysound limits a transmission to 30 s instead of 140 bytes. It delivered no wrong message in 87,380 simulated trials; a CRC-32 over the whole text guards delivery. ggwave delivered 34 wrong payloads in 67,320 trials of its default variable-length protocols, 32 of them for 8-byte messages.
- **Cost.** ggwave's receiver uses less CPU: 41–51× less while idle and 1.2–14× less in trials that decode a message.

The benchmark was written by the author of bysound and exercises only the C receiver. The session layer is described and implemented but not part of the benchmark. Nothing in this report has been measured over the air.

## 1. Introduction

Acoustic data links use hardware that is already present: phones, laptops and smart speakers have a speaker and a microphone, and a microcontroller board can be given both with an I2S microphone and an amplifier. Typical uses hand over a token that is longer than a person wants to type but shorter than a file:

- a pairing code;
- a URL;
- a payment reference;
- Wi-Fi credentials for an IoT device.

Three properties matter in practice:

1. **It must work in real rooms:** background noise, echo, inexpensive transducers, and clocks that differ between devices.
2. **It must be exact.** A wrong token is worse than no token, so the receiver should reject what it cannot verify.
3. **It should be unobtrusive.** The 15–19.5 kHz range is above what most adults hear well, and 18–19.5 kHz is inaudible to most adults. Many phone speakers and microphones also lose sensitivity above about 17 kHz, so the two goals pull in opposite directions.

ggwave [1] made the approach easy to adopt: a small C++ library, ready-made bindings for several languages, and protocols in audible, "ultrasound" (15–19.4 kHz), dual-tone and mono-tone variants. bysound was developed for applications that need the three properties above. This report documents what was changed, why, and how much of each change shows up in a simulated comparison with ggwave.

**Contributions.**
- A specification of **bysound v4**, a payload framing on the same 46.875 Hz grid ([PROTOCOL.md](docs/PROTOCOL.md)).
- A specification of a **session layer** for two-party exchanges by sound ([SESSION.md](docs/SESSION.md)), with a security model ([SECURITY.md](docs/SECURITY.md)).
- Four interoperable open-source implementations of the modem and the session layer that share test vectors, with examples.
- A reproducible benchmark against unmodified ggwave, with all raw data.
- An analysis that separates the sources of the difference (bandwidth, peak-to-average power, coding, detection) and lists where ggwave is better.

## 2. Related work

- **ggwave** [1] (G. Gerganov, MIT). MFSK on a 46.875 Hz grid, RS error correction, start and end sound markers, and payloads of up to 140 bytes. Its audible and ultrasound protocols play 6 tones at once; it also has dual-tone (DT) and mono-tone (MT) protocols. Each comes in normal, fast and fastest speeds. bysound uses the same grid spacing, which makes the comparison direct.
- **Quiet** [2] (libquiet). Software-defined-radio-style modulation over sound (its profiles include OFDM and GMSK), built on liquid-dsp. It aims at higher bit rates with a more complex receiver.
- **minimodem** [3]. A general-purpose FSK software modem. It targets standard wireline and radio FSK protocols (Bell 103/202, RTTY and others), not near-ultrasonic links.
- **Dhwani** [4] (SIGCOMM 2013). Acoustic NFC between phones, with a security model based on self-jamming. It targets confidentiality at very short range, which bysound does not attempt.

The comparison is with ggwave only, because it is the closest in design. Systems with different goals (throughput, secure short-range exchange) would need a different test to be compared fairly.

## 3. Design of bysound v4

<p align="center"><img src="docs/assets/bands.svg" alt="The seven bysound channels" width="100%"></p>

**Tone grid.** Every tone sits at `f = (bin + 16·g + nibble) × 46.875 Hz`. The analysis window is exactly 1/46.875 s (1,024 samples at 48 kHz), so tones are orthogonal and fall on FFT bins. At 44.1 kHz the receiver uses Goertzel filters on the same grid.

**Channels.** Seven channels, each `G` groups of 16 tones, playing one tone per group per symbol (one nibble per group):

| Channel | Span | G | Symbol | Raw rate |
|---|---|---|---|---|
| `top` | 18.0–19.5 kHz | 2 | 32 ms | 250 bit/s |
| `high` | 17.25–19.5 kHz | 3 | 32 ms | 375 bit/s |
| `upper` | 16.5–18.75 kHz | 3 | 32 ms | 375 bit/s |
| `mid` | 15.0–17.25 kHz | 3 | 32 ms | 375 bit/s |
| `xhigh` | 16.5–19.5 kHz | 4 | 32 ms | 500 bit/s |
| `wide` | 15.0–19.5 kHz | 6 | 32 ms | 750 bit/s |
| `audible` | 1.875–4.875 kHz | 4 | 40 ms | 400 bit/s |

`wide` occupies the same span as ggwave's ultrasound protocols (15–19.4 kHz), so it is the like-for-like comparison. ggwave has no protocol confined above 18 kHz, which is where `top` sits.

**Frame.** A frame has four parts:
1. An 8-symbol **preamble**, keyed per channel and profile.
2. A 4-byte **header**: type, block count, CRC-8 and version|channel.
3. The **body**: the packed text plus a CRC-32, cut into ≤ 48-byte blocks. Each block is `RS([index, total] + data, 12 parity)` over GF(256).
4. A 350 ms **gap**.

The length of the last block is not transmitted. The receiver closes it at the first length that Reed–Solomon accepts *and* whose assembled payload passes the CRC-32, because RS alone cannot decide the length (if the last parity byte is `0x00`, the codeword one byte shorter is also valid). The frame is sent several times in a row; each repetition is called a *lap*.

<p align="center"><img src="docs/assets/frame.svg" alt="Frame anatomy" width="100%"></p>
<p align="center"><img src="docs/assets/spectrogram.svg" alt="Spectrogram of a real frame" width="100%"></p>

## 4. The changes, one by one

The benchmark in Section 6 uses the **C receiver**. It is a bounded-memory port of the TypeScript reference receiver: hard decisions per symbol, echo subtraction, a per-tone noise floor, maximum-likelihood header selection and errors-only RS decoding. It does not compute soft symbols, does not mark erasures and decodes each lap on its own (see the header comment of [`c/src/demod.c`](c/src/demod.c)). The changes are therefore listed in two groups: those the benchmark measures, and those that are implemented (some only in the TypeScript, Swift and Kotlin receivers) but that the benchmark does not exercise. The item numbers are used in the rest of the report.

### 4.1 Measured in this benchmark

| # | Area | ggwave | bysound | Expected effect |
|---|---|---|---|---|
| 1 | **Bandwidth** | 6 tones in the audible/ultrasound protocols (≈ 4.4 kHz span) | 2–6 tones per channel (1.5–4.5 kHz) | With the same total power in fewer tones, each tone stands higher above white noise. This accounts for most of the full-band SNR difference (Section 6.2). |
| 2 | **Peak-to-average power** | 6 tones summed: PAPR 12.1–12.9 dB | 2–3 tones: PAPR 6.3–8.3 dB (4 tones 9.5–9.6 dB, 6 tones 11.4 dB) | A speaker is limited by its peak, not its average. At the same peak amplitude a lower PAPR puts 0.8–6.6 dB more average power in the air than ggwave's ultrasound protocols (Section 6.2). |
| 3 | **Noise floor per tone** | Fixed-ratio marker detection, no per-tone noise estimate | Running background level for every tone, divided out before detection | A steady narrowband interferer raises the floor of its own tone instead of being read as a symbol. |
| 4 | **Preamble** | Start and end marker tones | 8-symbol pattern keyed per channel and profile; threshold 0.32 on a normalised score (noise alone scores about 0.06). A preamble found inside a frame body is taken as the next lap only if it scores at least 0.7 of the current frame's own preamble score | It identifies the channel and the symbol timing in one step and keeps channels apart. The 0.7 rule separates a real next lap from body data that happens to match a few preamble nibbles. |
| 5 | **Header decision** | 1 length byte with its own RS code | 4-byte header with CRC-8; the receiver picks the most likely of the 259 valid headers | A header with a few wrong nibbles is still chosen correctly, because only valid headers are candidates. |
| 6 | **Echo subtraction** | Hard decision per symbol | Hard decision per symbol after subtracting the previous symbol's echo, whose strength is measured on the preamble | Reverberation leaves energy from the previous chord in the current one. Section 6.3 measures the effect under one synthetic echo. |
| 8 | **Blocks + CRC-32** | One RS codeword of up to 140 B; parity grows with length (4 bytes for 8 B, 12 for 32 B, 56 for 140 B); no checksum | Up to 255 blocks of up to 48 data bytes, each with 12 RS parity bytes, plus a CRC-32 over the whole text | Messages longer than 140 B with the same protection per block, and a final check that rejects a wrongly corrected block (Section 6.5). |
| 10 | **Robust profile** | Speed variants change the symbol length | `top`, `high`, `upper`, `mid` with symbol, window and hop doubled | 2 dB measured on the 2 dB grid (3 dB expected from doubling the symbol energy), at half the speed. |

### 4.2 Implemented, not measured here

| # | Area | ggwave | bysound | Where |
|---|---|---|---|---|
| 7 | **Erasure decoding** | RS errors only | Each block is tried with 0, 2 and 4 erasures on the least-confident bytes | TypeScript, Swift and Kotlin receivers. The C receiver corrects errors only. |
| 9 | **Combining laps** | Each transmission decoded alone | Up to 6 laps combined per block | TypeScript, Swift and Kotlin receivers. The C receiver decodes each lap on its own. |
| 11 | **Text packing** | Raw bytes | Digit pairs packed into 1 byte; ASCII as is; other text as UTF-8 | All four implementations. The benchmark payloads contain no digits, so packing gives no advantage in Section 6. |
| 12 | **Clash detection** | Not provided | Overlapping transmissions the receiver can hear are reported as a clash | All four receivers. No benchmark trial has two senders. |
| 13 | **Soft symbols** | Hard decision per symbol | A likelihood for each of the 16 tones, used by items 7 and 9 | TypeScript, Swift and Kotlin receivers. |

How much items 7, 9 and 13 add is not known from this benchmark. ggwave also has an optional direct-sequence spreading mode (`GGWAVE_OPERATING_MODE_USE_DSS`), which is off by default and was not tested.

## 5. Implementations

| Implementation | Modem | Session layer | Notes |
|---|---:|---:|---|
| TypeScript (`web/src`) | ≈ 1.6 k | ≈ 1.7 k | Reference modem receiver and session layer; Web Audio, AudioWorklet and a Worker |
| Swift (`ios/Sources/BySound`) | ≈ 2.0 k | ≈ 1.9 k | vDSP FFT at 48 kHz, Goertzel at 44.1 kHz; AVAudioEngine |
| Kotlin (`android/bysound/src/main`) | ≈ 2.1 k | ≈ 1.4 k | Modem and session in plain Kotlin (testable on the JVM); AudioTrack/AudioRecord |
| C99 (`c/src`) | ≈ 1.7 k | ≈ 0.9 k | No `malloc`, streaming transmitter and receiver; FFT (float) or Goertzel engine |

Lines are counted with `wc -l`, comments and blank lines included, test files and simulation helpers excluded. **Modem:** `web/src/modem`; the Swift files directly in `ios/Sources/BySound` except `BySoundSession.swift`; the Kotlin `modem` package plus `audio/SoundSender.kt` and `audio/SoundListener.kt`; `c/src` except `session.c` and `sha256.c`, including the internal tables header. **Session layer:** `web/src/session`; `ios/Sources/BySound/Session` plus `BySoundSession.swift`; the Kotlin `session` package plus `audio/BySoundSession.kt`; `session.c` and `sha256.c`. The C public headers are not counted (`bysound.h` ≈ 0.4 k lines, `bysound_session.h` ≈ 0.3 k). The Swift and Kotlin figures include the audio input/output wrappers.

The four modem implementations are tested against the frames in `test-vectors/v4-frames.json`, and the Swift and Kotlin tests also decode audio produced by the C implementation and one recorded transmission. The four session layers are tested against `session-acks.json`, `v4-acks.json`, `v4-risk.json` and `security-levels.json` and run the same scenarios on simulated air (Section 7). On the benchmark host the C modem compiles to about 35 KB of object code (`__TEXT` of the `c/src` objects other than the session layer, arm64, `-O2`), against about 60 KB for `ggwave.o`; the session layer adds about 19 KB.

The ESP32-S3 demo builds with ESP-IDF 5.4 and runs the session layer on the four ladder bands with two robust-capable demodulators; its image is 286.7 KB, with 150.1 KB of `.bss` and 10.8 KB of `.data` and no PSRAM (from `idf.py size`, see [`esp32/README.md`](esp32/README.md)). Without the robust receiver it needs about 85 KB less RAM. These figures come from the build, not from the benchmark. The demo needs an external I2S microphone and an I2S amplifier, and it has not been run on a board, so on-device behaviour and CPU load are not measured.

The [`examples/`](examples) folder holds small complete applications built on the libraries: a chat, a small file, Wi-Fi setup and the channel ladder in the browser, a Node command-line tool, WAV tools in C, link sharing on iOS, contact sharing on Android and the device side of Wi-Fi setup on the ESP32-S3. They are not part of the evaluation.

## 6. Evaluation

### 6.1 Method

Both libraries are compiled from source with the same compiler. The bysound side is the **C library** in [`c/`](c) (FFT engine). ggwave is fetched at commit `060aec7` and used through its C API with its **default parameters**. Each trial:
1. Encodes a payload: 8–128 B of ASCII without digits, so bysound's packing gives no advantage; 512 and 2048 B for bysound only, where they fit in its 30 s cap (2048 B only on `wide`); plus a UTF-8 case.
2. Passes it through a channel: white, pink or babble-like noise at a given SNR, optional echo (two delayed taps), ±100 ppm clock drift, lead silence.
3. Feeds the result to each library's own streaming receiver in 1,024-sample chunks.

**Receivers.** The bysound receiver is told which profile to listen for (normal or robust) and then listens to every channel of that profile at once (7 normal, 4 robust). The ggwave receiver listens to all its protocols, as its defaults do. The two are therefore not given the same prior information: ggwave searches a larger set.

**Metrics.** Success means the exact payload came out. SNR is defined over the full 0–24 kHz band; in-band and equal-peak SNR are also reported. SNR90 is the lowest grid SNR from which every higher grid point, the noiseless point included, reached at least 90 % success. Unless stated, a cell is 50 trials at 48 kHz on a 2 dB grid. No confidence intervals are given: with 30–50 trials per cell, one trial is 2–3 percentage points. Full method: [bench/README.md](bench/README.md).

**Sources of the numbers.** Every figure in this section is computed from the raw results in [`bench/results/`](bench/results). Airtimes, including those at 140 B and 1 KB, are the length of each encoder's output (`airtime.csv`) and were not passed through the channel. The ESP32 footprint (Section 5) comes from the ESP32 build and the line counts from `wc -l`; neither is a benchmark result. The session layer (Section 7) is not part of the benchmark.

### 6.2 White noise

<p align="center"><img src="bench/results/charts/snr_ultrasonic_1x.svg" alt="Success vs SNR, near-ultrasonic" width="100%"></p>

SNR90, 32-byte message, one transmission:

| Configuration | In band | Equal peak | Full band |
|---|---:|---:|---:|
| bysound `top` | −2 dB | −7 dB | −14 dB |
| bysound `high` / `upper` / `mid` | −2 dB | −4 dB | −12 dB |
| bysound `xhigh` | −1 dB | 0 dB | −10 dB |
| bysound `wide` | −1 dB | +3 dB | −8 dB |
| bysound `audible` | −1 dB | −1 dB | −10 dB |
| bysound `top` robust | −4 dB | −10 dB | −16 dB |
| bysound `high/upper/mid` robust | −4 dB | −6 dB | −14 dB |
| ggwave ultrasound (normal/fast/fastest) | −1 dB | +4 / +4 / +5 dB | −8 dB |
| ggwave audible (normal/fast/fastest) | −1 dB | +4 / +4 / +5 dB | −8 dB |
| ggwave DT fastest | +8 dB | +4 dB | −4 dB |

The three columns answer different questions. *In band* compares the detectors at the same SNR inside the band each mode uses. *Equal peak* compares the modes when both are played at the same peak amplitude (full-band SNR plus the mode's PAPR). *Full band* compares them at the same average power against noise spread over 0–24 kHz, and by construction it favours whichever mode uses the narrower band.

**Reading.**
- **In band**, bysound is within 0–3 dB of ggwave: equal for `wide`, `xhigh` and `audible`, 1 dB better for the 2- and 3-tone channels, 3 dB better for the robust profile. The detectors are close.
- **Full band**, the 2- and 3-tone channels need 4–6 dB less SNR than ggwave's ultrasound protocols, `xhigh` and `audible` 2 dB less, and the robust profile 6–8 dB less. `wide`, on the same 15–19.4 kHz span as ggwave ultrasound, is equal. The difference is mostly item 1 (bandwidth).
- **Equal peak**, the difference is 1–12 dB for the normal profile and up to 15 dB for `top` robust. Of that, PAPR alone contributes 0.8–6.6 dB (item 2); the rest is the full-band difference above. `wide` is 1–2 dB better than ggwave ultrasound (+3 dB against +4/+5 dB; lower is better). This comparison is against ggwave's 6-tone protocols; its DT protocols have a PAPR of 7.3–7.8 dB and its MT protocols 4.0 dB.
- **Equal power is not equal energy.** The modes take different times to send the same 32 bytes: sound is on for 1.98 s on `top`, 1.44 s on `high`, 0.86 s on `wide` and 3.97 s on `top` robust, against 1.71 s for ggwave fastest. Correcting the full-band difference for the ratio of sound-on times, the advantage over ggwave ultrasound fastest in energy per message is 5.3 dB for `top`, 4.7 dB for `high`, 3.0 dB for `wide` (same SNR90 in half the time), 4.3 dB for `top` robust and 3.7 dB for `high` robust: roughly 3–5 dB in every case. ggwave's slower speeds reach the same SNR90 as its fastest on this grid.

The ggwave `dt-normal` and `dt-fast` protocols have no SNR90 at 32 B because they decoded 86 % of the noiseless trials (100 % at 0 dB). The noiseless point is the signal at −40 dBFS surrounded by exact digital silence; in a separate scan at full scale they missed 0 of 500 payloads. This looks like an artefact of the test condition rather than a property of the protocols, and the noiseless-point requirement in SNR90 penalises only these two rows.

<p align="center"><img src="bench/results/charts/snr_ultrasonic_peak_1x.svg" alt="Success vs SNR at equal peak level" width="100%"></p>

### 6.3 Echo, coloured noise and drift

SNR90 in dB (full band), 32 B, one transmission ¹:

| Configuration | White | Echo | Pink | Babble-like | Drift ±100 ppm | All combined |
|---|---:|---:|---:|---:|---:|---:|
| bysound `top` | −12 | −8 | −20 | ≤ −32 | −12 | −16 |
| bysound `top` robust | −16 | −12 | −24 | ≤ −32 | −16 | −20 |
| bysound `wide` | −8 | −4 | −16 | ≤ −32 | −8 | −12 |
| bysound `audible` | −8 | −4 | −8 | −12 | −8 | −4 |
| ggwave ultrasound fastest | −8 | 0 | −16 | ≤ −32 | −8 | −8 |
| ggwave audible fastest | −8 | 0 | −8 | −12 | −8 | 0 |

¹ The impairment columns use 30 trials per cell on a 4 dB grid. The White column is the Section 6.2 data (50 trials) read on the same 4 dB grid, which is why `top` shows −12 dB here and −14 dB in Section 6.2. −32 dB is the lowest point of the grid. "All combined" is echo, +100 ppm drift and pink noise.

**Echo.** With one transmission, bysound tolerates 4–12 dB more noise than ggwave under this echo: 8 dB for the 2- and 3-tone channels (−8 dB against 0 dB), 12 dB for `top` robust, and 4 dB for `xhigh`, `wide` and `audible`. With two transmissions ggwave's ultrasound protocols improve to −4 dB, which leaves 4–8 dB for the 2- and 3-tone channels and `top` robust, and none for `wide`. Relative to white noise alone, the echo costs every bysound configuration in the table 4 dB and ggwave 8 dB. That 4 dB is the part of the difference attributable to echo subtraction (item 6); the rest is the white-noise difference of Section 6.2. The echo is a single synthetic profile (direct path plus taps of 0.5 at 8 ms and 0.25 at 23 ms), not a measured room response.

**Coloured noise.** Pink and babble-like noise have most of their power below 5 kHz, so every near-ultrasonic mode of both libraries decodes at very low full-band SNR; the in-band SNR is much higher. Under pink noise the 2- and 3-tone channels reach 4 dB lower than ggwave ultrasound, and `wide` the same. Under babble-like noise both libraries reach the bottom of the grid, so the test does not separate them.

**Drift.** Clock drift of ±100 ppm did not change the SNR90 of any configuration in this table.

<p align="center"><img src="bench/results/charts/cond_echo_1x.svg" alt="Success vs SNR with echo" width="100%"></p>

### 6.4 Airtime, throughput and message size

| Configuration | 8 B | 32 B | 128 B | 140 B | B/s at 128 B |
|---|---:|---:|---:|---:|---:|
| bysound `top` | 1.57 s | 2.33 s | 6.30 s | 6.69 s | 20.3 |
| bysound `high/upper/mid` | 1.28 s | 1.79 s | 4.41 s | 4.67 s | 29.0 |
| bysound `wide` | **0.96 s** | **1.21 s** | **2.53 s** | **2.65 s** | **50.7** |
| bysound `top` robust | 2.78 s | 4.32 s | 12.25 s | 13.02 s | 10.4 |
| ggwave fastest | 1.00 s | 1.71 s | 4.59 s | 4.97 s | 27.9 |
| ggwave fast | 1.32 s | 2.73 s | 8.49 s | 9.26 s | 15.1 |
| ggwave normal | 1.64 s | 3.75 s | 12.39 s | 13.55 s | 10.3 |

All airtimes are the length of the encoder output as recorded in `bench/results/airtime.csv`, with no lead silence. bysound's include its 350 ms trailing gap; ggwave's include its start and end markers. The fastest configuration in each column is in bold.

- On the same span, `wide` is 1.8× faster than ggwave fastest at 128 B.
- At 8 B only `wide` (0.96 s) is faster than ggwave fastest (1.00 s); the other normal-profile channels take 1.09–1.57 s. The fixed preamble, header and gap weigh most on short messages.
- `high`, `upper` and `mid` are about as fast as ggwave fastest from about 64 B (2.75 s against 2.67 s) and slightly faster at 128 B and 140 B.
- 140 B is ggwave's maximum per transmission. bysound caps a transmission at **30 seconds** instead of at a byte count, and the encoder rejects anything longer. That allows 700 B on `top`, 1,052 B on `high`/`upper`/`mid` and 2,114 B on `wide` (335 B and 520 B in the robust profile); the exact limits per channel are in [PROTOCOL.md](docs/PROTOCOL.md). For example, 1 KB takes 14.94 s on `wide`.

The code rate differs between the two libraries and with message length (bysound adds 14 bytes per 48-byte block plus a 4-byte CRC; ggwave adds 3 bytes plus parity that grows with length), so equal airtime does not mean equal redundancy.

<p align="center"><img src="bench/results/charts/throughput_vs_size.svg" alt="Throughput vs payload size" width="100%"></p>

### 6.5 Wrong payloads

| | Trials | Wrong payloads delivered |
|---|---:|---:|
| bysound | 87,380 | 0 |
| ggwave, default variable-length protocols | 67,320 | 34 ² |

bysound delivered no wrong message in 87,380 simulated trials; a CRC-32 over the whole text guards delivery. In 30 minutes of noise with no transmission (10 minutes each of white, pink and babble-like), neither library reported a detection: none for bysound's normal receiver, none for its robust receiver, and none for ggwave.

Of ggwave's 34 wrong payloads:

- **32 were 8-byte messages**, 31 of them on the fast and fastest speeds of the audible and ultrasound protocols and 1 on ultrasound normal. They occurred at SNRs from −12 to +4 dB and at the noiseless point, and in each of those trials the correct payload was not delivered. A separate scan without any channel shows the same: 11–21 of 500 different 8-byte payloads decoded to a wrong payload on those speeds.
- **2 were 32-byte messages** in two-transmission trials (ultrasound fastest), in which the correct payload was also delivered.

This is consistent with Reed–Solomon mis-correction, though the cause was not traced in ggwave's code: an 8-byte message carries 4 parity bytes, there is no checksum after the RS decode, and the receiver attempts the decode at many time alignments. With 12 parity bytes (32 B) and more, wrong payloads were rare (2) or absent. bysound's CRC-32 (item 8) is the check that turns such a case into a rejection.

² The mono-tone protocols, which ggwave only accepts in fixed-length mode, were run separately (2,970 trials). They delivered 315 wrong payloads in 148 trials, repeats within a trial counted, all for 8-byte messages; 74 of the 315 were at the noiseless point, so part of this is probably an artefact of digital silence. These runs are not part of the comparison above.

### 6.6 Cost

Receiver CPU time on the benchmark host (Apple M4 Pro), expressed as a real-time factor (seconds of audio processed per second of CPU; higher is cheaper):

| | bysound | ggwave | ggwave is lighter by |
|---|---:|---:|---:|
| Idle listening (60 s segments of noise, median) | 371× (normal, 7 channels), 463× (robust, 4 channels) | 18,993× | 41–51× |
| 32 B trials that decoded the payload (median per configuration) | 426–533× (6.8–12.4 ms per trial) | 660–5,903× (0.9–8.2 ms per trial) | 1.2–14× |
| All 32 B trials (median per configuration) | 436–534× (6.7–12.1 ms per trial) | 7,585–18,319× (0.3–0.6 ms per trial) | 14–42× |

The decoded-trial and all-trial rows are medians over the one-transmission trials of the white-noise plan, per configuration (11 for bysound, 9 for ggwave), receiver initialisation included; the last column divides the extremes of the two ranges. The all-trials row mixes in the many trials below the threshold, where ggwave does almost no work, so the first two rows describe the two real situations better. bysound's cost is nearly constant because it searches every channel at every step (4 ms, 8 ms in the robust profile) whether or not a message is present. Both run far faster than real time on this host. CPU load on a phone or a microcontroller was not measured.

## 7. Session layer and security

The modem moves one text from a speaker to a microphone, intact or not at all. The session layer turns that into a two-party exchange. It is specified in [SESSION.md](docs/SESSION.md) and implemented in TypeScript ([`web/src/session/`](web/src/session), the reference), Swift, Kotlin and C, sharing the test vectors `session-acks.json`, `v4-acks.json`, `v4-risk.json` and `security-levels.json` in [`test-vectors/`](test-vectors). Its security model, including what it does not protect against, is in [SECURITY.md](docs/SECURITY.md). **The session layer is not part of the benchmark in Section 6**; its behaviour is checked by simulated tests in each implementation (in TypeScript `session.test.ts`, `adapt.test.ts`, `security.test.ts` and `frames.test.ts`; in Swift `SessionAir.swift`, in Kotlin `SimAir.kt`, in C `test_session.c`), which mix the sound of several simulated devices into each microphone with white noise.

**Frames.** Two v4 frame types are used besides the payload and reference frames, both in the normal profile with a single RS codeword (8 parity bytes) and a CRC-16:
- **Ack and receipt** (`0x41`): the first 4 bytes of SHA-256 of the text, plus the channel and profile where the receiver read it. Acks are looped on `mid`, receipts on `high`; the two channels share no tone, so both sides can play at once.
- **Risk** (`0x58`): a reason (another transmission, a clone of the sender's own data, an overlap), the source (sender or receiver), a hop count and short identifiers of the two texts involved.

**Exchange.** The sender listens before it plays (3 s plus a random extra of up to 1.2 s at the default level), then plays one repetition at a time with a 2 s listen window after each. The receiver keeps listening for 1.5 s after the first frame (up to 6 s if a frame starts on another channel), delivers the text and loops acks until it hears a receipt. The sender stops on the ack and answers with receipts.

**Confirm gate.** After delivery the receiver keeps watching until the application calls `confirmed()`, typically right before it acts on the text. A different text or an alert heard in the meantime makes `confirmed()` fail.

**Second senders and alerts.** A second emitter is detected when a microphone hears it: by the sender before it plays or in its listen windows (including its own data played by another device), and by the receiver as two different texts or as overlapping frames (a clash). Nothing is delivered from a clash. Whoever detects it plays a risk frame; a sender that hears one stops and relays it once, with a maximum of two hops. Alerts reach the application through a callback, together with a suggested quarantine time. **Quarantine is only a callback**: bysound never refuses or delays an operation because of it.

**Security levels.** One option, `securityLevel`, sets the defaults of all of the above:

| | basic | standard | maximum (default) |
|---|---|---|---|
| Integrity (CRCs, Reed–Solomon) | yes | yes | yes |
| Sender pre-listen | 1 s, does not act | 2 s + up to 0.6 s | 3 s + up to 1.2 s |
| Detection of other emitters, clash rejection | no | yes | yes |
| Confirm watch | no | up to 120 s | up to 120 s |
| Emits and honours risk frames | no | yes | yes |
| Suggested quarantine | 30 s (unused) | 30 s | 60 s |

Every field can be overridden; the exact table is `test-vectors/security-levels.json`.

**Automatic adaptation.** Without a fixed channel, the sender adapts:
- **Channel ladder:** it plays 3 repetitions on each of `top`, `high`, `upper` and `mid`, from the least audible down, until an ack arrives.
- **Ambient check:** during the pre-listen it measures the noise of each channel, drops channels more than 10 dB noisier than the quietest, and starts in the robust profile if the first channel is loud. These thresholds are not calibrated in the field.
- **Robust switch:** after 6 normal-profile repetitions without an ack, it continues in the robust profile.
- **Calibration memory:** the ack says where the receiver read the frame; the sender stores that channel and profile and starts there next time (with 6 repetitions per channel), for up to 10 sends unless it is confirmed again. With a simulated speaker that cannot play above about 17 kHz, the first send steps down the ladder to `mid`, and the next send starts on `mid` and is read on its first repetition (`adapt.test.ts`).

**Limits.** The session layer adds detection and signalling, not secrecy or authentication. The text travels in the clear; anyone can play a valid frame, record and replay one, or play a fake ack or a fake alert. A fake alert can stop exchanges nearby (a denial of service) but cannot cause a wrong delivery. An emitter that no microphone hears cannot be detected. Applications that need confidentiality, sender authentication or single use add them inside the text (encryption, a signature, a nonce and an expiry). In the TypeScript and Swift implementations, ack and risk frames are read by a separate, smaller decoder that does not subtract echo or combine laps, so control frames are somewhat less robust than data in heavy noise; the Kotlin and C modem receivers decode them directly. Receipts are played on `high`, so a sender whose speaker cannot play above about 17 kHz cannot deliver them, and its receiver keeps acking for the full 30 s.

## 8. Trade-offs

The table collects the comparison in one place. Every row refers to the evidence behind it; rows marked "not measured" describe design or ecosystem, not benchmark results.

| Dimension | bysound | ggwave | Evidence |
|---|---|---|---|
| White noise, in band | SNR90 −1 to −4 dB | −1 dB (6-tone protocols) | Within 0–3 dB. Section 6.2 |
| White noise, full band and equal peak | 2–8 dB lower SNR90 than ggwave ultrasound (`wide` equal); 1–15 dB at equal peak | — | Full band favours narrow bands by construction; about 3–5 dB per unit of energy. Section 6.2 |
| Echo (one synthetic profile) | Costs 4 dB relative to white noise | Costs 8 dB | 4–12 dB difference with one transmission, 0–8 dB with two. Section 6.3 |
| Pink and babble-like noise | Pink: −20 dB on the 2- and 3-tone channels, −16 dB on `wide` | Pink: −16 dB (ultrasound) | Babble-like: both at the grid floor. Section 6.3 |
| Most noise-tolerant mode measured | `top` robust: −16 dB (4 dB grid), 4.32 s for 32 B | `mt-normal`: −20 dB, 16.9 s for 32 B; receiver must know the length | Sections 6.3 and 6.5 (footnote 2) |
| Wrong messages delivered | 0 in 87,380 trials; CRC-32 over the text | 34 in 67,320 trials (32 at 8 B); no checksum after RS | Section 6.5 |
| Airtime, short messages (8 B) | 0.96 s (`wide`) to 1.57 s (`top`) | 1.00 s (fastest) | Section 6.4 |
| Airtime, 128 B | 2.53 s (`wide`), 4.41 s (`high`), 6.30 s (`top`) | 4.59 s (fastest) | Section 6.4 |
| Maximum message per transmission | 335–2,114 B (30 s cap) | 140 B (64 B fixed length) | Section 6.4 |
| Audibility | `top` at 18–19.5 kHz, `high` at 17.25–19.5 kHz; an audible channel | Ultrasound at 15–19.4 kHz; audible, DT and MT protocols | Section 3. No listening test |
| CPU while idle | 371–463× real time | 18,993× | ggwave 41–51× lighter. Section 6.6 |
| CPU while decoding | 426–533× real time | 660–5,903× | ggwave 1.2–14× lighter. Section 6.6 |
| Code and memory | C: no `malloc`; ≈ 35 KB object code on the host (+ ≈ 19 KB session layer); ESP32-S3 image 287 KB, ≈ 161 KB static RAM with the session layer (build only) | ≈ 60 KB object code on the host; microcontroller footprint not measured here | Section 5 |
| Acks, clash detection, alerts, adaptation | Specified; implemented in all four languages; simulated tests | Not provided | Section 7, not measured in the benchmark |
| Implementations and platforms | Four native implementations (TypeScript, Swift, Kotlin, C99) sharing test vectors, covering the browser, iOS, Android, desktop and ESP32-S3 | One C++ core with ready-made bindings (Python, JavaScript/WebAssembly, an iOS Swift package) and examples for Arduino, RP2040 and ESP32 boards | Repositories; not measured |
| Maturity | New; no over-the-air measurement yet; ESP32 demo not yet run on a board | Years of public use, a larger community, a single self-contained source file | Not measured |

## 9. Limitations

**Scope of the evidence**

- **Simulation only.** The channel has no speaker or microphone frequency response, no non-linearity, no automatic gain control or noise suppression, and no measured room response. Many phone speakers and microphones lose sensitivity above about 17 kHz, which would affect `top` (18–19.5 kHz) the most and could change the ranking of the modes. Apart from one recorded transmission in the test suite, there is no over-the-air test yet.
- **No hardware measurements.** The ESP32 demo builds but has not been run on a board; it needs an external I2S microphone and amplifier. CPU load was measured only on a desktop host.
- **No listening test.** The statements about audibility rest on general knowledge of adult hearing, not on a test with listeners. Younger listeners hear higher frequencies, and a speaker driven hard can produce audible artefacts.
- **Only the C receiver was measured.** Soft symbols, erasure decoding and lap combining (items 7, 9 and 13) exist in the TypeScript, Swift and Kotlin receivers and are not in these numbers. Clash detection and text packing were not exercised either.
- **The session layer was not measured.** Its tests run on simulated air with white noise, and its ambient thresholds are not calibrated in the field.

**Fairness of the comparison**

- **Author bias.** The benchmark was written by the author of bysound. ggwave runs unmodified with its defaults and the code is public, but the choice of conditions, payloads and metrics is the author's.
- **Prior information.** The bysound receiver is told the profile; ggwave listens to all its protocols.
- **Full-band SNR favours narrowband modes by construction.** The in-band and equal-peak figures are given alongside for that reason, and the energy-per-message figure in Section 6.2 shows that part of the remaining difference is transmission time.
- **Code rate differs** between the libraries and with message length, so the comparison is between complete systems, not between detectors at equal redundancy.
- **ggwave options not tested:** its direct-sequence spreading mode, 44.1 kHz operation, and receivers restricted to one protocol.

**Statistics and models**

- 30–50 trials per cell (20 at 512 B, 10 at 2048 B) on a 2 or 4 dB grid, with no confidence intervals. SNR90 values are quantised to the grid, and differences of one grid step are within the resolution of the method.
- The noise models are synthetic; the babble-like noise is shaped noise, not recorded speech. The echo is a single two-tap profile.
- The noiseless point (signal at −40 dBFS with digital silence around it) does not occur with a real microphone. It affects ggwave's `dt-normal` and `dt-fast` rows and probably part of the mono-tone wrong payloads.
- Payloads are random ASCII without digits. Real payloads with numbers would be shorter on bysound because of digit packing.

**Security**

- **No secrecy and no sender authentication.** Anyone nearby can record the sound, and anyone can play a valid frame. A CRC-32 detects accidental errors, not deliberate ones. Applications must not send secrets in the clear and must validate what they receive ([SECURITY.md](docs/SECURITY.md)).

## 10. Conclusion

bysound keeps ggwave's core (MFSK on a 46.875 Hz grid with Reed–Solomon) and changes the framing and the receiver. In this simulation the two detectors are within 0–3 dB of each other at equal in-band SNR. The larger differences come from design choices around the detector: narrow channels and a lower peak-to-average power ratio give more margin against broadband noise for the same speaker level, echo subtraction accounts for a 4 dB smaller echo penalty under one synthetic echo, and RS in blocks with an end-to-end CRC-32 allows longer messages and produced no wrong delivery in 87,380 trials. On top of the modem, a session layer adds acknowledgements, second-sender detection, alerts and automatic channel adaptation; it is specified and implemented in all four languages but not measured here. The costs are CPU, most visibly while idle, and airtime for very short messages on the narrow channels. Whether these results hold over the air, with real speakers, microphones and rooms, has not been measured and is the next step. The code, test vectors, benchmark and raw results are MIT-licensed.

## Reproducibility

```sh
cd bench && ./fetch-ggwave.sh && make && make run     # ≈ 8 min on an Apple M4 Pro
```

Raw CSV files, the summary and every chart are in [`bench/results/`](bench/results). Results are deterministic except for the CPU columns.

## Disclosure

**Conflict of interest.** The benchmark was designed and written by the author of bysound, one of the two systems compared.

## Acknowledgements

The implementations, the benchmark and the drafting of this report were developed with the assistance of AI tools (Anthropic's Claude). The author designed the protocol, directed the work, reviewed the results and is responsible for all content. ggwave is the work of Georgi Gerganov and contributors.

## References

1. G. Gerganov, *ggwave: Tiny data-over-sound library*, https://github.com/ggerganov/ggwave (commit 060aec7).
2. B. Armstrong, *Quiet: transmit data with sound*, https://github.com/quiet/quiet.
3. K. Mostafa, *minimodem: general-purpose software audio FSK modem*, https://github.com/kamalmostafa/minimodem.
4. R. Nandakumar, K. K. Chintalapudi, V. N. Padmanabhan, R. Venkatesan, "Dhwani: Secure Peer-to-Peer Acoustic NFC", *ACM SIGCOMM 2013*.
