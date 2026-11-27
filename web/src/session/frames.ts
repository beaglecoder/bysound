// Session frames: acknowledgement / receipt (0x41) and risk signal (0x58). Byte layout in docs/SESSION.md §3; the bytes match
// protocol/v4-reference.mjs (buildAck, buildRisk) and test-vectors/v4-acks.json, v4-risk.json.
import { crc16, toSymbols, type Frame } from '../modem/codec';
import { BANDS, BAND_IDS, CHANNEL_IDS, MAGIC_ACK, MAGIC_RISK, PREAMBLE, TONES, headerBytes, preambleSymbol, type BandId, type Profile } from '../modem/protocol';
import { rsDecode, rsEncode } from '../modem/reedSolomon';
import { fromHex, hex, textHash } from './sha256';

/** Reed–Solomon parity bytes of an ack and of a risk frame. */
export const CONTROL_PARITY = 8;
/** Data bytes (with the CRC-16) of each frame: the header's `length` field. */
export const ACK_DATA = 7;
export const RISK_DATA = 10;
/** A risk frame heard with this many hops is never relayed again. */
export const RISK_MAX_HOPS = 2;
/** Acks travel on mid, receipts on high: the two do not share tones, so both sides can sound at once. */
export const ACK_BAND: BandId = 'mid';
export const RECEIPT_BAND: BandId = 'high';
/** Silence after each control frame (ack, receipt, risk) when they are looped. */
export const CONTROL_GAP_SECONDS = 0.15;

export type RiskReason = 'other' | 'clone' | 'overlap' | 'cancelled';
export type Role = 'sender' | 'receiver';
const REASONS: RiskReason[] = ['other', 'clone', 'overlap', 'cancelled'];
const ROLES: Role[] = ['sender', 'receiver'];

export interface AckFrame {
  /** false: ack (the receiver got the data). true: receipt (the sender heard the ack). */
  receipt: boolean;
  /** First 4 bytes of SHA-256(payload), 8 hex characters. */
  id: string;
  /** The band and profile of the frame the receiver delivered (a receipt echoes them). Absent: not stated. */
  heard?: { band: BandId; profile: Profile };
}
export interface RiskFrame {
  reason: RiskReason;
  /** Who originated the alert: the sender of the data or its receiver. Relays keep it. */
  source: Role;
  /** 0 for the original, +1 per relay. */
  hops: number;
  /** First 3 bytes of SHA-256 of the data the originator was sending or had received, 6 hex characters ("000000" if unknown). */
  own: string;
  /** The same for the other transmission, "000000" if it could not be read. */
  intruder: string;
}

export const UNKNOWN_SHORT = '000000';

/** SHA-256 of the text, 64 hex characters. */
export const payloadId = (text: string) => hex(textHash(text));
/** The 12-byte fingerprint a reference frame carries, 24 hex characters. */
export const fingerprint = (text: string) => payloadId(text).slice(0, 24);
/** What an ack carries: 4 bytes, 8 hex characters. */
export const ackId = (text: string) => payloadId(text).slice(0, 8);
/** What a risk frame carries: 3 bytes, 6 hex characters. */
export const shortId = (text: string) => payloadId(text).slice(0, 6);

const withCrc = (data: number[]) => { const c = crc16(data); return data.concat([c >> 8, c & 255]); };

/** High nibble of the ack's first byte: 0 (not stated), or robust << 3 | (channel id + 1). */
const heardNibble = (h: AckFrame['heard']) => (h ? ((h.profile === 'robust' ? 8 : 0) | (CHANNEL_IDS[h.band] + 1)) : 0);

/** Ack data bytes: [heard << 4 | kind (2 ack, 3 receipt), id0..id3, CRC-16 hi, lo]. */
export function ackBytes(a: AckFrame): number[] {
  return withCrc([(heardNibble(a.heard) << 4) | (a.receipt ? 3 : 2), ...fromHex(a.id)]);
}
/** Risk data bytes: [reason 1..4, hops << 4 | source, own0..2, intruder0..2, CRC-16 hi, lo]. */
export function riskBytes(r: RiskFrame): number[] {
  return withCrc([REASONS.indexOf(r.reason) + 1, ((r.hops & 15) << 4) | ROLES.indexOf(r.source), ...fromHex(r.own), ...fromHex(r.intruder)]);
}

function controlFrame(magic: number, data: number[], bandId: BandId): Frame {
  const b = BANDS[bandId];
  const header = headerBytes(magic, data.length, bandId);
  const body = rsEncode(data, CONTROL_PARITY);
  const symbols = PREAMBLE.map((_, k) => preambleSymbol(b, k)).concat(toSymbols(header, b.groups), toSymbols(body, b.groups));
  return { header, body, symbols, frameSeconds: symbols.length * b.sym, band: b };
}

/** An ack or receipt frame (§3.1), by default on mid (ack) or high (receipt). Always the normal profile. */
export const ackFrame = (a: AckFrame, bandId: BandId = a.receipt ? RECEIPT_BAND : ACK_BAND) => controlFrame(MAGIC_ACK, ackBytes(a), bandId);
/** A risk frame (§3.2) on `bandId`. Always the normal profile. */
export const riskFrame = (r: RiskFrame, bandId: BandId) => controlFrame(MAGIC_RISK, riskBytes(r), bandId);

const crcOk = (d: number[]) => d.length > 2 && crc16(d.slice(0, -2)) === ((d[d.length - 2]! << 8) | d[d.length - 1]!);

/** Ack data bytes (after Reed–Solomon) → the ack, or null if the CRC-16 or a field is wrong. */
export function parseAck(data: number[]): AckFrame | null {
  if (data.length !== ACK_DATA || !crcOk(data)) return null;
  const kind = data[0]! & 15, h = data[0]! >> 4;
  if (kind !== 2 && kind !== 3) return null;
  const ack: AckFrame = { receipt: kind === 3, id: hex(data.slice(1, 5)) };
  if (h === 0) return ack;
  const band = BAND_IDS.find((b) => CHANNEL_IDS[b] === (h & 7) - 1);
  if (!band) return null;
  return { ...ack, heard: { band, profile: h & 8 ? 'robust' : 'normal' } };
}
/** Risk data bytes (after Reed–Solomon) → the risk frame, or null if the CRC-16 or a field is wrong. */
export function parseRisk(data: number[]): RiskFrame | null {
  if (data.length !== RISK_DATA || !crcOk(data)) return null;
  const reason = REASONS[data[0]! - 1], source = ROLES[data[1]! & 15];
  if (!reason || !source) return null;
  return { reason, source, hops: data[1]! >> 4, own: hex(data.slice(2, 5)), intruder: hex(data.slice(5, 8)) };
}

/** Body bytes (data + parity) → data, correcting with RS. `erasures`: positions of doubtful bytes. */
export function decodeControlBody(body: number[], erasures: number[] = []): number[] | null {
  return rsDecode(body, CONTROL_PARITY, erasures)?.data ?? null;
}

/** The incident a risk frame reports, without the hops: a relay is the same incident. */
export const riskKey = (r: RiskFrame) => `${r.reason}/${r.source}/${r.own}/${r.intruder}`;

/** Two bands share at least one tone. */
export function bandsOverlap(a: BandId, b: BandId): boolean {
  const x = BANDS[a], y = BANDS[b];
  return x.bin < y.bin + y.groups * TONES && y.bin < x.bin + x.groups * TONES;
}

/**
 * Bands a risk frame is looped on, in order. mid first (it does not overlap top or high), then the working band (unless it is mid or
 * audible). With `avoid` (the band of what is being reported), only the bands that do not overlap it; if none is left, top or mid; if
 * neither, the default list. Never audible.
 */
export function riskBands(work: BandId, avoid?: BandId | null): BandId[] {
  const base: BandId[] = work === 'audible' || work === 'mid' ? ['mid'] : ['mid', work];
  if (!avoid || avoid === 'audible') return base;
  const clean = base.filter((b) => !bandsOverlap(b, avoid));
  if (clean.length) return clean;
  const spare = (['mid', 'top'] as BandId[]).filter((b) => !bandsOverlap(b, avoid));
  return spare.length ? spare : base;
}
