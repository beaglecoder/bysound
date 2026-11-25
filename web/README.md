# bysound (web)

TypeScript implementation of **bysound v4**: text over sound, using near-inaudible multi-tone FSK (bands around 15-19.5 kHz, plus an audible band) with Reed-Solomon and CRC. No runtime dependencies, no backend.

## Run the demo

```sh
npm install && npm run dev      # http://localhost:5173 (the microphone needs HTTPS or localhost)
npm test                        # modem tests + API tests
npm run build                   # static site in dist/, usable from any static host
```

The **Music + hidden message** button plays a short melody with the text hidden above 18 kHz; another device pressing Listen decodes it (`send(text, { backing })` does the same from code).

![Music in the low band, data tones above 18 kHz](../docs/assets/music.svg)

## API

```ts
import { send, listen, encode, decode } from './src/index';

const tx = await send('hello', { band: 'top', repeat: 'forever' }); // call from a click/tap
tx.stop();

const rx = await listen({
  onFrame: (h) => h.kind === 'payload' && console.log(h.payload, h.band),
  onLevel: (e) => console.log(e.rms),
});
rx.stop();

const samples = encode('hello', { band: 'mid', profile: 'robust', sampleRate: 48000 }); // Float32Array
const heard = decode(samples, 48000); // offline, e.g. samples read from a WAV file
```

Limit: one transmission never lasts more than 30 s (preamble + header + body + gap), so `encode`/`send` throw an `Error` with `code === 'TOO_LONG'` for a longer text. `maxPayloadBytes(band, profile)` (in `src/modem/codec`) gives the limit in packed bytes; ASCII letters pack 1:1, digit pairs 2:1.

| band | max text, normal | max text, robust |
|---|---:|---:|
| `top` | 700 | 335 |
| `high`, `upper`, `mid` | 1 052 | 520 |
| `audible` | 1 122 | - |
| `xhigh` | 1 408 | - |
| `wide` | 2 114 | - |

Bands: `top` (18.0-19.5 kHz), `high`, `upper`, `mid` (15.0-17.2 kHz), `audible`, `xhigh`, `wide`. `profile: 'robust'` is twice as slow and more noise tolerant (top, high, upper, mid only); the receiver always listens for both.

## Browser notes

- Playback and capture must start from a user gesture. The microphone needs HTTPS or `localhost`.
- Echo cancellation, noise suppression and auto gain are turned off on the microphone; they destroy the tones.
- Safari/iOS: Web Audio is stricter (gesture required for every new context, silent switch can mute playback, mic permission is asked each session). Test on the device.
- Many phone and laptop speakers or microphones cannot reproduce above ~17 kHz. If nothing is heard, use `mid` or `audible`.
- Use the device speaker at high volume; headphones and Bluetooth outputs change the audio path.
- Sample rate follows the device (44.1 or 48 kHz); the modem supports both.

`src/modem/` is the normative modem (protocol, codec, Reed-Solomon, modulator, receiver); `src/audio.ts` is the only code that touches Web Audio.
