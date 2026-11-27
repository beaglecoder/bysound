// bysound v4 constants. Normative source: protocol-v4.json (repo root); executable reference: protocol/v4-reference.mjs.
export type BandId = 'top' | 'high' | 'upper' | 'mid' | 'audible' | 'xhigh' | 'wide';

/** Frame profile. `robust` doubles symbol, window and search step (top, high, upper and mid only): twice the time in exchange for about 3 dB. */
export type Profile = 'normal' | 'robust';
/** `profile` is only set on the robust copy of a band (see inProfile); without it the band is normal. */
export interface Band { id: BandId; bin: number; groups: number; amp: number; sym: number; windowStart: number; profile?: Profile }

export const SPACING_HZ = 46.875;
export const TONES = 16;
export const VERSION = 4;
export const GAP_SECONDS = 0.35;
/** Hard cap: one transmission (preamble + header + body + gap) never lasts longer than this. */
export const MAX_FRAME_SECONDS = 30;
export const RAMP_SECONDS = 0.003;
export const HOP_SECONDS = 0.004;
export const WINDOW_SECONDS = 1 / SPACING_HZ;
export const PREAMBLE_THRESHOLD = 0.32;

export const REF_BYTES = 12;
export const REF_DATA = 14;
export const REF_PARITY = 16;
export const MOTHER_PARITY = 32;
export const MOTHER_BYTES = REF_DATA + MOTHER_PARITY;
export const BLOCK_DATA = 48;
export const BLOCK_PARITY = 12;
export const BLOCK_HEADER = 2;
export const BLOCK_BYTES = BLOCK_HEADER + BLOCK_DATA + BLOCK_PARITY;
export const MAX_BLOCKS = 255;
export const HEADER_BYTES = 4;
export const RISK_LENGTH = 10;
export const RISK_BODY = 18;

export const MAGIC_PAYLOAD = 0x56;
export const MAGIC_REFERENCE = 0x52;
export const MAGIC_RISK = 0x58;
export const MAGIC_ACK = 0x41;
export const ACK_LENGTH = 7;
export const ACK_BODY = 15;

export const PREAMBLE = [
  [0, 8, 4, 12], [15, 7, 11, 3], [2, 10, 6, 14], [13, 5, 9, 1],
  [4, 12, 0, 8], [11, 3, 15, 7], [6, 14, 2, 10], [9, 1, 13, 5],
];

const band = (id: BandId, bin: number, groups: number, amp: number, sym = 0.032, windowStart = 0.004): Band => ({ id, bin, groups, amp, sym, windowStart });
export const BANDS: Record<BandId, Band> = {
  top: band('top', 384, 2, 0.3),
  high: band('high', 368, 3, 0.3),
  upper: band('upper', 352, 3, 0.3),
  mid: band('mid', 320, 3, 0.3),
  audible: band('audible', 40, 4, 0.2, 0.04, 0.005),
  xhigh: band('xhigh', 352, 4, 0.25),
  wide: band('wide', 320, 6, 0.2),
};
/** Every band, in channel-id order. */
export const BAND_IDS = Object.keys(BANDS) as BandId[];

export const samples = (seconds: number, rate: number) => Math.round(seconds * rate);
export const symbolStart = (k: number, rate: number, b: Band) => Math.round(k * b.sym * rate);
/** Frequency in Hz of tone `tone` (0..15) of group `group` on band `b`. */
export const toneHz = (b: Band, group: number, tone: number) => (b.bin + group * TONES + tone) * SPACING_HZ;
/** Band identifier in the header. */
export const CHANNEL_IDS: Record<BandId, number> = { top: 0, high: 1, upper: 2, mid: 3, audible: 4, xhigh: 5, wide: 6 };
/** Preamble key [column shift, nibble offset]: each band has its own pattern. */
export const PREAMBLE_KEYS: Record<BandId, [number, number]> = {
  top: [0, 3], high: [3, 9], upper: [2, 14], mid: [1, 13], audible: [0, 0], xhigh: [1, 5], wide: [3, 14],
};
/** Those of the robust profile: each band and profile has its own pattern. */
export const PREAMBLE_KEYS_ROBUST: Partial<Record<BandId, [number, number]>> = { top: [1, 13], high: [3, 11], upper: [0, 8], mid: [2, 9] };
/** The bands that have a robust profile. */
export const ROBUST_BANDS: readonly BandId[] = ['top', 'high', 'upper', 'mid'];
/** The band has a robust profile. */
export const hasRobust = (id: BandId) => ROBUST_BANDS.includes(id);
/** The band with the timings of its profile: robust doubles the symbol and the window start (the receiver sets the window and step). */
export function inProfile(b: Band, profile: Profile = 'normal'): Band {
  return profile === 'robust' && hasRobust(b.id) ? { ...b, sym: b.sym * 2, windowStart: b.windowStart * 2, profile: 'robust' } : b;
}
export function preambleSymbol(b: Band, k: number): number[] {
  const [shift, add] = (b.profile === 'robust' ? PREAMBLE_KEYS_ROBUST[b.id] : undefined) ?? PREAMBLE_KEYS[b.id];
  return Array.from({ length: b.groups }, (_, g) => (PREAMBLE[k]![(g + shift) % 4]! + add) & 15);
}

/** Header CRC-8: polynomial 0x2F, init 0xFF. */
export function crc8(data: number[]): number {
  let c = 0xff;
  for (const byte of data) {
    c ^= byte;
    for (let i = 0; i < 8; i++) c = c & 0x80 ? ((c << 1) ^ 0x2f) & 0xff : (c << 1) & 0xff;
  }
  return c;
}

/** Header: [type, length, CRC-8, version | band | set]. The CRC also covers the profile (0 normal, 1 robust), which is not transmitted. */
export function headerBytes(magic: number, length: number, id: BandId, set = 0, profile: Profile = 'normal'): number[] {
  const version = (VERSION << 4) | (CHANNEL_IDS[id] << 1) | (set & 1);
  return [magic, length & 255, crc8([magic, length & 255, version, profile === 'robust' ? 1 : 0]), version];
}
