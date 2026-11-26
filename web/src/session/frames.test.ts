import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';
import { GAP_SECONDS, type BandId } from '../modem/protocol';
import { render } from '../modem/modulator';
import { air } from '../modem/testAudio';
import { ControlListener, type ControlHeard } from './controlListener';
import { ackFrame, ackId, bandsOverlap, parseAck, parseRisk, riskBands, riskFrame, shortId, type RiskFrame } from './frames';
import { sha256, hex } from './sha256';

const vectors = (name: string) => JSON.parse(readFileSync(new URL(`../../../test-vectors/${name}`, import.meta.url), 'utf8')).vectors as Record<string, unknown>[];
const REASONS = ['other', 'clone', 'overlap', 'cancelled'] as const;
const h = (a: number[]) => a.map((b) => b.toString(16).padStart(2, '0')).join('');

describe('sha256', () => {
  it('matches node:crypto', () => {
    for (const s of ['', 'abc', 'ñandú 🙂', 'x'.repeat(1000)]) {
      expect(hex(sha256(new TextEncoder().encode(s)))).toBe(createHash('sha256').update(s, 'utf8').digest('hex'));
    }
  });
});

describe('ack frames match test-vectors/v4-acks.json', () => {
  for (const v of vectors('v4-acks.json')) {
    it(`${v.band as string} ${v.receipt ? 'receipt' : 'ack'}`, () => {
      const f = ackFrame({ receipt: v.receipt as boolean, id: ackId(v.text as string) }, v.band as BandId);
      expect(h(f.header)).toBe(v.header);
      expect(h(f.body)).toBe(v.body);
      expect(f.symbols).toEqual(v.symbols);
      expect(Number(f.frameSeconds.toFixed(6))).toBe(v.frameSeconds);
      expect(parseAck(f.body.slice(0, 7))).toEqual({ receipt: v.receipt, id: ackId(v.text as string) });
    });
  }
});

describe('risk frames match test-vectors/v4-risk.json', () => {
  for (const v of vectors('v4-risk.json')) {
    const r: RiskFrame = { reason: REASONS[(v.reason as number) - 1]!, source: v.source ? 'receiver' : 'sender', hops: v.hops as number, own: h(v.own as number[]), intruder: h(v.intruder as number[]) };
    it(`${v.band as string} reason ${v.reason as number} source ${v.source as number} hops ${v.hops as number}`, () => {
      const f = riskFrame(r, v.band as BandId);
      expect(h(f.header)).toBe(v.header);
      expect(h(f.body)).toBe(v.body);
      expect(f.symbols).toEqual(v.symbols);
      expect(Number(f.frameSeconds.toFixed(6))).toBe(v.frameSeconds);
      expect(parseRisk(f.body.slice(0, 10))).toEqual(r);
    });
  }
});

describe('risk bands', () => {
  it('mid first, then the working band, skipping what overlaps the reported band', () => {
    expect(riskBands('top')).toEqual(['mid', 'top']);
    expect(riskBands('mid')).toEqual(['mid']);
    expect(riskBands('audible')).toEqual(['mid']);
    expect(riskBands('top', 'high')).toEqual(['mid']);
    expect(riskBands('top', 'mid')).toEqual(['top']);
    expect(riskBands('xhigh', 'wide')).toEqual(['mid', 'xhigh']); // nothing is clean: the default list
    expect(bandsOverlap('mid', 'top')).toBe(false);
    expect(bandsOverlap('upper', 'mid')).toBe(true);
  });
});

describe('ControlListener', () => {
  const RATE = 48000;
  const listen = (x: Float32Array, rate = RATE) => {
    const heard: ControlHeard[] = [];
    const l = new ControlListener(rate, (c) => heard.push(c));
    for (let i = 0; i < x.length; i += 1024) l.feed(x.subarray(i, i + 1024));
    l.feed(new Float32Array(rate));
    return heard;
  };
  const risk: RiskFrame = { reason: 'overlap', source: 'receiver', hops: 1, own: shortId('a'), intruder: shortId('b') };

  for (const band of ['top', 'high', 'upper', 'mid', 'xhigh', 'wide'] as BandId[]) {
    it(`reads an ack and a risk frame on ${band} in noise`, () => {
      const a = ackFrame({ receipt: false, id: ackId('hello') }, band), r = riskFrame(risk, band);
      const x = air([render(a.symbols, a.band, RATE, GAP_SECONDS), render(r.symbols, r.band, RATE, GAP_SECONDS)], RATE, { noise: 0.05, seed: 3 });
      expect(listen(x)).toEqual([{ kind: 'ack', ack: { receipt: false, id: ackId('hello') }, band }, { kind: 'risk', risk, band }]);
    });
  }

  it('works at 44.1 kHz (Goertzel)', () => {
    const a = ackFrame({ receipt: true, id: ackId('hello') });
    const x = air([render(a.symbols, a.band, 44100, GAP_SECONDS)], 44100, { noise: 0.03 });
    expect(listen(x, 44100)).toEqual([{ kind: 'ack', ack: { receipt: true, id: ackId('hello') }, band: 'high' }]);
  });

  it('reads an ack on mid while a payload plays on top', async () => {
    const { payloadFrame } = await import('../modem/codec');
    const p = payloadFrame('a payload that keeps playing', 'top');
    const a = ackFrame({ receipt: false, id: ackId('x') });
    const payload = render(p.symbols, p.band, RATE, GAP_SECONDS), ack = render(a.symbols, a.band, RATE, GAP_SECONDS);
    const x = air([payload, payload], RATE, { noise: 0.02 });
    x.forEach((_, i) => { if (i >= 30000 && i - 30000 < ack.length) x[i]! += ack[i - 30000]!; });
    expect(listen(x).filter((c) => c.kind === 'ack')).toHaveLength(1);
  });

  it('ignores payload frames and noise', async () => {
    const { payloadFrame } = await import('../modem/codec');
    const p = payloadFrame('not a control frame', 'mid');
    expect(listen(air([render(p.symbols, p.band, RATE, GAP_SECONDS)], RATE, { noise: 0.05 }))).toEqual([]);
    expect(listen(air([new Float32Array(RATE * 3)], RATE, { noise: 0.2, seed: 9 }))).toEqual([]);
  });
});
