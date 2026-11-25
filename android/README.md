# bysound for Android

Kotlin implementation of the bysound v4 data-over-sound modem: text goes out as near-inaudible multi-tone FSK (15-20 kHz bands, plus an audible band) with Reed-Solomon and CRC, and comes back in from the microphone. It follows the TypeScript reference in `../web/src/modem` (payload frames only; reference, ack and risk frames are not part of this port).

## What is inside

- `bysound/` - Android library.
  - `modem` package: pure Kotlin, no Android imports (so it tests on the JVM). `Band`, `Profile`, `Modem` (encode), `Receiver` (streaming decoder), `ReceiverHost` (normal + robust receivers on the same audio), Reed-Solomon, CRCs, text packing.
  - `audio` package: `SoundSender` (AudioTrack, built-in loudspeaker, music volume to maximum while sending) and `SoundListener` (AudioRecord, UNPROCESSED source when available, 48 kHz, background thread).
  - The manifest declares `RECORD_AUDIO` and `MODIFY_AUDIO_SETTINGS`; request `RECORD_AUDIO` at runtime before listening.
- `demo/` - one-screen app: text, band, Send/Stop, Listen/Stop, level bar, list of received texts.

## Tests

```
./gradlew :bysound:testDebugUnitTest
```

They check the vectors in `../test-vectors/v4-frames.json`, Reed-Solomon, a render-noise-receive loopback on every band at 44.1 and 48 kHz (short and multi-block UTF-8 text, robust profile), and decoding of the C implementation's `c/test/golden/payload_wide_44100.i16` and the recording `c/test/recordings/falso-v4.wav`.

## Demo app

Put `sdk.dir=/path/to/Android/sdk` in `android/local.properties` (it is gitignored), then:

```
./gradlew :demo:installDebug
```

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

**Length limit.** One transmission (preamble + header + body + gap) never lasts more than 30 s (`Modem.MAX_FRAME_SECONDS`). A longer text throws `TextTooLongException` (an `IllegalArgumentException`, with `maxPackedBytes`); `Modem.maxPayloadBytes(band, profile)` gives the limit in packed bytes. ASCII letters pack 1:1, digit pairs 2:1.

| band | max text, normal | max text, robust |
|---|---:|---:|
| `TOP` | 700 | 335 |
| `HIGH`, `UPPER`, `MID` | 1 052 | 520 |
| `AUDIBLE` | 1 122 | - |
| `XHIGH` | 1 408 | - |
| `WIDE` | 2 114 | - |

