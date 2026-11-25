import { describe, expect, it } from 'vitest';
import { payloadFrame } from './codec';
import { render } from './modulator';
import { GAP_SECONDS, type BandId, type Profile } from './protocol';
import { Receiver, type Heard } from './receiver';
import { air, feedChunks } from './testAudio';

// Receiver bugs found by the benchmark in bench/ (see bench/README.md).

/** First text of the series whose last block ends in a parity byte 0x00 (about 1 in 256). */
function zeroParityText(prefix: string): string {
  for (let i = 0; ; i++) {
    const text = `${prefix} ${i}`;
    const body = payloadFrame(text, 'top').body;
    if (body[body.length - 1] === 0) return text;
  }
}

function heard(text: string, band: BandId, profile: Profile, rate: number, laps = 1): Heard[] {
  const f = payloadFrame(text, band, profile);
  const lap = render(f.symbols, f.band, rate, GAP_SECONDS);
  const out: Heard[] = [];
  const r = new Receiver(rate, { onFrame: (h) => out.push(h) }, profile);
  feedChunks(air(Array(laps).fill(lap), rate, { noise: 0 }), (c) => r.feed(c));
  return out;
}

describe('last block whose last parity byte is 0x00 (it used to close one byte short)', () => {
  const short = zeroParityText('zero parity');
  const long = zeroParityText('a payload long enough to need two blocks of forty-eight bytes, zero parity');
  it('the texts are what the test needs', () => {
    expect(payloadFrame(short, 'top').body.length).toBeLessThan(64);
    expect(payloadFrame(long, 'top').body.length).toBeGreaterThan(64);
  });
  it.each([
    ['one block', 'normal'], ['one block', 'robust'], ['two blocks', 'normal'], ['two blocks', 'robust'],
  ] as const)('%s, %s', (size, profile) => {
    const text = size === 'one block' ? short : long;
    const first = heard(text, 'top', profile, 48000)[0];
    expect(first?.kind === 'payload' && first.payload).toBe(text);
  });
});

describe('a body that looks like the preamble does not cut off the read (it used to fail only at high SNR)', () => {
  // On top/robust, body symbols 50..57 of this payload match 6 of the preamble's 16 nibbles: 0.375 in a clean channel.
  const text = 'q(qs@TgOXJfdn<NXK<f,h?<StQ*yJq)b';
  it.each([48000, 44100])('top, robust, clean, %d Hz', (rate) => {
    const out = heard(text, 'top', 'robust', rate);
    expect(out.length).toBe(1);
    expect(out[0]!.kind === 'payload' && out[0]!.payload).toBe(text);
  });
  it('two repetitions are still both read', () => {
    const out = heard(text, 'top', 'robust', 48000, 2);
    expect(out.map((h) => h.kind === 'payload' && h.payload)).toEqual([text, text]);
  });
});
