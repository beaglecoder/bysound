# web-channel-ladder

Visualizes the sender's automatic adaptation (docs/SESSION.md section 9).

```sh
cd examples && npm run dev:web-channel-ladder
```

1. **Simulation (no microphone):** replays the ladder `top → high → upper → mid` (3 repetitions per channel, robust after 6 normal repetitions without an ack). Each repetition is `encode`d, optionally passed through a **broken tweeter** (4 cascaded low-pass sections, adjustable cutoff) plus microphone noise, and read with offline `decode`. With the defaults (14 kHz cutoff, -35 dBFS noise) the high channels fail and the ladder converges on `mid` (robust). It is a model of the sender's schedule (`ladder.ts` mirrors `session/sender.ts` using the exported `channelOrder`, `HOP_REPETITIONS`, `ROBUST_AFTER_REPETITIONS`), not a measurement of a real speaker.
2. **Live:** `sendSession` with `band` unset (auto) and a `receiveSession` on a second device. The log lists each repetition's band (`R` = robust) from `onRepetition`, the route from `onAmbient`, and where it was read from `Delivered`. **Forget calibration** clears the `localStorage` entry so the next send starts at the top again.
