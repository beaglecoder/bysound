# BySound for iOS

Swift implementation of the **bysound v4** data-over-sound modem: text is sent as near-inaudible multi-tone FSK
(15-20 kHz bands, plus an audible band) with Reed-Solomon and CRC, and received from the microphone.
It is a v4-only, payload-frames-only port of the TypeScript reference in `../web/src/modem`.

## What is inside

- `Sources/BySound` - the Swift package (iOS 15+, macOS 13+, no third-party dependencies)
  - `Protocol`, `Checksums`, `ReedSolomon`, `Codec`, `Modulator` - constants, bands, CRC-8/16/32, RS, text packing, frame and audio synthesis
  - `Receiver`, `ChannelReader`, `Listener` - streaming receiver (preamble search on all bands, header, block decoding, soft combining of repeated laps); `Listener` runs the normal and robust profiles together
  - `SoundSender`, `SoundListener` - iOS-only AVAudioEngine wrappers (loudspeaker output, microphone input)
- `Tests` - protocol vectors, RS, loopback at 44.1/48 kHz for every band, and decoding of audio made by the C implementation and a real recording
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

**Length limit.** One transmission (preamble + header + body + gap) never lasts more than 30 s (`Protocol.maxFrameSeconds`). A longer text throws `ModemError.exceedsMaxDuration(packedBytes:maxPackedBytes:)`; `Modem.maxPayloadBytes(band:profile:)` gives the limit in packed bytes. ASCII letters pack 1:1, digit pairs 2:1.

| band | max text, normal | max text, robust |
|---|---:|---:|
| `top` | 700 | 335 |
| `high`, `upper`, `mid` | 1 052 | 520 |
| `audible` | 1 122 | - |
| `xhigh` | 1 408 | - |
| `wide` | 2 114 | - |
