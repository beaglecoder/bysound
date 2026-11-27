// Input limits of the session layer and the modem API: empty text, texts too long for one transmission or for the robust profile.
import { describe, expect, it } from 'vitest';
import { decode, encode } from '../audio';
import { TooLongError, maxPayloadBytes } from '../modem/codec';
import type { Profile } from '../modem/protocol';
import { longestRepetitionSeconds } from './adapt';
import { RiskMemory } from './common';
import type { Platform } from './ears';
import { ackId } from './frames';
import { SenderSession, type SenderOptions } from './sender';

/** A clock and a speaker with no audio processing: timers and the end of each sound run in time order. */
function fakePlatform(rate = 48000) {
  let t = 0;
  const queue: { due: number; fn: () => void; live: boolean }[] = [];
  const at = (seconds: number, fn: () => void) => { const e = { due: t + seconds, fn, live: true }; queue.push(e); return () => { e.live = false; }; };
  const platform: Platform = {
    sampleRate: rate,
    now: () => t,
    after: at,
    play: (audio, onEnd) => at(audio.length / rate, onEnd),
    random: () => 0.5,
  };
  const run = (until: () => boolean, limit = 1000) => {
    for (let i = 0; i < limit && !until(); i++) {
      queue.sort((a, b) => a.due - b.due);
      const e = queue.shift();
      if (!e) return;
      t = e.due;
      if (e.live) e.fn();
    }
  };
  return { platform, run };
}

const quiet: SenderOptions = { securityLevel: 'basic', security: { listenBeforeSendingSeconds: 0 }, ambient: false, calibration: null, memory: new RiskMemory(), timeoutSeconds: 1000 };

describe('input limits', () => {
  it('encode and decode reject an empty text, a level outside 0..1 and a bad sample rate', () => {
    expect(() => encode('')).toThrow(RangeError);
    expect(() => encode('hi', { level: Number.NaN })).toThrow(RangeError);
    expect(() => encode('hi', { level: 2 })).toThrow(RangeError);
    expect(() => encode('hi', { level: 0 })).not.toThrow();
    expect(() => encode('hi', { sampleRate: Number.NaN })).toThrow(RangeError);
    expect(() => decode(new Float32Array(10), 0)).toThrow(RangeError);
  });

  it('a session refuses an empty text and a text too long for its bands, before playing anything', () => {
    const { platform } = fakePlatform();
    expect(() => new SenderSession('', platform, quiet)).toThrow(RangeError);
    const long = 'a'.repeat(maxPayloadBytes('top') + 1);
    expect(() => new SenderSession(long, platform, quiet)).toThrow(TooLongError);
    expect(() => new SenderSession(long, platform, { ...quiet, channels: ['mid', 'top'] })).toThrow(TooLongError);
    expect(() => new SenderSession(long, platform, { ...quiet, band: 'mid' })).not.toThrow();
    const robustTooLong = 'a'.repeat(maxPayloadBytes('top', 'robust') + 1);
    expect(() => new SenderSession(robustTooLong, platform, { ...quiet, band: 'top', profile: 'robust' })).toThrow(TooLongError);
    expect(() => new SenderSession(long, platform, { ...quiet, mode: 'reference' })).not.toThrow();
  });

  it('the longest repetition of a text too long for robust top is computed, not thrown', () => {
    const text = 'a'.repeat(maxPayloadBytes('top', 'robust') + 50);
    expect(longestRepetitionSeconds({ kind: 'payload', text })).toBeGreaterThan(30);
  });

  it('a text that fits normal but not robust keeps the normal profile when the ladder goes robust, and is acked', async () => {
    const text = 'a'.repeat(Math.round((maxPayloadBytes('top', 'robust') + maxPayloadBytes('top')) / 2));
    const { platform, run } = fakePlatform();
    const profiles: Profile[] = [];
    const s = new SenderSession(text, platform, { ...quiet, band: 'top', onRepetition: (r) => profiles.push(r.profile) });
    s.start();
    run(() => profiles.length >= 8);
    expect(profiles).toEqual(new Array(8).fill('normal'));
    s.hear({ type: 'ack', ack: { receipt: false, id: ackId(text) }, band: 'mid' });
    await expect(s.delivered).resolves.toMatchObject({ band: 'top', profile: 'normal' });
    expect(s.state).toBe('receipt');
    s.stop();
  });
});
