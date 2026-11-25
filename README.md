<div align="center">

<img src="docs/assets/hero.svg" alt="A phone sending data as sound to another phone and to an ESP32" width="720">

# bysound

**Send text between devices through sound. Near-ultrasonic, error-corrected, no network.**

The same library in **TypeScript · Swift · Kotlin · C**, with a tiny demo for the **browser, iOS, Android and ESP32**.
No accounts, no API keys, no server: everything runs locally.

[![CI](https://github.com/beaglecoder/bysound/actions/workflows/ci.yml/badge.svg)](https://github.com/beaglecoder/bysound/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-web%20%7C%20iOS%20%7C%20Android%20%7C%20ESP32-8b949e)

[How it works](docs/HOW-IT-WORKS.md) · [Paper](PAPER.md) · [Protocol spec](docs/PROTOCOL.md) · [Benchmark vs ggwave](bench/README.md)

</div>

---

## What it does

- 📱 ➜ 🔊 ➜ 📱 One device **plays** a short burst of tones, another **listens** and gets the **exact same text**.
- 🔇 The tones sit at **15–19.5 kHz**: most people don't hear them. An audible channel is there too.
- ✅ You get the **exact message or nothing**. It never hands you a wrong one.
- 🌐 Works between a **browser, a phone and a $5 microcontroller**, in any direction.
- 🔌 No pairing, no Wi-Fi, no Bluetooth, no camera.

Good for: pairing codes, links, payment references, Wi-Fi setup for IoT boards, anything short you'd rather not type.

## Better than ggwave, in one picture

[ggwave](https://github.com/ggerganov/ggwave) is the best-known open-source library for data over sound. bysound starts from the same idea and improves it:

<img src="docs/assets/same-5-seconds.svg" alt="In the 5 seconds ggwave needs for 140 bytes, bysound sends 284 bytes on the same frequencies, or the same 140 bytes where almost nobody hears it" width="100%">

- ⚡ **More data in the same time:** on the same frequencies, **twice as much** in 5 seconds.
- 🔇 **Or the same speed, unheard:** the `high` channel matches ggwave's speed at 17–19.5 kHz, where almost nobody hears it.
- 🏠 **Better with noise and echo:** it reads messages in **8–12 dB more noise** when the room echoes.
- 🧩 **Rebuilds damaged data:** doubtful bytes are marked and repaired, and repetitions add up.
- ✅ **Never lies:** **0 wrong messages** in 88,220 tests. ggwave delivered 349.
- 👥 **Notices a second sender** talking at the same time instead of mixing them up.

ggwave still wins on CPU (14–42× lighter) and maturity. Everything was measured on the same simulated channel; the details, charts and caveats are in the **[paper](PAPER.md)** and **[how it works](docs/HOW-IT-WORKS.md#6-bysound-vs-ggwave-feature-by-feature)**.

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
| 📊 Benchmark | [`bench/`](bench) · vs ggwave | `./fetch-ggwave.sh && make && make run` |

All of them talk to each other. Each folder's README has its API, tests and details.

## In your code

```ts
import { send, listen } from 'bysound';
await send('hello', { band: 'top' });                       // from a click
await listen({ onFrame: (msg) => console.log(msg.payload) });
```

Swift, Kotlin and C look the same: see [`ios/`](ios), [`android/`](android) and [`c/`](c).

## Citing

```bibtex
@software{bysound,
  author  = {Gandara, Agustin},
  title   = {bysound: a robust near-ultrasonic data-over-sound modem},
  year    = {2026},
  url     = {https://github.com/beaglecoder/bysound},
  license = {MIT}
}
```

## License

[MIT](LICENSE). ggwave is MIT-licensed by Georgi Gerganov; the benchmark downloads it and does not redistribute it.
