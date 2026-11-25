// Audio of a frame: one tone per group, a 3 ms ramp at each symbol edge and a pause at the end. Port of Modulator.swift (ios/Sources/BySound).
import { RAMP_SECONDS, TONES, samples, symbolStart, toneHz, type Band } from './protocol';

export function render(symbols: number[][], b: Band, rate: number, gapSeconds: number): Float32Array {
  const out = new Float32Array(symbolStart(symbols.length, rate, b) + samples(gapSeconds, rate));
  const ramp = samples(RAMP_SECONDS, rate);
  // Each tone is computed once for the whole length of a symbol (same arithmetic, so the audio is identical): a frame
  // repeats the same 16 tones per group in every symbol, and without this it took millions of sines per frame.
  let longest = 0;
  for (let k = 0; k < symbols.length; k++) longest = Math.max(longest, symbolStart(k + 1, rate, b) - symbolStart(k, rate, b));
  const sines = new Map<number, Float64Array>();
  const sine = (g: number, t: number) => {
    const key = g * TONES + t;
    let s = sines.get(key);
    if (!s) {
      const omega = (2 * Math.PI * toneHz(b, g, t)) / rate;
      s = new Float64Array(longest);
      for (let n = 0; n < longest; n++) s[n] = Math.sin(omega * n);
      sines.set(key, s);
    }
    return s;
  };
  symbols.forEach((tones, k) => {
    const start = symbolStart(k, rate, b);
    const len = symbolStart(k + 1, rate, b) - start;
    const waves = tones.map((t, g) => sine(g, t));
    for (let n = 0; n < len; n++) {
      let env = 1;
      if (n < ramp) env = 0.5 * (1 - Math.cos((Math.PI * n) / ramp));
      else if (n >= len - ramp) env = 0.5 * (1 - Math.cos((Math.PI * (len - 1 - n)) / ramp));
      let s = 0;
      for (let g = 0; g < waves.length; g++) s += waves[g]![n]!;
      out[start + n] = s * env * b.amp;
    }
  });
  return out;
}

/** Scales the frame down only if it exceeds 1: clipping it (wide sums to 1.2) pollutes neighboring tones. */
export function fitLevel(audio: Float32Array, level: number): Float32Array {
  let peak = 0;
  for (let i = 0; i < audio.length; i++) peak = Math.max(peak, Math.abs(audio[i]!));
  const gain = level * (peak > 1 ? 1 / peak : 1);
  if (gain === 1) return audio;
  return audio.map((v) => v * gain);
}
