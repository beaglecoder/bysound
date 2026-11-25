import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { decode } from './index';
import { renderMusicDemo } from './music-demo';

const wav = (name: string) => {
  const b = readFileSync(new URL(`../../docs/assets/${name}`, import.meta.url));
  const pcm = new Int16Array(b.buffer.slice(b.byteOffset + 44, b.byteOffset + b.byteLength));
  return Float32Array.from(pcm, (v) => v / 32768);
};

describe('music + hidden message', () => {
  it('decodes the message from the published WAV', () => {
    const heard = decode(wav('music-demo.wav'), 48000);
    expect(heard.length).toBeGreaterThan(0);
    for (const h of heard) expect(h).toMatchObject({ kind: 'payload', payload: 'hello from the music', band: 'top' });
  });

  it('decodes a freshly rendered mix, at 44.1 kHz too', () => {
    for (const rate of [48000, 44100]) expect(decode(renderMusicDemo('hello from the music', rate), rate)[0]).toMatchObject({ payload: 'hello from the music' });
  });
});
