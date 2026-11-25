import { describe, expect, it } from 'vitest';
import { referenceFrame } from './codec';
import { render } from './modulator';
import { BANDS, GAP_SECONDS } from './protocol';
import { Receiver, type Heard } from './receiver';
import { symbolWindow } from './symbolWindow';
import { feedChunks } from './testAudio';

const FP = Array.from({ length: 12 }, (_, i) => (i * 29 + 7) & 255);

describe('symbol window', () => {
  it('smooths the edges and keeps the center flat', () => {
    const w = symbolWindow(1024);
    expect(w[0]).toBe(0);
    expect(w[1023]).toBe(0);
    expect(w[25]).toBeCloseTo(0.4823, 3);
    expect(w[52]).toBe(1);
    expect(w[512]).toBe(1);
    expect(w[100]).toBe(w[923]);
  });

  it('without smoothing it is rectangular', () => {
    expect(symbolWindow(1024, 0).every((v) => v === 1)).toBe(true);
  });

  it('is read with a sweep 38 dB above the signal', () => {
    const rate = 48000;
    const frame = render(referenceFrame(FP, 'top', 0).symbols, BANDS.top, rate, GAP_SECONDS);
    const lead = 24000;
    const repetitions = 5;
    const mix = new Float32Array(lead + frame.length * repetitions);
    const sweep = 24000;
    let phase = 0;
    for (let i = 0; i < mix.length; i++) {
      phase += (2 * Math.PI * (15000 + (5000 * (i % sweep)) / sweep)) / rate;
      mix[i] = Math.sin(phase) * 0.316;
    }
    const gain = 10 ** (-48 / 20);
    for (let r = 0; r < repetitions; r++) {
      for (let i = 0; i < frame.length; i++) mix[lead + r * frame.length + i]! += frame[i]! * gain;
    }
    const heard: Heard[] = [];
    const receiver = new Receiver(rate, { onFrame: (h) => heard.push(h) });
    feedChunks(mix, (c) => receiver.feed(c));
    expect(heard.length).toBeGreaterThan(0);
  });
});
