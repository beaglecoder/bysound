# bysound for Android

Kotlin implementation of the **bysound v4** data-over-sound modem: text goes out as near-inaudible multi-tone FSK (channels between 15 and 19.5 kHz, plus an audible channel) with Reed–Solomon blocks and a CRC-32, and comes back in from the microphone. It follows the TypeScript reference in [`../web/src/modem`](../web/src/modem) and, on top of it, the session layer of [docs/SESSION.md](../docs/SESSION.md) ([`../web/src/session`](../web/src/session)): acks and receipts, the confirm gate, other-emitter detection, risk frames, security levels, reference mode and automatic channel adaptation.

## Install and build

Requirements: JDK 21 (the Gradle build needs it) and the Android SDK, found through `ANDROID_HOME` (or `ANDROID_SDK_ROOT`) or through `sdk.dir` in `android/local.properties` (gitignored). Android 7.0+ (`minSdk` 24).

The library is not published to Maven. Include the module from this repository in your Gradle build, as [`examples/android-share-contact`](../examples/android-share-contact) does:

```groovy
// settings.gradle
include ':bysound'
project(':bysound').projectDir = file('path/to/bysound/android/bysound')
// app/build.gradle
dependencies { implementation project(':bysound') }
```

The manifest declares `RECORD_AUDIO` and `MODIFY_AUDIO_SETTINGS`; request `RECORD_AUDIO` at runtime before listening.

## API

```kotlin
// Send (loops until stop)
val sender = SoundSender(context)
sender.send("Hello", Band.TOP)   // music volume to maximum, plays on the loudspeaker
sender.stop()                    // restores the volume

// Receive (RECORD_AUDIO must be granted)
val listener = SoundListener(context, onFrame = { heard ->
    println("${heard.band}: ${heard.text} (${heard.correctedBytes} bytes corrected)")
})
listener.start()
listener.stop()

// Pure Kotlin, no Android: samples and a receiver
val audio: FloatArray = Modem.encode("Hello", Band.MID, Profile.NORMAL, sampleRate = 48000)
val rx = Receiver(48000) { heard -> println(heard.text) }
rx.feed(audio)
```

### Session (acks, confirm gate, risk frames, adaptation)

```kotlin
// Sender: listens first, repeats down a channel ladder until the receiver acknowledges, remembers where it was read
val send = sendSession(context, "pay 12.50 to shop 7", SenderOptions(onAlert = { a -> log(a) }, onQuarantine = { q -> hold(q.durationMs) }))
val d = send.delivered.await()          // suspend (Kotlin stdlib only), or .whenComplete { d, e -> ... }
println("read on ${d.band.id} by the other device")   // fails with SessionError(code, alert) otherwise
send.stop()

// Receiver: delivers after the final listen, acknowledges, keeps watching until you confirm
val rx = receiveSession(context, ReceiverOptions(securityLevel = SecurityLevel.MAXIMUM))   // maximum is the default
val got = rx.result.await()             // Received(text, band, via)
val gate = rx.confirmed().await()       // call right before acting; fails MULTIPLE_TRANSMISSIONS / RISK_SIGNAL / CANCELLED
rx.cancel()

// Reference mode: only the 12-byte fingerprint travels; the receiver resolves it from texts it already has
sendSession(context, "pay 12.50 to shop 7", SenderOptions(mode = Mode.REFERENCE))
receiveSession(context, ReceiverOptions(resolve = catalogResolver(listOf("pay 12.50 to shop 7"))))
```

- **Errors.** `sendSession` throws, on the caller's thread, `IllegalArgumentException` for an empty text and `TextTooLongException` if the text does not fit one transmission on every channel it may play (the fixed `band`, or every channel of the ladder). Both functions throw `IllegalStateException` if the microphone cannot be opened; if it fails later, the sender stops (`STOPPED`) and the receiver is cancelled (`CANCELLED`).
- **Robust profile.** A text that fits the normal profile but not the robust one stays in the normal profile when the ladder would switch to robust (`onRepetition` reports `NORMAL`).
- **Threads.** The state machine runs on its own handler thread: futures complete there, so hop to the main thread before touching views. A `resolveAsync` lookup may complete on any thread.
- **Calibration** (where the last receiver read this phone) is kept in SharedPreferences unless `SenderOptions.calibration` is given (`null`: none). Quarantine is only the `onQuarantine` callback; nothing is ever refused because of it.
- **Without Android** (tests, other audio back ends): implement `Platform` and feed `SenderSession.hear` / `ReceiverSession.hear` from `Ears` (see `bysound/src/test/.../session/SimAir.kt`).

### Length limit

One transmission (preamble + header + body + gap) never lasts more than 30 s (`Modem.MAX_FRAME_SECONDS`). A longer text throws `TextTooLongException` (an `IllegalArgumentException`, with `maxPackedBytes`); `Modem.maxPayloadBytes(band, profile)` gives the limit in packed bytes. ASCII letters pack 1:1, digit pairs 2:1.

| Channel | Max packed bytes, normal | Max packed bytes, robust |
|---|---:|---:|
| `TOP` | 700 | 335 |
| `HIGH`, `UPPER`, `MID` | 1,052 | 520 |
| `AUDIBLE` | 1,122 | – |
| `XHIGH` | 1,408 | – |
| `WIDE` | 2,114 | – |

## Tests

```sh
cd android
./gradlew :bysound:testDebugUnitTest
```

They check the vectors in `../test-vectors/` (`v4-frames`, `v4-acks`, `session-acks`, `v4-risk`, `security-levels`), Reed–Solomon, a render-noise-receive loopback on every channel at 44.1 and 48 kHz (short and multi-block UTF-8 text, robust profile), decoding of the C implementation's `c/test/golden/payload_wide_44100.i16` and of the recording `c/test/recordings/payload-top-v4.wav`, and the session scenarios of `web/src/session/*.test.ts` on simulated air (several devices, speakers, microphones and noise on one sample clock).

## Demo app

```sh
cd android
./gradlew :demo:installDebug
```

One screen: text, channel ("auto" runs a session), Send/Stop, Listen/Stop, a status line, a level bar and the list of received texts.

## Notes

- `bysound/`, the library:
  - `modem` package: pure Kotlin, no Android imports (so it tests on the JVM). `Band`, `Profile`, `Modem` (encode), `Receiver` (streaming decoder: payload, reference, ack and risk frames, plus per-channel noise reports), `ReceiverHost` (normal and robust receivers on the same audio), Reed–Solomon, CRCs, text packing.
  - `session` package: pure Kotlin, JVM-testable. `SenderSession` and `ReceiverSession` (state machines driven by a `Platform` — clock, timers, playback — and `AirEvent`s), frames (`ackFrame`, `riskFrame`, `parseAck`, `parseRisk`), `SecurityLevel`, calibration stores, `catalogResolver`.
  - `audio` package: `SoundSender` (AudioTrack on the built-in loudspeaker; sets the music volume to maximum while sending, only when the output is the loudspeaker), `SoundListener` (AudioRecord, `UNPROCESSED` source when available, 48 or 44.1 kHz, background thread) and the session API `sendSession` / `receiveSession` over both.
- `demo/`: the one-screen app above.
