<div align="center">

<img src="docs/assets/hero.svg" alt="A phone sending data as sound to another phone and to an ESP32" width="720">

# bysound

**Send text between devices through sound. Near-ultrasonic, error-corrected, no network.**

The same library in **TypeScript · Swift · Kotlin · C**, with a small demo for the **browser, iOS, Android and ESP32**.
No accounts, no API keys, no server: everything runs locally.

[![CI](https://github.com/beaglecoder/bysound/actions/workflows/ci.yml/badge.svg)](https://github.com/beaglecoder/bysound/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-web%20%7C%20iOS%20%7C%20Android%20%7C%20ESP32-8b949e)

[How it works](docs/HOW-IT-WORKS.md) · [Paper](PAPER.md) · [Protocol spec](docs/PROTOCOL.md) · [Session layer](docs/SESSION.md) · [Security](docs/SECURITY.md) · [Benchmark vs ggwave](bench/README.md)

</div>

---

## What it does

- 📱 ➜ 🔊 ➜ 📱 One device **plays** a short burst of tones, another **listens** and gets the **exact same text**.
- 🔇 The tones sit at **15–19.5 kHz**, above what most adults hear well; the `top` channel (18–19.5 kHz) is inaudible to most adults. An audible channel is there too.
- ✅ You get the **exact message or nothing**: a checksum over the whole text guards delivery.
- 🔁 **Adapts itself:** the sender tries channels from the least audible down until the receiver acknowledges, switches to a slower, sturdier mode if nobody answers, and remembers what worked. A phone whose speaker stops around 17 kHz ends up starting on `mid`.
- 🌐 One format for a **browser, a phone and a microcontroller**: all four implementations pass the same test vectors.
- 🔌 No pairing, no Wi-Fi, no Bluetooth, no camera.

Good for: pairing codes, links, payment references, Wi-Fi setup for IoT boards, anything short you'd rather not type.

## Compared with ggwave

[ggwave](https://github.com/ggerganov/ggwave) is a widely used open-source library for data over sound. bysound starts from the same idea (tones on a 46.875 Hz grid with Reed–Solomon) and changes a few things. ggwave's built-in modes are audible (1.1–6.3 kHz) or start at 15 kHz ("ultrasound", 15–19.4 kHz, which many young people hear); none sits above 18 kHz.

<img src="docs/assets/same-5-seconds.svg" alt="In the 5 seconds ggwave needs for 140 bytes, bysound sends 284 bytes on the same frequencies, or the same 140 bytes higher up, where most adults don't hear it" width="100%">

- ⚡ **More data in the same time:** on the same frequencies, **twice as much** in 5 seconds.
- 🔇 **Or the same speed, higher up:** for messages of about 64 bytes or more, the `high` channel (17.25–19.5 kHz) matches ggwave's fastest speed.

**Above 18 kHz, like for like.** ggwave's 2-tone (DT) and 1-tone (MT) modes fit inside `top`'s 18–19.5 kHz when moved to start at 18 kHz, a custom configuration of ggwave:

<img src="docs/assets/inaudible.svg" alt="Above 18 kHz: bysound top tolerates 25 times the message's power in noise and sends 32 bytes in 2.3 s; ggwave's DT moved there tolerates 2.5 times in 3.7 s; its MT tolerates 63 to 100 times but takes 5.6 to 16.9 s and stops at 64 bytes" width="100%">

- 🔊 **Against ggwave's DT moved to 18 kHz,** `top` tolerates **10× more noise** and is faster (2.3 s against 3.7 s for 32 B).
- 🐢 **ggwave's MT tolerates more** (×63–100) but is slower, stops at 64 B, needs the receiver to know the length, and delivered wrong 8-byte messages.

| Choose bysound when… | Choose ggwave when… |
|---|---|
| 🏠 The room is noisy or echoey: on the narrow channels 2–8 dB more margin against broadband noise, and 4–12 dB more with echo (equal power, simulated) | 🔋 CPU or battery is tight: ggwave's receiver is 41–51× lighter while idle, 1.2–14× while decoding |
| ✅ A wrong message is worse than none: 0 wrong in 87,380 trials (ggwave: 34 in 67,320, mostly 8-byte) | ⏱️ Messages are a few bytes: at 8 B ggwave (1.00 s) beats every bysound channel except `wide` (0.96 s) |
| 📦 Messages go beyond 140 bytes: up to 30 s per transmission (700–2,114 B) | 🏛️ You want years of public use and a larger community |
| 🔇 It should be hard to hear: `top` (18–19.5 kHz) and `high` (17.25–19.5 kHz); ggwave has no built-in mode above 18 kHz | 🧩 You want ready-made bindings (Python, JavaScript/WebAssembly, an iOS Swift package) and Arduino/RP2040/ESP32 examples |
| 🤝 You want second-sender alerts and channel adaptation built in | 🎛️ You want 1–2-tone modes (DT/MT at 1.1–2.6 kHz): MT, if the receiver knows the length and time is no issue, tolerates the most noise measured |
| 🧰 You want native TypeScript, Swift, Kotlin and allocation-free C | 📡 You need a long real-world track record: bysound's over-the-air data is one phone pair in one room |

In-band, the two detectors are within 0–3 dB of each other: most of bysound's margin comes from narrower channels. Details and caveats: **[paper](PAPER.md#8-trade-offs)**, **[benchmark](bench/README.md)**.

## Works in noisy places

<img src="docs/assets/noise.svg" alt="How much louder than the message the noise can be, bysound vs ggwave" width="100%">

Noise measured over the whole audio band (0–24 kHz), 32-byte message, simulated:

- 🌬️ **Next to a fan or with static hiss:** still reads when the noise is **25×** stronger than the message. ggwave: 6×.
- 🏠 **In a room with echo:** **6×** (16× with the robust profile). ggwave: up to the message's own level.
- 🚗 **Street noise, air conditioning:** **100×**. ggwave: 40×.
- 🍻 **Bar, people talking:** both work. Voices live below 5 kHz and the message above 15 kHz.
- 🎶 **Over music:** the message decodes on top of a song ([try it](#music--hidden-message)). A loud nightclub has not been tested.
- 🧩 **Repairs damage:** doubtful bytes are marked and fixed, and if a noise burst ruins part of one repetition, the next one fills the gap (TypeScript, Swift and Kotlin receivers).

**Measured over the air** ([field/](field/README.md)): a Galaxy A50 and an iPad 44–65 cm apart, with recorded noise played by a laptop usually closer to the listening device than the sender:

<img src="field/charts/noise-by-scenario.svg" alt="Over-the-air reads by noise scenario" width="100%">

- 📶 **Same threshold as in simulation:** the weakest real reads were at 7.8 dB (`top`) and 10.3 dB (`mid`) of measured SNR; the simulation predicts 9.9 and 10.1 dB. Real failures came from noise too close to the microphone, not from the decoder.
- 🍻 **Bar, traffic, an audible alarm and a loud whistle at 18–19.5 kHz:** read every time. Music 23 of 30. Hair dryer 14 of 28 and vacuum 7 of 28, with the appliance closer to the microphone than the sender.
- ⏱️ **About 1.1 s** from the first tones to the decoded message (6.5 s end to end in that app, including a 3 s pre-listen and a server lookup).


## How far apart

The figures above compare the message and the noise **where they reach the microphone**. Over the air, only 44–65 cm between devices has been measured. It matters because it changes how loud each one arrives:

- 📏 **Twice as far, 4× weaker:** in open space, doubling the distance between sender and receiver makes the message 4× weaker (−6 dB), so it tolerates 4× less noise. On `top` next to a fan: ×25 at one distance, ×6 at twice that distance, ×1.5 at four times.
- 🔊 **Where the noise is matters too:** a fan twice as close to the receiver as the sender counts 4× louder. Keep the listening device away from the noise.
- 🤚 **Made for short range:** arm's length, or across a counter. At a few meters it depends on the speaker. Many phones lose a lot of volume above 17 kHz, so `top` carries less far than `mid` or `audible`.
- 🏠 **Rooms aren't open space:** walls reflect sound back, so far from the sender it fades more slowly than the rule says. The echo they add is the "room with echo" case above.

## Integrity and safety

- ✅ **Exact message or nothing:** three checks before anything is accepted (header checksum, error correction in every block, a CRC-32 over the whole text). No wrong message in 87,380 simulated trials.
- 👥 **Notices a second sender:** if two devices transmit at once, the receiver reports a *clash* and delivers nothing. Call `confirmed()` before acting and it also catches a sender that shows up later.
- ⚠ **Warns the others:** whoever notices another sender plays an alert, relayed at most twice. The suggested quarantine is only a callback: bysound never blocks anything.
- 🎚️ **Security levels:** `basic`, `standard` or `maximum` (the default).
- 🔐 **With a backend:** only a 12-byte fingerprint travels by sound; the real data, the sender's identity, the confirmation that it arrived and blocking of suspicious messages live on your server ([how](docs/SECURITY.md#using-a-backend), [example](examples/backend-reference/)).
- 📡 **Measured over the air:** no wrong message in 172 attempts with noise or in silence. With another sender playing, the real sender noticed and held back in 52 of 52 attempts. A second request registered on the same server is the hard case: with the latest build measured, all 8 attempts were delivered with a conflict warning after a median 1.8 s ([details](field/README.md)).
- 🔓 **Not secret:** anyone nearby can hear the sound, just like anyone can photograph a QR code. Don't send passwords in the clear; sign or encrypt in your app if it matters.

Session layer (acks, alerts, levels, adaptation): specified in **[docs/SESSION.md](docs/SESSION.md)** and implemented in TypeScript (the reference), Swift, Kotlin and C, which share its test vectors. What is and isn't protected: **[docs/SECURITY.md](docs/SECURITY.md)**.

## Try it in one minute

```sh
git clone https://github.com/beaglecoder/bysound && cd bysound/web
npm install && npm run dev        # open http://localhost:5173 on two devices
```

Press **Listen** on one and **Send** on the other. Nothing arrives? Turn the volume up or pick the `mid` channel: some speakers don't reach 17 kHz.

No second device? Turn text into a sound file and back:

```sh
make -C c cli
c/build/bysound encode "hello sound" -o hello.wav
c/build/bysound decode hello.wav
```

## Music + hidden message

<img src="docs/assets/music.svg" alt="Music in the low band, the message above 18 kHz" width="100%">

- 🎵 You hear a song; the message rides **above 18 kHz**.
- 👂 The other device reads the text from the music.
- ▶️ Try the **♪ Music + hidden message** button in the playground, or decode [`music-demo.wav`](docs/assets/music-demo.wav) with `c/build/bysound decode`.

## How it works

<img src="docs/assets/frame.svg" alt="Frame anatomy" width="100%">

- 🎹 Text becomes **tones on a fixed grid**; each tone carries 4 bits.
- 🚦 A **preamble** says "a message starts here" and which channel it uses.
- 🧱 The text travels in **blocks with error correction**, plus a checksum over the whole message.
- 📻 The receiver **listens to all channels at once**, so the sender can pick any of them and nothing has to be configured.
- 👂 The receiver **learns the room's noise and echo**, repairs damaged bytes and **checks** everything before answering.

<img src="docs/assets/spectrogram.svg" alt="Spectrogram of a real message" width="100%">

Step by step, with more pictures: **[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)**. Every byte: **[docs/PROTOCOL.md](docs/PROTOCOL.md)**.

## What's in the box

| | Folder | Demo |
|---|---|---|
| 🌐 Browser | [`web/`](web) · TypeScript | `npm run dev` |
| 🍎 iOS | [`ios/`](ios) · Swift | open `Demo/BySoundDemo.xcodeproj` |
| 🤖 Android | [`android/`](android) · Kotlin | `./gradlew :demo:installDebug` |
| 🔧 Any computer | [`c/`](c) · C99, no `malloc` | `c/build/bysound` (WAV in/out) |
| 📟 ESP32-S3 | [`esp32/`](esp32) · ESP-IDF | `idf.py build flash monitor` |
| 🧪 Examples | [`examples/`](examples) · all platforms | see [`examples/README.md`](examples/README.md) |
| 📊 Benchmark | [`bench/`](bench) · vs ggwave | `./fetch-ggwave.sh && make && make run` |

All of them talk to each other. Each folder's README has its API, tests and details. The ESP32-S3 demo runs the session layer (it acks what it hears) and needs an I2S microphone and an I2S amplifier; it builds (image 287 KB, about 161 KB of static RAM, no PSRAM), but has not been run on a board yet.

## Examples

Small complete apps in [`examples/`](examples), all local (no keys, no backend): [Wi-Fi setup](examples/web-wifi-setup) with its [ESP32 side](examples/esp32-wifi-setup), [store presence](examples/store-presence) (rotating check-in tokens) and the [channel ladder](examples/web-channel-ladder) in the browser; [WAV tools in C](examples/c-wav-tools); [share a link](examples/ios-share-link) on iOS and [share a contact](examples/android-share-contact) on Android.

## In your code

```ts
import { send, listen, sendSession, receiveSession } from 'bysound';
await send('hello', { band: 'top' });                       // from a click
await listen({ onFrame: (msg) => console.log(msg.payload) });

// With the session layer: ack, channel ladder, second-sender alerts
await (await sendSession('pay 12.50 to shop 7')).delivered; // resolves on the receiver's ack
const rx = await receiveSession();
const { text } = await rx.result;
await rx.confirmed();                                       // right before acting on text
```

Swift, Kotlin and C look the same: see [`ios/`](ios), [`android/`](android) and [`c/`](c).

## Citing

```bibtex
@software{bysound,
  author  = {Gandara, Agustin},
  title   = {bysound: a robust near-ultrasonic data-over-sound modem},
  version = {4.0.0},
  year    = {2026},
  url     = {https://github.com/beaglecoder/bysound},
  license = {MIT}
}
```

## License

[MIT](LICENSE). ggwave is MIT-licensed by Georgi Gerganov; the benchmark downloads it and does not redistribute it.
