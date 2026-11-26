# bysound security model

What bysound protects, what it does not, and what an app has to add. The mechanisms are specified in [PROTOCOL.md](PROTOCOL.md) (frames and integrity) and [SESSION.md](SESSION.md) (acks, the confirm gate, other emitters, risk signal, security levels).

**In one sentence:** bysound delivers the exact text that was played, or nothing, and tells you when more than one device was playing; it does not hide the text, and it cannot tell you *who* played it.

## What bysound guarantees

**Integrity: the exact text or nothing.** A delivered text has passed, in order: a per-band preamble, a header chosen only among valid headers and protected by a CRC-8 (which also covers the band and the profile), Reed–Solomon on every block with consistency checks on block index and count, a CRC-32 over the whole text, and strict unpacking (invalid bytes or invalid UTF-8 reject the frame). A frame that fails any of them is dropped, and the receiver waits for the next repetition. Noise, echo and interference can delay a read or prevent it; they do not change what is read. In reference mode, the text the app's resolver returns must also hash to the fingerprint that was heard (SHA-256, 96 bits). Acks and risk frames carry their own CRC-16 and Reed–Solomon parity.

**Detection of a second emitter, when the microphone hears it.** With `standard` or `maximum` (the default):

- a sender does not start if another bysound transmission is already sounding (pre-listen), and stops if it hears one in its listen windows, or hears its own data played by someone else (a clone);
- a receiver delivers nothing if, before delivery, it reads two different texts or two frames overlap (a clash), on the same band or on different ones;
- after delivery, the receiver keeps watching until the app calls `confirmed()`; a different text or an alert heard meanwhile makes `confirmed()` fail.

**Alert propagation.** Whoever detects another emitter plays a risk frame. A receiver that hears one before delivery fails; during the confirm watch it closes the gate. A sender that hears one stops and relays it once, one hop further, up to two hops. Every alert reaches the app through `onAlert` and a suggested quarantine through `onQuarantine`.

## What bysound does not do

- **No secrecy.** The text travels in the clear. Anyone within earshot (including a recorder in a pocket) can decode it. Reference mode sends only a 12-byte fingerprint, but anyone who can guess or enumerate the possible texts can match it.
- **No sender authentication.** Anyone can play a valid frame. bysound cannot tell the shop's phone from a phone that copies it.
- **No replay protection.** A recording plays back as a valid frame. Acks and receipts carry 4 bytes of SHA-256 of a text that may be public: they can be recorded and replayed too. An ack means "a bysound receiver nearby says it read this", not proof of anything.
- **No guarantee of detection.** An emitter that does not reach a microphone cannot be detected by it. A much weaker second emitter on the same band may stay hidden under the stronger one. A second emitter that starts and stops entirely while a half-duplex sender is playing is invisible to that sender (the receiver still protects itself if it hears both). Only bysound frames are recognised; any other sound is noise. After the confirm watch ends, nothing is watched.
- **No enforcement.** Quarantine is a suggestion delivered to the app; bysound never blocks an operation because of it. A device that does not run bysound ignores risk frames.
- **Range is the only boundary.** Near-ultrasonic sound goes through doors and thin walls less than radio, but it is not a perimeter you can rely on.

## Attacks considered

| Attack | What happens | Mitigation |
|---|---|---|
| **Eavesdropping**: someone nearby records and decodes. | They get the text. | Do not send secrets. Use reference mode when the receiver already has the possible texts; send an opaque, short-lived id that only your app can resolve; or encrypt inside the text at the app level. |
| **Replay**: a recording of a past exchange is played again. | It decodes as a valid, unchanged frame. If it sounds while the real sender is sending, the sender reports a clone (`CLONED_TRANSMISSION`) and the receiver a clash or two texts. If it sounds alone, it is delivered. | Make texts single-use at the app level: a nonce or a counter checked by whoever acts on it, an expiry time (TTL), a signature over both. |
| **Jamming**: loud noise or tones on the bands. | Reads are delayed or prevented; corrupted frames are dropped, never delivered wrong. The sender's ambient check skips the noisiest channels and goes robust. | None needed for integrity. For availability, a fallback channel in the app (QR, NFC, typing a code). |
| **Rogue sender racing the real one**: a nearby device plays its own text (another account, another amount) at the same time. | The receiver reads two texts or a clash: nothing is delivered, it plays a risk frame, the real sender stops and relays it. If the rogue starts after delivery, the confirm gate fails. If the real sender hears the rogue, it stops and alerts too. | Use the default `maximum` (at least `standard`) on both sides, call `confirmed()` before acting, show the alert to the person. |
| **Rogue sender alone**: the real sender is silent or out of range, and only the rogue plays. | Its text is delivered: bysound has nothing to compare it with. | Validate the content: whose text is it? Sign it at the app level, or check it against something the person sees (an amount on the shop's screen, a name). |
| **Cloned frames**: a device replays the real sender's own frames, possibly on another band, to confuse or to steal the ack. | The real sender detects its own data played while it is silent or on a band it is not playing (`CLONED_TRANSMISSION`) and alerts. Overlapping copies make a clash at the receiver. | As for replay: single-use texts. |
| **Fake ack**: someone plays an ack for a public text to make the sender stop. | The sender stops and believes it was received. Nothing wrong is delivered to anyone. | Do not treat the ack as a proof of delivery when it matters; confirm at the app level. |
| **Fake alert**: someone plays risk frames to stop exchanges nearby. | bysound senders stop and receivers fail (with `honor`), for as long as it plays: a denial of service, never a wrong delivery. | Retry; with `riskSignal.honor: false` an app can only report alerts. |

## Security levels

| | basic | standard | maximum (default) |
|---|---|---|---|
| Integrity (CRCs, Reed–Solomon) | yes | yes | yes |
| Sender pre-listen | 1 s, does not act | 2 s + up to 0.6 s, refuses if busy | 3 s + up to 1.2 s, refuses if busy |
| Sender watches while sending | no | yes | yes |
| Receiver final listen, clash rejection | no (first frame delivered) | 1.5 s (up to 6 s) | 1.5 s (up to 6 s) |
| Confirm watch until `confirmed()` | no | yes, up to 120 s | yes, up to 120 s |
| Emits / honors risk frames | no / no | yes / yes | yes / yes |
| Risk loop: receiver / sender | — | 4 s / 15 s | 6 s / 30 s |
| Suggested quarantine | 30 s (unused) | 30 s | 60 s |

Exact values: [`test-vectors/security-levels.json`](../test-vectors/security-levels.json). Every field can be overridden. **With `basic`, a receiver can act on a rogue's text even when two senders were playing**: use it only for demos and data without consequences.

## Guidance for apps

1. **Validate the content.** bysound tells you the text is the one that was played, not that it is legitimate. Parse it strictly and check it against your rules.
2. **Use the confirm gate.** Call `await confirmed()` right before acting on the data (paying, unlocking, saving), not right after receiving it. If it fails, do not act; show the alert. If it answers `expired`, decide what an unwatched gap means for you and log it.
3. **Keep the default level** on both sides for anything with consequences.
4. **Add what bysound does not provide, inside the text:** a signature or MAC if you need to know who sent it; a nonce or a counter and an expiry (TTL) if it must be used once; encryption if it must stay private. bysound transports any text you put in, up to 255 blocks.
5. **Show alerts to the person.** A risk alert means another device is playing nearby: it may be harmless (two people paying at the same counter), or it may be an attack. The suggested quarantine is a reasonable time to wait before retrying.
6. **Do not treat acks as proof.** They are for the experience (stop playing, show a check mark).

## bysound and ggwave

Only properties that can be checked in ggwave's public README and source ([`bench/third_party/ggwave`](../bench/third_party/ggwave)) are listed. "Not provided" means not found in either.

| | bysound | ggwave |
|---|---|---|
| Error correction | Reed–Solomon per 62-byte block (12 parity bytes), erasures from soft decisions, combining of repetitions | Reed–Solomon; parity bytes depend on the length (`max(4, 2·⌊len/5⌋)`, 2 below 4 bytes; `getECCBytesForLength` in `src/ggwave.cpp`) |
| Checksum beyond the error-correcting code | CRC-8 on the header, CRC-32 on the text (CRC-16 on fingerprint, ack and risk frames) | Not provided (decoding succeeds when Reed–Solomon decodes) |
| Exact text or nothing | Yes (CRC-32 after Reed–Solomon, strict UTF-8) | Not documented |
| Maximum length | 255 blocks of 48 bytes | 140 bytes variable-length, 64 fixed-length (`kMaxLengthVariable`, `kMaxLengthFixed`) |
| Detects a second emitter / overlap | Yes (pre-listen, windows, clash, confirm watch) | Not provided |
| Acknowledgement by sound | Yes (ack + receipt) | Not provided |
| Alert propagation between devices | Yes (risk frames, up to 2 relays) | Not provided |
| Security levels | basic / standard / maximum | Not provided |
| Encryption | Not provided | Not provided (the optional "Direct Sequence Spread" mode XORs data with a fixed sequence; it is not documented as a security feature) |
| Sender authentication, replay protection | Not provided | Not provided |
