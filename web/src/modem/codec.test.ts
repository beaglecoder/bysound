import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';
import { assemble, decodeBlock, decodeMother, pack, payloadFrame, referenceFrame, unpack } from './codec';
import { rsDecode, rsEncode } from './reedSolomon';
import { BANDS, type BandId } from './protocol';

const path = new URL('../../../test-vectors/v4-frames.json', import.meta.url);
const file = JSON.parse(readFileSync(path, 'utf8'));
const hex = (a: number[]) => a.map((b) => b.toString(16).padStart(2, '0')).join('');
const fingerprint = (t: string) => Array.from(createHash('sha256').update(t, 'utf8').digest()).slice(0, 12);

describe('vectores v4-frames.json', () => {
  it('the bands are the same', () => {
    for (const [id, b] of Object.entries<any>(file.bands)) expect({ ...BANDS[id as BandId], id: undefined, windowStart: undefined }).toEqual({ ...b, id: undefined, windowStart: undefined });
  });
  // The robust ones are in robust.test.ts.
  const normal = file.vectors.filter((v: any) => v.profile === 'normal');
  it('there are 42 normal ones', () => expect(normal.length).toBe(42));
  it.each(normal.map((v: any) => ({ ...v, id: `${v.name} ${v.mode} ${v.band} ${v.set}` })) as any[])('$id', (v: any) => {
    const f = v.mode === 'reference' ? referenceFrame(fingerprint(v.text), v.band, v.set) : payloadFrame(v.text, v.band);
    expect(hex(f.header)).toBe(v.header);
    expect(hex(f.body)).toBe(v.body);
    expect(f.symbols).toEqual(v.symbols);
    expect(Number(f.frameSeconds.toFixed(6))).toBe(v.frameSeconds);
  });
});

describe('codes', () => {
  it('pack and unpack round trip', () => {
    for (const t of ['', '0123', 'a1b22', 'ñandú 🙂 12', file.vectors[0].text]) expect(unpack(pack(t))).toBe(t);
  });
  it('RS corrects 6 errors, or 12 erasures', () => {
    const data = Array.from({ length: 50 }, (_, i) => (i * 37) & 255);
    const word = rsEncode(data, 12);
    const bad = word.slice();
    [0, 7, 13, 30, 44, 61].forEach((p) => { bad[p]! ^= 0x5a; });
    expect(rsDecode(bad, 12)?.data).toEqual(data);
    const erased = word.slice();
    const at = [1, 2, 3, 10, 11, 12, 20, 21, 22, 50, 51, 52];
    at.forEach((p) => { erased[p]! ^= 0xff; });
    expect(rsDecode(erased, 12, at)?.data).toEqual(data);
    bad[20]! ^= 1;
    expect(rsDecode(bad, 12)).toBeNull();
  });
  it('a block is corrected and the payload assembled', () => {
    const f = payloadFrame('hola 1234 ñ', 'upper');
    const body = f.body.slice();
    body[3]! ^= 0xff;
    const r = decodeBlock(body);
    expect(r?.corrected).toBe(1);
    expect(assemble([r!.block.chunk])).toBe('hola 1234 ñ');
  });
  it('the reference checks out with a single set, and with both together', () => {
    const fp = fingerprint('dato');
    const a = referenceFrame(fp, 'top', 0).body, b = referenceFrame(fp, 'top', 1).body;
    const data = a.slice(0, 14);
    expect(decodeMother(data, [a.slice(14), null])?.fingerprint).toEqual(fp);
    expect(decodeMother(data, [null, b.slice(14)])?.fingerprint).toEqual(fp);
    const hurt = data.slice();
    for (let i = 0; i < 14; i += 2) hurt[i]! ^= 0x11;
    expect(decodeMother(hurt, [a.slice(14), b.slice(14)])?.fingerprint).toEqual(fp);
  });
});
