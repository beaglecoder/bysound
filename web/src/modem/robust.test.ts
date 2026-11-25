import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';
import { payloadFrame, referenceFrame } from './codec';
import { render } from './modulator';
import { GAP_SECONDS, PREAMBLE_KEYS_ROBUST, ROBUST_BANDS, type BandId } from './protocol';
import { ReceiverHost } from './host';
import { Receiver, type Heard } from './receiver';
import { air, feedChunks } from './testAudio';

// v4 robust profile in the web modem: the frames are those of the reference (v4-frames.json) and the receiver reads them.

const file = JSON.parse(readFileSync(new URL('../../../test-vectors/v4-frames.json', import.meta.url), 'utf8'));
const spec = JSON.parse(readFileSync(new URL('../../../protocol-v4.json', import.meta.url), 'utf8'));
const hex = (a: number[]) => a.map((b) => b.toString(16).padStart(2, '0')).join('');
const fingerprint = (t: string) => Array.from(createHash('sha256').update(t, 'utf8').digest()).slice(0, 12);
const FP = Array.from({ length: 12 }, (_, i) => (i * 29 + 7) & 255);
const FP_HEX = FP.map((b) => b.toString(16).padStart(2, '0')).join('');
const SHORT = '{"m":"Almacén","a":1250.5}';

describe('robust frames', () => {
  it('the preamble keys match the JSON', () => expect(PREAMBLE_KEYS_ROBUST).toEqual(spec.preambleKeys.robust));
  const robust = file.vectors.filter((v: any) => v.profile === 'robust');
  it('there are robust vectors', () => expect(robust.length).toBeGreaterThan(0));
  it.each(robust.map((v: any) => ({ ...v, id: `${v.name} ${v.mode} ${v.band}` })) as any[])('$id', (v: any) => {
    const f = v.mode === 'reference' ? referenceFrame(fingerprint(v.text), v.band, v.set, 'robust') : payloadFrame(v.text, v.band, 'robust');
    expect(hex(f.header)).toBe(v.header);
    expect(hex(f.body)).toBe(v.body);
    expect(f.symbols).toEqual(v.symbols);
    expect(Number(f.frameSeconds.toFixed(6))).toBe(v.frameSeconds);
  });
  it('robust on a band without robust comes out normal', () => {
    expect(payloadFrame(SHORT, 'xhigh', 'robust')).toEqual(payloadFrame(SHORT, 'xhigh'));
  });
});

describe.each([48000, 44100])('robust receiver at %d Hz', (rate) => {
  const listen = (x: Float32Array, host = true) => {
    const heard: Heard[] = [];
    const events = { onFrame: (h: Heard) => heard.push(h) };
    const r = host ? new ReceiverHost(rate, events) : new Receiver(rate, events);
    feedChunks(x, (c) => r.feed(c));
    return heard;
  };
  const robustRef = (band: BandId, set: number) => { const f = referenceFrame(FP, band, set, 'robust'); return render(f.symbols, f.band, rate, GAP_SECONDS); };
  it.each(ROBUST_BANDS as BandId[])('robust reference with noise on %s', (band) => {
    const heard = listen(air([robustRef(band, 0), robustRef(band, 1)], rate, { noise: 0.3, seed: 5 }));
    expect(heard.length).toBeGreaterThan(0);
    for (const h of heard) {
      expect(h.kind === 'reference' && h.fingerprint).toBe(FP_HEX);
      expect(h.profile).toBe('robust');
      expect(h.band).toBe(band);
    }
  });
  it('robust payload frame', () => {
    const f = payloadFrame(SHORT, 'mid', 'robust');
    const lap = render(f.symbols, f.band, rate, GAP_SECONDS);
    const first = listen(air([lap, lap], rate, { noise: 0.2 }))[0];
    expect(first?.kind === 'payload' && first.payload).toBe(SHORT);
  });
  it('the normal receiver alone does not hear it, and a normal frame does not come out as robust', () => {
    expect(listen(air([robustRef('top', 0)], rate, { noise: 0.05 }), false)).toEqual([]);
    const f = referenceFrame(FP, 'top', 0);
    const heard = listen(air([render(f.symbols, f.band, rate, GAP_SECONDS)], rate, { noise: 0.05 }));
    expect(heard.length).toBe(1);
    expect(heard[0]!.profile).toBeUndefined();
  });
});
