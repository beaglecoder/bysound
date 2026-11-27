import { describe, expect, it } from 'vitest';
import { payloadFrame, referenceFrame } from './codec';
import { render } from './modulator';
import { BANDS, BAND_IDS, GAP_SECONDS, type BandId } from './protocol';
import { Receiver, type Heard } from './receiver';
import { air, feedChunks, gaussian, type Air } from './testAudio';

const SHORT = '{"m":"Almacén","a":1250.5}';
const LONG = '00020101021226470017com.example.pagos01220000003100099988877766520454115303032540812500.505802AR5918ALMACEN LA ESQUINA6007CORDOBA62250108F-0001230509PEDIDO-776304F4AF ñandú 🙂';
const FP = Array.from({ length: 12 }, (_, i) => (i * 29 + 7) & 255);
const FP_HEX = FP.map((b) => b.toString(16).padStart(2, '0')).join('');

function listen(x: Float32Array, rate: number) {
  const heard: Heard[] = [];
  const blocks: [number, number][] = [];
  const headers: BandId[] = [];
  let clashes = 0;
  const r = new Receiver(rate, { onFrame: (h) => heard.push(h), onBlocks: (c, t) => blocks.push([c, t]), onHeader: (b) => headers.push(b), onClash: () => { clashes++; } });
  feedChunks(x, (c) => r.feed(c));
  return { heard, blocks, headers, clashes, failures: r.failures };
}
const payloadAudio = (text: string, band: BandId, rate: number) => render(payloadFrame(text, band).symbols, BANDS[band], rate, GAP_SECONDS);
const refAudio = (band: BandId, rate: number, set: number) => render(referenceFrame(FP, band, set).symbols, BANDS[band], rate, GAP_SECONDS);
const MIC: Air = { noise: 0.15, gain: 0.2, echo: 0.3, seed: 7 };

describe.each([48000, 44100])('round trip at %d Hz', (rate) => {
  it.each(BAND_IDS)('short payload with noise on %s', (band) => {
    const lap = payloadAudio(SHORT, band, rate);
    const out = listen(air([lap, lap], rate, { noise: 0.3 }), rate);
    const first = out.heard[0];
    expect(first?.kind === 'payload' && first.payload).toBe(SHORT);
    expect(first!.band).toBe(band);
    expect(out.clashes).toBe(0);
    expect(out.failures).toBe(0);
    expect(out.headers.every((b) => b === band)).toBe(true);
  });
  it.each(BAND_IDS)('long multi-block payload on %s', (band) => {
    const lap = payloadAudio(LONG, band, rate);
    const out = listen(air([lap, lap], rate, { noise: 0.15, seed: 3 }), rate);
    const first = out.heard[0];
    expect(first?.kind === 'payload' && first.payload).toBe(LONG);
    expect(out.blocks[out.blocks.length - 1]).toEqual([3, 3]);
    expect(out.clashes).toBe(0);
  });
  it.each(BAND_IDS)('reference, sets A and B, on %s', (band) => {
    const out = listen(air([refAudio(band, rate, 0), refAudio(band, rate, 1)], rate, { noise: 0.3, seed: 5 }), rate);
    expect(out.heard.length).toBe(2);
    for (const h of out.heard) expect(h.kind === 'reference' && h.fingerprint).toBe(FP_HEX);
    expect(out.clashes).toBe(0);
    expect(out.failures).toBe(0);
  });
  it.each(BAND_IDS)('weak microphone, with echo and noise, on %s', (band) => {
    const lap = payloadAudio(SHORT, band, rate);
    const first = listen(air([lap, lap, lap], rate, MIC), rate).heard[0];
    expect(first?.kind === 'payload' && first.payload).toBe(SHORT);
  });
});

describe('repetitions', () => {
  const rate = 48000;
  /** Covers the symbols [from, to) of a repetition with a burst of noise. */
  const hit = (lap: Float32Array, band: BandId, from: number, to: number) => {
    const out = lap.slice();
    const n = gaussian(from + to + 1);
    const sym = BANDS[band].sym * rate;
    for (let i = Math.round(from * sym); i < Math.round(to * sym); i++) out[i]! += n() * 3;
    return out;
  };
  it('a one-block payload hit in each repetition closes by combining repetitions', () => {
    const lap = payloadAudio(SHORT, 'upper', rate);
    const body = payloadFrame(SHORT, 'upper').symbols.length;
    const mid = Math.round((11 + body) / 2);
    const out = listen(air([hit(lap, 'upper', 11, mid), hit(lap, 'upper', mid, body)], rate, { noise: 0.05, seed: 11 }), rate);
    const read = out.heard.find((h) => h.kind === 'payload' && h.payload === SHORT);
    expect(read?.combined).toBeGreaterThan(1);
  });
  it('a long payload with strong noise gathers blocks from several repetitions', () => {
    const lap = payloadAudio(LONG, 'upper', rate);
    const out = listen(air([lap, lap, lap, lap], rate, { noise: 0.8, seed: 13 }), rate);
    expect(out.heard.some((h) => h.kind === 'payload' && h.payload === LONG)).toBe(true);
  });
  it('a reference that hops bands and starts on A and B', () => {
    const laps = [refAudio('top', rate, 0), refAudio('top', rate, 1), refAudio('top', rate, 0), refAudio('high', rate, 0)];
    const out = listen(air(laps, rate, { noise: 0.2 }), rate);
    expect(out.heard.map((h) => h.band)).toEqual(['top', 'top', 'top', 'high']);
    expect(out.clashes).toBe(0);
  });
  it('a reference broken in one set is repaired with the other', () => {
    const laps = [hit(refAudio('top', rate, 0), 'top', 24, 70), hit(refAudio('top', rate, 1), 'top', 10, 34)];
    const out = listen(air(laps, rate, { noise: 0.05, seed: 2 }), rate);
    const read = out.heard.find((h) => h.kind === 'reference' && h.fingerprint === FP_HEX);
    expect(read?.combined).toBeGreaterThan(1);
  });
  it('two overlapping senders on different bands are a clash', () => {
    const a = payloadAudio(LONG, 'top', rate), b = payloadAudio(LONG, 'audible', rate);
    const mix = new Float32Array(a.length + rate);
    mix.set(a, 0);
    const off = Math.round(0.6 * rate);
    for (let i = 0; i < Math.min(b.length, mix.length - off); i++) mix[i + off]! += b[i]!;
    expect(listen(air([mix], rate, { noise: 0.05 }), rate).clashes).toBeGreaterThan(0);
  });
  it('with no signal it hears nothing and counts no failures', () => {
    const out = listen(air([new Float32Array(rate * 4)], rate, { noise: 0.3 }), rate);
    expect(out.heard).toEqual([]);
    expect(out.failures).toBe(0);
    expect(out.headers).toEqual([]);
  });
});

describe('two senders on the same band', () => {
  it('a preamble on top of the body is a clash', () => {
    const rate = 48000;
    const a = payloadAudio(LONG, 'upper', rate), b = payloadAudio(SHORT + ' other', 'upper', rate);
    const mix = a.slice();
    const off = Math.round(1 * rate);
    for (let i = 0; i < b.length; i++) mix[i + off]! += b[i]!;
    expect(listen(air([mix], rate, { noise: 0.05 }), rate).clashes).toBeGreaterThan(0);
  });
});
