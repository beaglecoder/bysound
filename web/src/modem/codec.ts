// v4 frames: packing, CRC, reference (fingerprint) and payload in blocks. Port of Codec.swift (ios/Sources/BySound) and its Kotlin counterpart (android/bysound).
import { rsDecode, rsEncode } from './reedSolomon';
import {
  BANDS, BLOCK_BYTES, BLOCK_DATA, BLOCK_HEADER, BLOCK_PARITY, GAP_SECONDS, HEADER_BYTES, MAGIC_PAYLOAD, MAGIC_REFERENCE, MAX_BLOCKS, MAX_FRAME_SECONDS, MOTHER_PARITY,
  PREAMBLE, REF_BYTES, REF_DATA, REF_PARITY, hasRobust, headerBytes, inProfile, preambleSymbol, type Band, type BandId, type Profile,
} from './protocol';

/** UTF-8 bytes of the run, in chunks of up to 255 that never split a character (the receiver joins consecutive chunks). */
function utf8Chunks(text: string): number[][] {
  const chunks: number[][] = [];
  let cur: number[] = [];
  const encoder = new TextEncoder();
  for (const ch of text) {
    const b = Array.from(encoder.encode(ch));
    if (cur.length + b.length > 255) {
      chunks.push(cur);
      cur = [];
    }
    cur.push(...b);
  }
  if (cur.length > 0) chunks.push(cur);
  return chunks;
}

export function pack(text: string): number[] {
  const dig = (c: number) => c >= 0x30 && c <= 0x39;
  const asc = (c: number) => c >= 0x20 && c <= 0x7e;
  const out: number[] = [];
  let i = 0;
  while (i < text.length) {
    const c = text.charCodeAt(i);
    if (dig(c) && i + 1 < text.length && dig(text.charCodeAt(i + 1))) {
      out.push(0x80 + (c - 0x30) * 10 + (text.charCodeAt(i + 1) - 0x30));
      i += 2;
    } else if (asc(c)) {
      out.push(c);
      i += 1;
    } else {
      let j = i;
      while (j < text.length && !asc(text.charCodeAt(j))) j++;
      for (const chunk of utf8Chunks(text.slice(i, j))) out.push(0xff, chunk.length, ...chunk);
      i = j;
    }
  }
  return out;
}

export function unpack(data: number[]): string | null {
  let s = '';
  let i = 0;
  const utf8 = new TextDecoder('utf-8', { fatal: true });
  while (i < data.length) {
    const b = data[i];
    if (b! >= 0x20 && b! <= 0x7e) { s += String.fromCharCode(b!); i += 1; }
    else if (b! >= 0x80 && b! <= 0xe3) { const v = b! - 0x80; s += `${Math.floor(v / 10)}${v % 10}`; i += 1; }
    else if (b === 0xff) {
      // Consecutive 0xFF runs are a single text: older senders cut every 255 bytes even mid-character.
      const bytes: number[] = [];
      while (data[i] === 0xff) {
        if (i + 1 >= data.length) return null;
        const n = data[i + 1]!;
        if (n === 0 || i + 2 + n > data.length) return null;
        for (let k = 0; k < n; k++) bytes.push(data[i + 2 + k]!);
        i += 2 + n;
      }
      try { s += utf8.decode(new Uint8Array(bytes)); } catch { return null; }
    } else return null;
  }
  return s;
}

export function crc16(data: number[]): number {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

export function crc32(data: number[]): number {
  let c = 0xffffffff;
  for (const b of data) { c ^= b; for (let i = 0; i < 8; i++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1; }
  return (c ^ 0xffffffff) >>> 0;
}

export function toSymbols(bytes: number[], groups: number): number[][] {
  const n = bytes.flatMap((b) => [b >> 4, b & 15]);
  while (n.length % groups) n.push(0);
  const out: number[][] = [];
  for (let i = 0; i < n.length; i += groups) out.push(n.slice(i, i + groups));
  return out;
}

/** `band`: the band with the timings of the frame's profile, the one to pass to render. */
export interface Frame { header: number[]; body: number[]; symbols: number[][]; frameSeconds: number; band: Band }

function frame(b: Band, header: number[], body: number[]): Frame {
  const symbols = PREAMBLE.map((_, k) => preambleSymbol(b, k)).concat(toSymbols(header, b.groups), toSymbols(body, b.groups));
  return { header, body, symbols, frameSeconds: symbols.length * b.sym, band: b };
}

/** The profile a band actually uses: robust only on the bands that have it. */
const profileOf = (bandId: BandId, profile: Profile): Profile => (profile === 'robust' && hasRobust(bandId) ? 'robust' : 'normal');

/**
 * Reference: 12 bytes of the SHA-256 + CRC-16 + the 16 parity bytes of set `set` (0 = A, 1 = B) of the RS(46,14) mother code. `profile`:
 * robust (top, high, upper and mid only) lasts twice as long; the body is the same.
 */
export function referenceFrame(fingerprint: number[], bandId: BandId, set = 0, profile: Profile = 'normal'): Frame {
  if (fingerprint.length !== REF_BYTES) throw new Error('The fingerprint must be 12 bytes');
  const c = crc16(fingerprint);
  const data = fingerprint.concat([c >> 8, c & 255]);
  const mother = rsEncode(data, MOTHER_PARITY);
  const parity = mother.slice(REF_DATA + set * REF_PARITY, REF_DATA + (set + 1) * REF_PARITY);
  const p = profileOf(bandId, profile);
  return frame(inProfile(BANDS[bandId], p), headerBytes(MAGIC_REFERENCE, REF_DATA, bandId, set, p), data.concat(parity));
}

/** Thrown when a payload does not fit in one transmission (`code` is 'TOO_LONG'). */
export class TooLongError extends Error {
  readonly code = 'TOO_LONG';
  constructor(message: string) { super(message); this.name = 'TooLongError'; }
}

const US = (seconds: number) => Math.round(seconds * 1e6);

/** Symbols (preamble + header + body) of a payload frame with `packed` packed bytes (before the CRC-32). */
function payloadSymbols(groups: number, packed: number): number {
  const len = packed + 4;
  const total = Math.ceil(len / BLOCK_DATA);
  const body = len + total * (BLOCK_HEADER + BLOCK_PARITY);
  return PREAMBLE.length + Math.ceil((2 * HEADER_BYTES) / groups) + Math.ceil((2 * body) / groups);
}

/** Exact duration in microseconds of the transmission (frame + gap) of a payload with `packed` packed bytes. */
const payloadMicros = (b: Band, packed: number) => payloadSymbols(b.groups, packed) * US(b.sym) + US(GAP_SECONDS);

/**
 * Largest packed payload (bytes after text packing, before the CRC-32) that fits in MAX_FRAME_SECONDS for the band and profile.
 * ASCII letters pack 1:1 (so it is also the letter count); digit pairs pack 2:1.
 */
export function maxPayloadBytes(bandId: BandId, profile: Profile = 'normal'): number {
  const b = inProfile(BANDS[bandId], profileOf(bandId, profile));
  const cap = US(MAX_FRAME_SECONDS);
  let lo = 0, hi = BLOCK_DATA * MAX_BLOCKS - 4;
  if (payloadMicros(b, hi) <= cap) return hi;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (payloadMicros(b, mid) <= cap) lo = mid; else hi = mid;
  }
  return lo;
}

/** Payload frame: pack + CRC-32 in 48-byte blocks, each one RS([index, total] + data, 12). */
export function payloadFrame(text: string, bandId: BandId, profile: Profile = 'normal'): Frame {
  const content = pack(text);
  const c = crc32(content);
  const data = content.concat([(c >>> 24) & 255, (c >>> 16) & 255, (c >>> 8) & 255, c & 255]);
  const total = Math.ceil(data.length / BLOCK_DATA);
  if (total > MAX_BLOCKS) throw new TooLongError('The data is too long to send by sound');
  const p = profileOf(bandId, profile);
  const b = inProfile(BANDS[bandId], p);
  if (payloadMicros(b, content.length) > US(MAX_FRAME_SECONDS)) {
    throw new TooLongError(`The data is too long: one transmission cannot last more than ${MAX_FRAME_SECONDS} s (maximum for ${bandId}/${p}: ${maxPayloadBytes(bandId, p)} packed bytes, you have ${content.length})`);
  }
  const body: number[] = [];
  for (let b = 0; b < total; b++) body.push(...rsEncode([b, total].concat(data.slice(b * BLOCK_DATA, (b + 1) * BLOCK_DATA)), BLOCK_PARITY));
  return frame(b, headerBytes(MAGIC_PAYLOAD, total, bandId, 0, p), body);
}

export interface Block { index: number; total: number; chunk: number[] }

export function decodeBlock(bytes: number[], erasures: number[] = []): { block: Block; corrected: number } | null {
  if (bytes.length <= BLOCK_HEADER + BLOCK_PARITY || bytes.length > BLOCK_BYTES) return null;
  const r = rsDecode(bytes, BLOCK_PARITY, erasures);
  if (!r) return null;
  const [index = 0, total = 0] = r.data;
  if (total < 1 || index >= total) return null;
  return { block: { index, total, chunk: r.data.slice(BLOCK_HEADER) }, corrected: r.corrected };
}

/** The text if the CRC-32 checks out; null otherwise. `chunks` holds all the blocks in order. */
export function assemble(chunks: number[][]): string | null {
  if (chunks.slice(0, -1).some((c) => c.length !== BLOCK_DATA)) return null;
  const data = chunks.flat();
  if (data.length <= 4) return null;
  const content = data.slice(0, -4);
  const t = data.slice(-4);
  const crc = ((t[0]! << 24) | (t[1]! << 16) | (t[2]! << 8) | t[3]!) >>> 0;
  return crc32(content) === crc ? unpack(content) : null;
}

const parityFrom = (set: number) => REF_DATA + (set & 1) * REF_PARITY;
export const parityRange = (set: number) => Array.from({ length: REF_PARITY }, (_, i) => parityFrom(set) + i);

/** Mother code with whatever parity is available: what is missing and `erasures` are treated as erasures. Verifies the CRC-16. */
export function decodeMother(data: number[], parity: (number[] | null)[], erasures: number[] = []): { fingerprint: number[]; corrected: number } | null {
  if (data.length !== REF_DATA) return null;
  const word = data.concat(new Array<number>(MOTHER_PARITY).fill(0));
  const erased = new Set(erasures);
  for (const set of [0, 1]) {
    const p = parity[set];
    if (p && p.length === REF_PARITY) p.forEach((v, i) => { word[parityFrom(set) + i] = v; });
    else parityRange(set).forEach((i) => erased.add(i));
  }
  if (erased.size > MOTHER_PARITY) return null;
  const r = rsDecode(word, MOTHER_PARITY, Array.from(erased).sort((a, b) => a - b));
  if (!r) return null;
  const content = r.data.slice(0, REF_BYTES);
  if (crc16(content) !== ((r.data[REF_BYTES]! << 8) | r.data[REF_BYTES + 1]!)) return null;
  return { fingerprint: content, corrected: r.corrected };
}
