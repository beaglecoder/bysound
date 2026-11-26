import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { BAND_IDS, decode, encode } from './index';

const TEXT = '{"m":"Almacén","a":1250.5} ñandú 🙂';

describe('encode / decode', () => {
  for (const band of BAND_IDS) {
    it(`round-trips on ${band}`, () => {
      const heard = decode(encode(TEXT, { band, sampleRate: 48000 }), 48000);
      expect(heard).toHaveLength(1);
      expect(heard[0]).toMatchObject({ kind: 'payload', payload: TEXT, band });
    });
  }

  it('round-trips the robust profile at 44.1 kHz', () => {
    const heard = decode(encode(TEXT, { band: 'mid', profile: 'robust', sampleRate: 44100 }), 44100);
    expect(heard[0]).toMatchObject({ kind: 'payload', payload: TEXT, band: 'mid', profile: 'robust' });
  });

  it('decodes a looped frame once per lap', () => {
    const one = encode('one', { band: 'top' });
    const two = encode('one', { band: 'top' });
    const gap = new Float32Array(48000 * 2);
    const x = new Float32Array(one.length + gap.length + two.length);
    x.set(one);
    x.set(two, one.length + gap.length);
    expect(decode(x, 48000).map((h) => h.kind === 'payload' && h.payload)).toEqual(['one', 'one']);
  });

  it('applies level', () => {
    const peak = (x: Float32Array) => x.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
    expect(peak(encode('hi', { level: 0.5 }))).toBeCloseTo(peak(encode('hi', { level: 1 })) * 0.5, 5);
  });

  it('decodes a real 48 kHz WAV recording', () => {
    const wav = readFileSync(new URL('../../c/test/recordings/payload-top-v4.wav', import.meta.url));
    const pcm = new Int16Array(wav.buffer.slice(wav.byteOffset + 44, wav.byteOffset + wav.byteLength - ((wav.byteLength - 44) % 2)));
    const samples = Float32Array.from(pcm, (v) => v / 32768);
    const heard = decode(samples, 48000);
    const first = heard.find((h) => h.kind === 'payload');
    expect(first && first.kind === 'payload' && first.payload.startsWith('000201010212264700')).toBe(true);
  });
});
