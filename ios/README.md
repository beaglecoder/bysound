# BySound for iOS

Swift implementation of the **bysound v4** data-over-sound modem: text is sent as near-inaudible multi-tone FSK
(15-20 kHz bands, plus an audible band) with Reed-Solomon and CRC, and received from the microphone.
It is a v4-only port of the TypeScript reference in `../web/src/modem` (payload and reference frames) and `../web/src/session` (the session layer, [docs/SESSION.md](../docs/SESSION.md)).

## What is inside

- `Sources/BySound` - the Swift package (iOS 15+, macOS 13+, no third-party dependencies)
  - `Protocol`, `Checksums`, `ReedSolomon`, `Codec`, `Modulator` - constants, bands, CRC-8/16/32, RS, text packing, frame and audio synthesis
  - `Receiver`, `ChannelReader`, `Listener` - streaming receiver (preamble search on all bands, header, block decoding, soft combining of repeated laps); `Listener` runs the normal and robust profiles together
  - `SoundSender`, `SoundListener` - iOS-only AVAudioEngine wrappers (loudspeaker output, microphone input)
  - `Session/` - the session layer, independent of AVFoundation and testable on macOS: `Frames` (ack / receipt / risk frames), `ControlListener` (reads them), `Security`, `Adapt` (channel ladder, ambient check, robust ladder, calibration stores), `SenderSession` and `ReceiverSession` (state machines over a `SessionPlatform` and `AirEvent`s), `Ids` (SHA-256 identifiers)
  - `BySoundSession` - iOS-only async API that wires the state machines to the speaker and the microphone
- `Tests` - protocol vectors, RS, loopback at 44.1/48 kHz for every band, decoding of audio made by the C implementation and a real recording, the ack / risk / reference / security-level vectors, and the session scenarios of `web/src/session` on simulated air (`SessionAir.swift`)
- `Demo` - a minimal SwiftUI app

## Tests

```sh
cd ios
swift test -c release   # release matters: the receiver is slow in debug builds
```

## Demo app

Open `Demo/BySoundDemo.xcodeproj` in Xcode, pick your **signing team** in Signing & Capabilities
(it is left empty on purpose) and run on a device: the simulator has no useful microphone/speaker path.
Build check without signing:

```sh
xcodebuild -project Demo/BySoundDemo.xcodeproj -scheme BySoundDemo \
  -destination 'generic/platform=iOS Simulator' build CODE_SIGNING_ALLOWED=NO
```

## API example

```swift
import BySound

// Send: play "Hello" on the top band until stopped (loudspeaker, level 1.0).
let sender = SoundSender()
try sender.start(text: "Hello", band: .top)
// ... sender.stop()

// Receive: ask for the microphone, then listen.
let listener = SoundListener()
listener.onFrame = { heard in print(heard.text, heard.band, heard.correctedBytes) }
SoundListener.requestPermission { ok in if ok { try? listener.start() } }

// Without audio hardware: render and decode samples yourself.
let samples = try Modem.encode(text: "Hello", band: .top, sampleRate: 48000)
let rx = Receiver(sampleRate: 48000) { print($0.text) }
rx.feed(samples + samples)
```

Set the device volume to maximum when sending; the sender always uses the loudspeaker.

### Sessions (acknowledgement, other emitters, confirm gate)

`BySoundSession` turns the one-way modem into a two-party exchange. The sender picks a channel by the noise it measures, steps down the ladder until
the receiver acknowledges by sound, and remembers where it was read (`UserDefaults`); the receiver delivers one text, acks it, and keeps watching
until you call `confirmed()`. A second sender nearby is detected, nothing is delivered from the clash and a risk frame stops the others.
`securityLevel` is `.basic`, `.standard` or `.maximum` (default); quarantine is only a callback. Needs the microphone.

```swift
// Send: returns at the receiver's ack (the Task can be cancelled to stop). Or `BySoundSession.startSending` for a handle with `stop()`.
do {
    let d = try await BySoundSession.send(text: "pay 12.50 to shop 7")
    print("Received on \(d.band) by the other device")
} catch let e as SessionError {
    print(e.code)               // .otherEmitter, .riskSignal, .timeout, ...
}

// Receive: the data arrives after a final listen; confirm before acting on it.
var options = ReceiverOptions()
options.onQuarantine = { print("pause \($0.durationMs) ms") }        // a suggestion, never enforced
let reception = try await BySoundSession.receive(options: options)   // throws .multipleTransmissions on two senders at once
let gate = try await reception.confirmed()                           // throws if another emitter appeared meanwhile
print(reception.text, gate.watch)                                    // .clean, .expired or .off
// reception.cancel() if the person leaves without confirming.

// Reference mode: only the 12-byte fingerprint travels; the receiver resolves it with texts both devices already have.
var rx = ReceiverOptions()
rx.resolve(from: [Ids.fingerprint("order 7"): "order 7"])
var tx = SenderOptions()
tx.mode = .reference
```

The state machines (`SenderSession`, `ReceiverSession`) take a `SessionPlatform` (clock, timers, playback) and `AirEvent`s from `Ears`, so they run
on any audio stack and in tests without hardware. Options: `securityLevel`, `security` (field-by-field overrides), `band` / `channels` / `profile`,
`calibration` (`.automatic`, `.disabled` or your own `CalibrationStore`), `memory`, `onAlert`, `onQuarantine`.

**Length limit.** One transmission (preamble + header + body + gap) never lasts more than 30 s (`Protocol.maxFrameSeconds`). A longer text throws `ModemError.exceedsMaxDuration(packedBytes:maxPackedBytes:)`; `Modem.maxPayloadBytes(band:profile:)` gives the limit in packed bytes. ASCII letters pack 1:1, digit pairs 2:1.

| band | max text, normal | max text, robust |
|---|---:|---:|
| `top` | 700 | 335 |
| `high`, `upper`, `mid` | 1 052 | 520 |
| `audible` | 1 122 | - |
| `xhigh` | 1 408 | - |
| `wide` | 2 114 | - |
