// Offline model of the sender's automatic ladder (docs/SESSION.md section 9, web/src/session/sender.ts) plus a "broken tweeter".
import { DEFAULT_CHANNELS, HOP_REPETITIONS, ROBUST_AFTER_REPETITIONS, channelOrder, decode, encode, hasRobust, type BandId, type Profile } from '../../web/src/index';

export interface Step { index: number; band: BandId; profile: Profile }

/**
 * The repetitions the sender plays with 'auto' and no calibration: HOP_REPETITIONS on each channel of the ladder (top, high, upper, mid),
 * cycling; after ROBUST_AFTER_REPETITIONS normal repetitions without an ack the rest are robust (on bands that have it).
 */
export function* ladder(start: BandId = DEFAULT_CHANNELS[0]!, every = HOP_REPETITIONS): Generator<Step> {
  const route = channelOrder(start, DEFAULT_CHANNELS);
  let hop = 0, onChannel = 0, normal = 0, robust = false;
  for (let index = 0; ; index++) {
    if (onChannel >= every) { onChannel = 0; hop = (hop + 1) % route.length; }
    onChannel++;
    const band = route[hop]!;
    if (!robust && normal >= ROBUST_AFTER_REPETITIONS) robust = true;
    const profile: Profile = robust && hasRobust(band) ? 'robust' : 'normal';
    if (profile === 'normal') normal++;
    yield { index, band, profile };
  }
}

/** One second-order Butterworth-style low-pass section (RBJ cookbook), run in place. */
function biquadLowPass(x: Float32Array, rate: number, cutoff: number, q: number) {
  const w = (2 * Math.PI * cutoff) / rate, alpha = Math.sin(w) / (2 * q), c = Math.cos(w);
  const b0 = (1 - c) / 2, b1 = 1 - c, b2 = (1 - c) / 2, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha;
  let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (let i = 0; i < x.length; i++) {
    const y = (b0 * x[i]! + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
    x2 = x1; x1 = x[i]!; y2 = y1; y1 = y; x[i] = y;
  }
}

/** A speaker whose tweeter died: everything above `cutoff` Hz fades out. 4 cascaded sections = 48 dB/octave, like a worn-out driver. */
export function brokenTweeter(audio: Float32Array, rate: number, cutoff: number): Float32Array {
  const out = Float32Array.from(audio);
  for (const q of [0.5412, 1.3065, 0.5412, 1.3065]) biquadLowPass(out, rate, cutoff, q);
  return out;
}

/** Deterministic microphone noise (white, Gaussian-ish), `db` dBFS rms. Without it a sharp filter cannot hide anything from the receiver. */
export function addNoise(audio: Float32Array, db: number, seed = 1): void {
  let s = seed >>> 0;
  const rnd = () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 4294967296; };
  const amp = 10 ** (db / 20);
  for (let i = 0; i < audio.length; i++) audio[i]! += amp * (rnd() + rnd() + rnd() + rnd() - 2) * Math.sqrt(3);
}

export interface Outcome { step: Step; heard: BandId | null; profile: Profile | null }

/** Plays one repetition into the (simulated) air and tries to read it offline. */
export function tryRepetition(text: string, step: Step, rate: number, cutoff: number | null, noiseDb = -50): Outcome {
  let audio = encode(text, { band: step.band, profile: step.profile, sampleRate: rate });
  if (cutoff !== null) audio = brokenTweeter(audio, rate, cutoff);
  addNoise(audio, noiseDb);
  const h = decode(audio, rate).find((x) => x.kind === 'payload' && x.payload === text);
  return { step, heard: h ? h.band : null, profile: h ? (h.profile ?? 'normal') : null };
}
