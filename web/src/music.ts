// A short original tune for the "music + hidden message" demo: pad chords plus a melody, all below ~3 kHz.
// Pure sample maths (no Web Audio) so the browser demo, the test and the WAV generator render the same thing.

const BPM = 110;
const BARS = 4;
const hz = (midi: number) => 440 * 2 ** ((midi - 69) / 12);

// Chord per bar: Am, F, C, G (MIDI notes, root first).
const CHORDS = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]];

// Melody as [midi, start beat, length in beats]; 4 beats per bar.
const MELODY: [number, number, number][] = [
  [76, 0, 1], [72, 1, 0.5], [74, 1.5, 0.5], [76, 2, 1], [79, 3, 1],
  [77, 4, 1], [76, 5, 0.5], [74, 6, 0.5], [72, 6.5, 0.5], [74, 7, 1],
  [76, 8, 1.5], [72, 9.5, 0.5], [67, 10, 1], [72, 11, 1],
  [74, 12, 1], [71, 13, 0.5], [67, 13.5, 0.5], [69, 14, 1], [72, 15, 1],
];

export const MELODY_SECONDS = (BARS * 4 * 60) / BPM;

/** The tune as mono Float32 (peak about 0.5). Loops seamlessly: every note rings out before the end. */
export function renderMelody(sampleRate: number): Float32Array {
  const beat = 60 / BPM;
  const out = new Float32Array(Math.round(MELODY_SECONDS * sampleRate));
  const note = (freq: number, start: number, length: number, gain: number, attack: number) => {
    const release = Math.min(0.35, length);
    const from = Math.round(start * sampleRate);
    const to = Math.min(out.length, Math.round((start + length + release) * sampleRate));
    for (let i = from; i < to; i++) {
      const t = (i - from) / sampleRate;
      const env = Math.min(1, t / attack) * (t < length ? 1 : Math.max(0, 1 - (t - length) / release)) * Math.exp(-t * 0.8);
      const ph = 2 * Math.PI * freq * t;
      out[i]! += gain * env * (Math.sin(ph) + 0.25 * Math.sin(2 * ph) + 0.08 * Math.sin(3 * ph));
    }
  };
  CHORDS.forEach((chord, bar) => {
    chord.forEach((m) => note(hz(m), bar * 4 * beat, 4 * beat - 0.3, 0.1, 0.25));
    note(hz(chord[0]! - 12), bar * 4 * beat, 4 * beat - 0.3, 0.12, 0.1);
  });
  for (const [m, start, length] of MELODY) note(hz(m), start * beat, length * beat * 0.95, 0.22, 0.02);
  let peak = 0;
  for (const v of out) peak = Math.max(peak, Math.abs(v));
  return out.map((v) => (v / peak) * 0.5);
}
