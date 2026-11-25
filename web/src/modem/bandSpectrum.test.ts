import { describe, expect, it } from 'vitest';
import { payloadFrame } from './codec';
import { render } from './modulator';
import { BANDS, GAP_SECONDS, SPACING_HZ, TONES, type BandId } from './protocol';

// Measures what the web modem actually emits: the spectrum of one frame per band must fall on the frequencies in docs/PROTOCOL.md
// (and not on audible ones). It checks that audible distortion comes from the speaker and not from the synthesis.

const RATE = 48000;
const FFT = 4096;

function fft(re: Float64Array, im: Float64Array) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j]!, re[i]!]; [im[i], im[j]] = [im[j]!, im[i]!]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    for (let i = 0; i < n; i += len) {
      for (let k = 0; k < len / 2; k++) {
        const wr = Math.cos(ang * k), wi = Math.sin(ang * k);
        const a = i + k, b = i + k + len / 2;
        const tr = re[b]! * wr - im[b]! * wi, ti = re[b]! * wi + im[b]! * wr;
        re[b] = re[a]! - tr; im[b] = im[a]! - ti;
        re[a] = re[a]! + tr; im[a] = im[a]! + ti;
      }
    }
  }
}

/** Energy per frequency range, accumulated over the whole frame. */
function energy(audio: Float32Array): (loHz: number, hiHz: number) => number {
  const power = new Float64Array(FFT / 2);
  for (let at = 0; at + FFT <= audio.length; at += FFT / 2) {
    const re = new Float64Array(FFT), im = new Float64Array(FFT);
    for (let i = 0; i < FFT; i++) re[i] = audio[at + i]! * (0.5 - 0.5 * Math.cos((2 * Math.PI * i) / FFT));
    fft(re, im);
    for (let k = 0; k < FFT / 2; k++) power[k]! += re[k]! ** 2 + im[k]! ** 2;
  }
  const hz = RATE / FFT;
  return (lo, hi) => {
    let sum = 0;
    for (let k = Math.max(0, Math.floor(lo / hz)); k <= Math.min(FFT / 2 - 1, Math.ceil(hi / hz)); k++) sum += power[k]!;
    return sum;
  };
}

describe('web modem: the spectrum falls in the right band', () => {
  for (const id of ['top', 'high', 'upper', 'mid', 'audible', 'xhigh', 'wide'] as BandId[]) {
    it(`${id}: almost all the energy is in its tones`, () => {
      const b = BANDS[id];
      const frame = payloadFrame('SBL2|CH|abcdefghijklmnopqrstuv|abcd1234 — prueba de espectro', id);
      const e = energy(render(frame.symbols, b, RATE, GAP_SECONDS));
      const lo = b.bin * SPACING_HZ - 150;
      const hi = (b.bin + b.groups * TONES) * SPACING_HZ + 150;
      const total = e(0, RATE / 2);
      expect(e(lo, hi) / total).toBeGreaterThan(0.97);
      if (id !== 'audible') {
        // What is audible (below 12 kHz) must be practically nothing: only the symbol edges.
        expect(e(0, 12000) / total).toBeLessThan(0.002);
      } else {
        expect(lo).toBeGreaterThan(1500);
        expect(hi).toBeLessThan(5200);
      }
    });
  }

  it('the high bands start where docs/PROTOCOL.md says', () => {
    const khz = (id: BandId) => (BANDS[id].bin * SPACING_HZ) / 1000;
    expect(khz('top')).toBeCloseTo(18, 1);
    expect(khz('high')).toBeCloseTo(17.25, 1);
    expect(khz('upper')).toBeCloseTo(16.5, 1);
    expect(khz('mid')).toBeCloseTo(15, 1);
    expect(khz('xhigh')).toBeCloseTo(16.5, 1);
    expect(khz('wide')).toBeCloseTo(15, 1);
    expect(khz('audible')).toBeCloseTo(1.875, 2);
  });
});
