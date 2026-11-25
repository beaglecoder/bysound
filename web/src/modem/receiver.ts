// v4 streaming receiver: all 7 bands at once. Port of the v4 part of the Swift demodulator (ios/Sources/BySound).
// Differences from the native ports, meant for the web:
//  - the last block of a payload frame is also combined across repetitions (a short payload is ONE block, and it is the last);
//  - a new preamble after the frame's minimum length cuts off the read of a broken block, so the next repetition is not lost;
//  - an unreadable header inside a frame being read is the look-alike preamble of another band: it does not count as a failure;
//  - clashes and `onHeader` require an almost complete header: with 5 nibbles out of 8 a foreign frame passes for our own.
import { assemble, decodeBlock, decodeMother, parityRange, type Block } from './codec';
import { fftOf, isPowerOfTwo, type Fft } from './fft';
import { symbolWindow } from './symbolWindow';
import {
  ACK_BODY, ACK_LENGTH, BANDS, BAND_IDS, BLOCK_BYTES, BLOCK_HEADER, BLOCK_PARITY, GAP_SECONDS, HEADER_BYTES, HOP_SECONDS, MAGIC_ACK, MAGIC_PAYLOAD, MAGIC_REFERENCE,
  MAGIC_RISK, MAX_BLOCKS, MOTHER_BYTES, PREAMBLE, PREAMBLE_THRESHOLD, REF_DATA, REF_PARITY, RISK_BODY, RISK_LENGTH, SPACING_HZ, TONES,
  VERSION, WINDOW_SECONDS, CHANNEL_IDS, ROBUST_BANDS, crc8, headerBytes, inProfile, preambleSymbol, samples, symbolStart, toneHz, type Band, type BandId,
  type Profile,
} from './protocol';

/** `profile` is only set if the frame was robust. */
export type Heard =
  | { kind: 'payload'; payload: string; band: BandId; correctedBytes: number; combined: number; score?: number; profile?: Profile }
  | { kind: 'reference'; fingerprint: string; band: BandId; correctedBytes: number; combined: number; score?: number; profile?: Profile };

export interface ReceiverEvents {
  onFrame?(h: Heard): void;
  /** A valid header was read on that band. */
  onHeader?(band: BandId): void;
  onBlocks?(closed: number, total: number): void;
  /** Two overlapping transmissions. */
  onClash?(): void;
}

const HOP_HISTORY = 160;
const RING_SECONDS = 12;
/** Spare room in the ring for the block being processed (the worklet sends 128 at a time; it grows if a larger one arrives). */
const RING_SLACK = 4096;
const FLOOR_SECONDS = 0.5;
const FLOOR_WARM_SECONDS = 0.3;
const FLOOR_WARM_TAU = 0.08;
const FLOOR_OUTLIER = 4;
const HEADER_SHARE = 0.12;
const HEADER_MIN_AGREEMENT = 5;
const HEADER_SURE_AGREEMENT = 7;
const COMBINE_MAX = 6;
const COMBINE_SECONDS = 30;
const REBUILD_TRIES_MAX = 300;
const OVERLAP_FROM_SECONDS = 0.3;
const OVERLAP_MIN_HOPS = 2;
const OVERLAP_MIN_SCORE = 0.45;
const SELF_ECHO_SECONDS = 0.3;
const OVERLAP_MARGIN_SECONDS = 0.15;
const HEADER_FAIL_NEAR_SECONDS = 0.15;
const HEADER_FAIL_WAIT_SECONDS = 0.3;
const ERASURE_TRIES = [0, 4, 8, 12];
const BLOCK_ERASURE_TRIES = [0, 2, 4];
const UNIFORM = new Array<number>(16).fill(1 / 16);

type Soft = number[][];
interface Reading { magic: number; length: number; set: number; at: number; channel: BandId; soft: Soft }
interface Decoded { fingerprint: number[]; corrected: number; erasures: number }
interface HeaderCandidate { magic: number; length: number; set: number; nibbles: number[] }

const nibblesOf = (bytes: number[]) => bytes.flatMap((b) => [b >> 4, b & 15]);
/** Possible headers of a band in a profile (the CRC covers the profile). */
function headerCandidates(id: BandId, profile: Profile = 'normal'): HeaderCandidate[] {
  const head = (magic: number, n: number, set = 0): HeaderCandidate => ({ magic, length: n, set, nibbles: nibblesOf(headerBytes(magic, n, id, set, profile)) });
  const out = [head(MAGIC_REFERENCE, REF_DATA, 0), head(MAGIC_REFERENCE, REF_DATA, 1), head(MAGIC_RISK, RISK_LENGTH), head(MAGIC_ACK, ACK_LENGTH)];
  for (let t = 1; t <= MAX_BLOCKS; t++) out.push(head(MAGIC_PAYLOAD, t));
  return out;
}

const hard = (p: number[]) => { let best = 0; for (let v = 1; v < p.length; v++) if (p[v]! > p[best]!) best = v; return best; };
function margin(p: number[]): number {
  let a = -1, b = -1;
  for (const v of p) { if (v > a) { b = a; a = v; } else if (v > b) b = v; }
  return a - b;
}
function hardBytes(soft: Soft): number[] {
  const out: number[] = [];
  for (let i = 0; i + 1 < soft.length; i += 2) out.push((hard(soft[i]!) << 4) | hard(soft[i + 1]!));
  return out;
}
const summed = (readings: Soft[]): Soft => readings[0]!.map((_, i) => readings[0]![i]!.map((_, v) => readings.reduce((s, r) => s + r[i]![v]!, 0)));
const surest = (readings: Soft[]): Soft => readings[0]!.map((_, i) => readings.map((r) => r[i]!).reduce((best, p) => (margin(p) > margin(best) ? p : best)));
const bitCount = (n: number) => { let c = 0; for (let v = n; v; v &= v - 1) c++; return c; };
const leastSureBytes = (soft: Soft, bytes: number) => {
  const sure = soft.map(margin);
  const byte = (i: number) => Math.min(sure[2 * i]!, sure[2 * i + 1]!);
  return Array.from({ length: bytes }, (_, i) => i).sort((a, b) => byte(a) - byte(b));
};

/**
 * Energy of each tone in `x` (Goertzel), for when the grid does not fall on the bins of an FFT (44.1 kHz). It goes 4 tones per pass:
 * each does exactly the same arithmetic as it would alone, but the four chains advance together and the processor does not wait on each one.
 */
function goertzel(x: Float64Array, coeffs: Float64Array, out: Float64Array) {
  const n = x.length;
  let t = 0;
  for (; t + 4 <= coeffs.length; t += 4) {
    const c0 = coeffs[t]!, c1 = coeffs[t + 1]!, c2 = coeffs[t + 2]!, c3 = coeffs[t + 3]!;
    let a1 = 0, a2 = 0, b1 = 0, b2 = 0, d1 = 0, d2 = 0, e1 = 0, e2 = 0;
    for (let i = 0; i < n; i++) {
      const v = x[i]!;
      const a0 = v + c0 * a1 - a2; a2 = a1; a1 = a0;
      const b0 = v + c1 * b1 - b2; b2 = b1; b1 = b0;
      const d0 = v + c2 * d1 - d2; d2 = d1; d1 = d0;
      const e0 = v + c3 * e1 - e2; e2 = e1; e1 = e0;
    }
    out[t] = a1 * a1 + a2 * a2 - c0 * a1 * a2;
    out[t + 1] = b1 * b1 + b2 * b2 - c1 * b1 * b2;
    out[t + 2] = d1 * d1 + d2 * d2 - c2 * d1 * d2;
    out[t + 3] = e1 * e1 + e2 * e2 - c3 * e1 * e2;
  }
  for (; t < coeffs.length; t++) {
    const c = coeffs[t]!;
    let s1 = 0, s2 = 0;
    for (let i = 0; i < n; i++) { const s0 = x[i]! + c * s1 - s2; s2 = s1; s1 = s0; }
    out[t] = s1 * s1 + s2 * s2 - c * s1 * s2;
  }
}

/**
 * Energies of grid tones in window `x`: with an FFT if the tones fall exactly on its bins (at 48 kHz the 1/46.875 s window is
 * 1,024 samples and grid bin b is FFT bin b), otherwise with Goertzel. Same window and same scale (|X(k)|²).
 */
function toneEnergies(r: Receiver, x: Float64Array, firstBin: number, coeffs: Float64Array, out: Float64Array, analyzed = false) {
  const fft = r.fft;
  if (!fft) return goertzel(x, coeffs, out);
  if (!analyzed) fft.analyze(x);
  const stride = r.binStride;
  for (let t = 0; t < out.length; t++) out[t] = fft.energy((firstBin + t) * stride);
}

/** Energies of a tone grid at each search step: a single pass for all the bands of that region. */
class ToneBank {
  private coeffs: Float64Array;
  private history: Float64Array[];
  constructor(readonly baseBin: number, tones: number, rate: number) {
    this.coeffs = Float64Array.from({ length: tones }, (_, t) => 2 * Math.cos((2 * Math.PI * (baseBin + t) * SPACING_HZ) / rate));
    this.history = Array.from({ length: HOP_HISTORY }, () => new Float64Array(tones));
  }
  /** `analyzed`: the window's FFT is already done (the two banks share it). */
  compute(r: Receiver, hop: number, analyzed: boolean) { toneEnergies(r, r.windowed, this.baseBin, this.coeffs, this.history[hop % HOP_HISTORY]!, analyzed); }
  at(hop: number) { return this.history[hop % HOP_HISTORY]!; }
}

class BlockAssembly {
  total = 0;
  chunks = new Map<number, number[]>();
  pending = new Map<number, Soft[]>();
  pendingLast: Soft[] = [];
  loops = 0;
  lastAt = 0;
  channel: BandId | null = null;
  /** Body bytes of the repetition that closed the last block: the header does not say. */
  knownBytes = 0;
  begin(total: number, at: number, horizon: number, channel: BandId) {
    if (total !== this.total || this.lastAt < horizon) this.reset();
    this.total = total;
    this.lastAt = at;
    this.channel = channel;
  }
  reset() {
    this.total = 0;
    this.chunks = new Map();
    this.pending = new Map();
    this.pendingLast = [];
    this.loops = 0;
    this.knownBytes = 0;
  }
}

class Pool {
  readings: Reading[] = [];
  starts: { band: BandId; at: number; span: number }[] = [];
  blocks = new BlockAssembly();
  headerFails: { at: number; noted: number }[] = [];
  failures = 0;
  constructor(private rate: number, private events: ReceiverEvents) {}
  clash() { this.events.onClash?.(); }
  noteStart(band: BandId, at: number, span: number) {
    const selfEcho = SELF_ECHO_SECONDS * this.rate;
    const margin_ = OVERLAP_MARGIN_SECONDS * this.rate;
    const overlapped = this.starts.some((o) => {
      const apart = Math.abs(at - o.at);
      return (apart >= selfEcho || o.band !== band) && apart < (o.at < at ? o.span : span) - margin_;
    });
    this.starts.push({ band, at, span });
    if (this.starts.length > 32) this.starts.shift();
    if (overlapped) this.clash();
  }
  /** Waits a little: the real band may read its header in the same step or right after. */
  flushHeaderFails(total: number) {
    if (!this.headerFails.length) return;
    const due = total - HEADER_FAIL_WAIT_SECONDS * this.rate;
    const near = HEADER_FAIL_NEAR_SECONDS * this.rate;
    const kept = this.headerFails.filter((f) => f.noted > due);
    for (const f of this.headerFails) {
      if (f.noted > due) continue;
      if (!this.starts.some((s) => f.at >= s.at - near && f.at <= s.at + s.span)) this.failures++;
    }
    this.headerFails = kept;
  }
}

const enum State { Search, Header, Body }

class ChannelReader {
  state = State.Search;
  progress = -1;
  nibbleConfidence: number[] = [];
  private tones: number;
  private coeffs: Float64Array;
  private floor: Float64Array;
  private scratch: Float64Array;
  /** Echo tail (see softSymbol); null at the start of a read. */
  private previous: Float64Array | null = null;
  /** Whitened symbol energies and probabilities of a group: softSymbol's working buffers, with no per-symbol allocation. */
  private symbolW: Float64Array;
  private groupD = new Float64Array(TONES);
  private echo = 0;
  private candStarts: number[] = [];
  private candScores: number[] = [];
  /** Preamble match (0..1) of the last frame: goes into the report. */
  private frameScore = 0;
  private frameStart = 0;
  private searchFrom = 0;
  private magic = 0;
  /** The header matched completely. With corrected errors, the length it carries may be false (see tryBlocks). */
  private headerSure = true;
  private dataLength = 0;
  private set = 0;
  private bodySoft: Soft = [];
  private bodyRead = 0;
  private lastTried = 0;
  private preamble: number[][];
  private candidates: HeaderCandidate[];
  private headerSymbols: number;
  private windowOut: Float64Array;
  private endOffset: number;
  private preambleSpan: number;

  constructor(private r: Receiver, readonly band: Band, private bank: ToneBank, private pool: Pool, private events: ReceiverEvents) {
    this.tones = band.groups * TONES;
    this.coeffs = Float64Array.from({ length: this.tones }, (_, t) => 2 * Math.cos((2 * Math.PI * toneHz(band, Math.floor(t / TONES), t % TONES)) / r.rate));
    this.floor = new Float64Array(this.tones);
    this.scratch = new Float64Array(this.tones);
    this.windowOut = new Float64Array(this.tones);
    this.symbolW = new Float64Array(this.tones);
    this.preamble = PREAMBLE.map((_, k) => preambleSymbol(band, k));
    this.candidates = headerCandidates(band.id, band.profile);
    this.headerSymbols = Math.ceil((HEADER_BYTES * 2) / band.groups);
    this.endOffset = band.windowStart + r.windowSeconds;
    this.preambleSpan = (PREAMBLE.length - 1) * band.sym + this.endOffset;
  }

  private get offset() { return this.band.bin - this.bank.baseBin; }
  private get isBlocks() { return this.magic === MAGIC_PAYLOAD; }
  /** Payload blocks in the frame being read; 0 for a reference. */
  get blocksInFrame() { return this.isBlocks ? this.dataLength : 0; }
  private get bodyBytes() { return this.isBlocks ? (this.dataLength - 1) * BLOCK_BYTES + BLOCK_HEADER + 1 + BLOCK_PARITY : REF_DATA + REF_PARITY; }
  private get bodyBytesMax() { return this.isBlocks ? this.dataLength * BLOCK_BYTES : this.bodyBytes; }
  private symbolsFor(bytes: number) { return Math.ceil((bytes * 2) / this.band.groups); }
  private start(k: number) { return symbolStart(k, this.r.rate, this.band); }
  private symbolEnd(k: number) { return this.frameStart + this.start(k) + samples(this.endOffset, this.r.rate); }
  private get firstBody() { return PREAMBLE.length + this.headerSymbols; }
  private hopEnd(i: number) { return this.r.winSize + i * this.r.hopSize; }
  private energy(hop: number, t: number) { return this.bank.at(hop)[this.offset + t]; }

  onHop(index: number) {
    if (this.state === State.Search) this.updateFloor(index);
    switch (this.state) {
      case State.Search:
        if (this.nibbleConfidence.length) this.nibbleConfidence = [];
        this.search(index);
        break;
      case State.Header: this.tryHeader(); break;
      case State.Body:
        if (this.searchInBody(index)) break;
        this.tryBody();
        break;
    }
    this.progress = this.currentProgress();
  }

  private updateFloor(hop: number) {
    const f = this.floor;
    if (hop === 0) {
      const sorted = Array.from({ length: this.tones }, (_, t) => this.energy(hop, t)).sort((a, b) => a! - b!);
      f.fill(sorted[this.tones >> 1]!);
      return;
    }
    const warm = (hop + 1) * this.r.hopSeconds < FLOOR_WARM_SECONDS;
    const a = this.r.hopSeconds / (warm ? FLOOR_WARM_TAU : FLOOR_SECONDS);
    for (let t = 0; t < this.tones; t++) {
      const x = this.energy(hop, t), v = f[t];
      f[t] = v! + a * ((v! > 0 ? Math.min(x!, FLOOR_OUTLIER * v!) : x)! - v!);
    }
  }

  private whitenMinimum() {
    let s = 0;
    for (let t = 0; t < this.tones; t++) s += this.floor[t]!;
    return Math.max((s / this.tones) * 0.05, 1e-20);
  }

  private preambleScore(index: number): { start: number; score: number } | null {
    const rate = this.r.rate;
    const start = this.hopEnd(index) - Math.round(this.preambleSpan * rate);
    if (start < 0) return null;
    const minimum = this.whitenMinimum();
    const w = this.scratch;
    let score = 0;
    for (let k = 0; k < PREAMBLE.length; k++) {
      const end = start + Math.round((k * this.band.sym + this.endOffset) * rate);
      const hop = Math.round((end - this.r.winSize) / this.r.hopSize);
      if (hop < 0 || hop > index || index - hop >= HOP_HISTORY) return null;
      const e = this.bank.at(hop), off = this.offset;
      for (let t = 0; t < this.tones; t++) w[t] = e[off + t]! / Math.max(this.floor[t]!, minimum);
      for (let g = 0; g < this.band.groups; g++) {
        const base = g * TONES;
        let sum = 0;
        for (let v = 0; v < TONES; v++) sum += w[base + v]!;
        if (sum > 1e-12) score += w[base + this.preamble[k]![g]!]! / sum;
      }
    }
    return { start, score: score / (PREAMBLE.length * this.band.groups) };
  }

  /** Center of the candidate plateau. */
  private plateau(): number {
    const best = Math.max(...this.candScores);
    this.frameScore = best;
    const centered = this.candStarts.filter((_, i) => this.candScores[i]! >= best * 0.97);
    const at = Math.round(centered.reduce((a, b) => a + b, 0) / centered.length);
    this.candStarts = [];
    this.candScores = [];
    return at;
  }

  private search(index: number) {
    const found = this.preambleScore(index);
    if (!found || found.start < this.searchFrom) return;
    if (found.score >= PREAMBLE_THRESHOLD && this.candStarts.length < 64) {
      this.candStarts.push(found.start);
      this.candScores.push(found.score);
      return;
    }
    if (this.candStarts.length) this.enterFrame(this.plateau());
  }

  private enterFrame(at: number) {
    this.frameStart = at;
    this.state = State.Header;
    this.measureEcho();
    this.tryHeader();
  }

  /** The preamble is also searched for inside a body: on top of it is another sender, and past the minimum it is the next repetition. */
  private searchInBody(index: number): boolean {
    const found = this.preambleScore(index);
    if (!found) return false;
    const rate = this.r.rate;
    const minEnd = this.frameStart + this.start(this.firstBody + this.symbolsFor(this.bodyBytes));
    if (found.score >= PREAMBLE_THRESHOLD && found.start >= this.frameStart + OVERLAP_FROM_SECONDS * rate && this.candStarts.length < 64) {
      this.candStarts.push(found.start);
      this.candScores.push(found.score);
      return false;
    }
    if (!this.candStarts.length) return false;
    const hops = this.candStarts.length;
    const score = Math.max(...this.candScores);
    const at = this.plateau();
    if (at + this.start(PREAMBLE.length) < minEnd) {
      // Data that resembles the preamble gives an isolated peak: two senders give a high plateau. A false alarm cuts off the read.
      if (hops >= OVERLAP_MIN_HOPS && score >= OVERLAP_MIN_SCORE) this.pool.clash();
      return false;
    }
    if (!this.isBlocks) return false;
    this.tryBody();
    if (this.state === State.Body) this.finishBlocks(null, this.bodySoft.length >> 1);
    this.enterFrame(at);
    return true;
  }

  /** Normalized (by noise floor) energies of symbol k of the current frame, into `out`. */
  private symbolEnergies(k: number, out: Float64Array): Float64Array {
    this.r.window(this.frameStart + this.start(k) + samples(this.band.windowStart, this.r.rate));
    toneEnergies(this.r, this.r.windowed, this.band.bin, this.coeffs, this.windowOut);
    const minimum = this.whitenMinimum();
    for (let t = 0; t < this.tones; t++) out[t] = this.windowOut[t]! / Math.max(this.floor[t]!, minimum);
    return out;
  }

  private measureEcho() {
    const pre = this.preamble;
    const ratios: number[] = [];
    let last: Float64Array = this.symbolEnergies(0, new Float64Array(this.tones));
    let cur: Float64Array = new Float64Array(this.tones);
    for (let k = 1; k < pre.length; k++) {
      this.symbolEnergies(k, cur);
      for (let g = 0; g < this.band.groups; g++) {
        const t = g * TONES + pre[k - 1]![g]!;
        if (last[t]! > 1e-12) ratios.push(cur[t]! / last[t]!);
      }
      [last, cur] = [cur, last];
    }
    ratios.sort((a, b) => a - b);
    this.echo = ratios.length ? Math.min(Math.max(ratios[ratios.length >> 1]!, 0), 0.8) : 0;
    this.previous = last;
  }

  private softSymbol(k: number): Soft {
    const w = this.symbolEnergies(k, this.symbolW);
    // The tail is updated in place: each group uses the previous tail first and only then overwrites it.
    const prev = this.previous ?? (this.previous = new Float64Array(this.tones));
    const echo = this.echo, d = this.groupD;
    const out: Soft = [];
    for (let g = 0; g < this.band.groups; g++) {
      const base = g * TONES;
      let sum = 0;
      for (let v = 0; v < TONES; v++) {
        d[v] = Math.max(w[base + v]! - echo * prev[base + v]!, 0);
        sum += d[v]!;
      }
      for (let v = 0; v < TONES; v++) prev[base + v] = w[base + v]! + echo * prev[base + v]!;
      if (sum <= 1e-12) out.push(UNIFORM.slice());
      else {
        const p = new Array<number>(TONES);
        for (let v = 0; v < TONES; v++) p[v] = d[v]! / sum;
        out.push(p);
      }
    }
    return out;
  }

  private resetSearch(keepConfidence = false) {
    this.state = State.Search;
    if (!keepConfidence) this.nibbleConfidence = [];
    this.searchFrom = this.frameStart + this.start(1);
    this.candStarts = [];
    this.candScores = [];
  }

  private bodyReset() {
    this.bodySoft = [];
    this.bodyRead = 0;
    this.nibbleConfidence = [];
    this.lastTried = BLOCK_HEADER + BLOCK_PARITY;
  }

  private tryHeader() {
    const first = PREAMBLE.length;
    if (this.r.total < this.symbolEnd(first + this.headerSymbols - 1)) return;
    const soft: Soft = [];
    for (let i = 0; i < this.headerSymbols; i++) for (const p of this.softSymbol(first + i)) soft.push(p);
    const n = HEADER_BYTES * 2;
    let best = -1;
    let choice = this.candidates[0];
    for (const c of this.candidates) {
      let s = 0;
      for (let i = 0; i < n; i++) s += soft[i]![c.nibbles[i]!]!;
      if (s > best) { best = s; choice = c; }
    }
    let agree = 0;
    for (let i = 0; i < n; i++) if (hard(soft[i]!) === choice!.nibbles[i]) agree++;
    if (this.foreignHeader(soft)) return this.resetSearch();
    if (agree < HEADER_MIN_AGREEMENT) return this.unreadableHeader();
    this.magic = choice!.magic;
    this.dataLength = choice!.length;
    this.set = choice!.set;
    this.headerSure = agree === n;
    this.bodyReset();
    if (this.magic === MAGIC_RISK || this.magic === MAGIC_ACK) {
      // The web does not handle risk signals or acks: they are not payload, so they are skipped.
      this.resetSearch();
      this.searchFrom = this.frameStart + this.start(this.firstBody + this.symbolsFor(this.magic === MAGIC_ACK ? ACK_BODY : RISK_BODY));
      return;
    }
    if (agree >= HEADER_SURE_AGREEMENT) {
      this.events.onHeader?.(this.band.id);
      this.pool.noteStart(this.band.id, this.frameStart, this.start(this.firstBody + this.symbolsFor(this.bodyBytes)));
    }
    this.state = State.Body;
    this.tryBody();
  }

  /** Unreadable: if this band had been reading a recent repetition, its header applies to this one. */
  /** The header's CRC-8 checks out with another band: it is another sender, not read as our own. */
  private foreignHeader(soft: Soft): boolean {
    const [magic = 0, length = 0, crc = 0, version = 0] = hardBytes(soft.slice(0, HEADER_BYTES * 2));
    if (version >> 4 !== VERSION || ((version >> 1) & 7) === CHANNEL_IDS[this.band.id]) return false;
    return crc === crc8([magic, length, version, this.band.profile === 'robust' ? 1 : 0]);
  }

  private unreadableHeader() {
    const horizon = this.frameStart - COMBINE_SECONDS * this.r.rate;
    const last = this.pool.readings[this.pool.readings.length - 1];
    const blocks = this.pool.blocks;
    if (last && last.channel === this.band.id && last.at >= horizon) {
      this.magic = last.magic;
      this.dataLength = last.length;
      this.set = last.set ^ 1;
    } else if (blocks.total > 0 && blocks.channel === this.band.id && blocks.lastAt >= horizon) {
      this.magic = MAGIC_PAYLOAD;
      this.dataLength = blocks.total;
      this.set = 0;
    } else {
      this.pool.headerFails.push({ at: this.frameStart, noted: this.r.total });
      this.resetSearch();
      return;
    }
    this.headerSure = true;
    this.bodyReset();
    this.state = State.Body;
    this.tryBody();
  }

  private tryBody() {
    if (this.isBlocks) return this.tryBlocks();
    const first = this.firstBody;
    const count = this.symbolsFor(this.bodyBytes);
    const before = this.bodySoft.length;
    while (this.bodyRead < count && this.r.total >= this.symbolEnd(first + this.bodyRead)) {
      for (const p of this.softSymbol(first + this.bodyRead)) this.bodySoft.push(p);
      this.bodyRead++;
    }
    const nibbles = this.bodyBytes * 2;
    if (this.bodySoft.length !== before) {
      const n = Math.min(nibbles, this.bodySoft.length);
      const conf = new Array<number>(n);
      for (let i = 0; i < n; i++) conf[i] = margin(this.bodySoft[i]!);
      this.nibbleConfidence = conf;
    }
    if (this.r.total < this.symbolEnd(first + count - 1)) return;
    const reading = this.bodySoft.slice(0, nibbles);
    const end = this.frameStart + this.start(first + count);
    this.resetSearch(true);
    const single = this.decodeReference([{ set: this.set, soft: reading }]);
    if (single) return this.finishReference(single, 1, end);
    const pool = this.pool;
    const horizon = this.frameStart - COMBINE_SECONDS * this.r.rate;
    pool.readings = pool.readings.filter((x) => x.at >= horizon);
    const same = pool.readings.filter((x) => x.magic === this.magic && x.length === this.dataLength);
    pool.readings.push({ magic: this.magic, length: this.dataLength, set: this.set, at: this.frameStart, channel: this.band.id, soft: reading });
    if (pool.readings.length > COMBINE_MAX * 3) pool.readings.shift();
    const earlier = same.slice(-(COMBINE_MAX - 1)).map((x) => ({ set: x.set, soft: x.soft }));
    const unseen = this.unseenReadings(first, count, nibbles, COMBINE_MAX - 1 - earlier.length);
    const rebuilt = this.rebuildReference({ set: this.set, soft: reading }, earlier.concat(unseen));
    if (rebuilt) return this.finishReference(rebuilt.decoded, rebuilt.readings, end);
    pool.failures++;
  }

  private finishReference(d: Decoded, combined: number, end: number) {
    this.searchFrom = end;
    this.pool.readings = [];
    const fingerprint = d.fingerprint.map((b) => b.toString(16).padStart(2, '0')).join('');
    this.deliver({ kind: 'reference', fingerprint, band: this.band.id, correctedBytes: d.corrected, combined }, end);
  }

  /** Repetitions in the stored audio whose start was not detected. */
  private unseenReadings(first: number, count: number, nibbles: number, limit: number): { set: number; soft: Soft }[] {
    const current = this.frameStart;
    const period = this.start(first + count) + samples(GAP_SECONDS, this.r.rate);
    const out: { set: number; soft: Soft }[] = [];
    let at = current - period;
    while (out.length < limit && at >= 0 && at > this.r.total - this.r.ringSize) {
      if (!this.pool.readings.some((x) => x.channel === this.band.id && Math.abs(x.at - at) < period / 2)) {
        this.frameStart = at;
        this.previous = null;
        const soft: Soft = [];
        for (let i = 0; i < count; i++) for (const p of this.softSymbol(first + i)) soft.push(p);
        out.push({ set: this.set ^ (Math.floor((current - at) / period) & 1), soft: soft.slice(0, nibbles) });
      }
      at -= period;
    }
    this.frameStart = current;
    return out;
  }

  private motherLayout(set: number, soft: Soft): Soft {
    const out: Soft = Array.from({ length: MOTHER_BYTES * 2 }, () => UNIFORM);
    const dataNibbles = REF_DATA * 2;
    for (let i = 0; i < Math.min(soft.length, dataNibbles + REF_PARITY * 2); i++) out[i < dataNibbles ? i : i + (set & 1) * REF_PARITY * 2] = soft[i]!;
    return out;
  }

  private decodeReference(readings: { set: number; soft: Soft }[]): Decoded | null {
    const layouts = readings.map((x) => this.motherLayout(x.set, x.soft));
    const have = [readings.some((x) => !(x.set & 1)), readings.some((x) => x.set & 1)];
    return this.decodeMotherSoft(layouts.length === 1 ? layouts[0]! : summed(layouts), have, this.disagreement(layouts));
  }

  private disagreement(layouts: Soft[]): number[] {
    if (layouts.length < 2) return [];
    const bytes = layouts.map((l) => hardBytes(l.slice(0, 2 * REF_DATA)));
    return Array.from({ length: REF_DATA }, (_, i) => i).filter((i) => new Set(bytes.map((b) => b[i])).size > 1);
  }

  private decodeMotherSoft(soft: Soft, have: boolean[], disagree: number[]): Decoded | null {
    const bytes = hardBytes(soft);
    const m = soft.map(margin);
    const sure = (i: number) => Math.min(m[2 * i]!, m[2 * i + 1]!);
    const data = bytes.slice(0, REF_DATA);
    const parity = [0, 1].map((set) => (have[set] ? parityRange(set).map((i) => bytes[i]!) : null));
    const dataPositions = Array.from({ length: REF_DATA }, (_, i) => i);
    const variants: { parity: (number[] | null)[]; base: number[]; known: number[] }[] = [];
    for (const set of [0, 1]) {
      if (!have[set]) continue;
      const only: (number[] | null)[] = [null, null];
      only[set] = parity[set]!;
      variants.push({ parity: only, base: [], known: dataPositions.concat(parityRange(set)) });
    }
    if (have[0] && have[1]) variants.push({ parity, base: disagree, known: Array.from({ length: MOTHER_BYTES }, (_, i) => i) });
    for (const v of variants) {
      const leastSure = v.known.filter((p) => !v.base.includes(p)).sort((a, b) => sure(a) - sure(b));
      for (const f of ERASURE_TRIES) {
        const erasures = v.base.concat(leastSure.slice(0, f));
        const d = decodeMother(data, v.parity, erasures);
        if (d) return { fingerprint: d.fingerprint, corrected: d.corrected, erasures: erasures.length };
      }
    }
    return null;
  }

  private rebuildReference(current: { set: number; soft: Soft }, earlier: { set: number; soft: Soft }[]): { decoded: Decoded; readings: number } | null {
    if (!earlier.length) return null;
    const masks = Array.from({ length: (1 << earlier.length) - 1 }, (_, i) => i + 1).sort((a, b) => bitCount(b) - bitCount(a));
    let tries = 0;
    for (const mask of masks) {
      const readings = [current].concat(earlier.filter((_, i) => (mask >> i) & 1));
      const layouts = readings.map((x) => this.motherLayout(x.set, x.soft));
      const have = [readings.some((x) => !(x.set & 1)), readings.some((x) => x.set & 1)];
      const disagree = this.disagreement(layouts);
      for (const candidate of [summed(layouts), surest(layouts)]) {
        if (tries + ERASURE_TRIES.length > REBUILD_TRIES_MAX) return null;
        tries += ERASURE_TRIES.length;
        const decoded = this.decodeMotherSoft(candidate, have, disagree);
        if (decoded) return { decoded, readings: readings.length };
      }
    }
    return null;
  }

  private tryBlocks() {
    const first = this.firstBody;
    const maxSymbols = this.symbolsFor(this.bodyBytesMax);
    let read = false;
    while (this.bodyRead < maxSymbols && this.r.total >= this.symbolEnd(first + this.bodyRead)) {
      for (const p of this.softSymbol(first + this.bodyRead)) this.bodySoft.push(p);
      this.bodyRead++;
      read = true;
    }
    if (read) {
      const from = Math.max(0, this.bodySoft.length - BLOCK_BYTES * 2);
      const conf = new Array<number>(this.bodySoft.length - from);
      for (let i = from; i < this.bodySoft.length; i++) conf[i - from] = margin(this.bodySoft[i]!);
      this.nibbleConfidence = conf;
    }
    // A header with errors may announce a false length, and waiting for that frame leaves the band deaf. If in addition the first
    // block does not close, this was not the frame: search again. The sender repeats, so a real frame is not lost.
    if (!this.headerSure && this.dataLength > 1 && this.bodySoft.length >> 1 >= BLOCK_BYTES) {
      const opening = decodeBlock(hardBytes(this.bodySoft.slice(0, 2 * BLOCK_BYTES)));
      if (!opening || opening.block.index !== 0 || opening.block.total !== this.dataLength) {
        this.pool.failures++;
        this.resetSearch();
        return;
      }
      this.headerSure = true;
    }
    const lastStart = (this.dataLength - 1) * BLOCK_BYTES;
    const available = this.bodySoft.length >> 1;
    for (let length = this.lastTried + 1; length <= Math.min(available - lastStart, BLOCK_BYTES); length++) {
      this.lastTried = length;
      const bytes = hardBytes(this.bodySoft.slice(2 * lastStart, 2 * (lastStart + length)));
      const r = decodeBlock(bytes);
      if (r && r.block.index === this.dataLength - 1 && r.block.total === this.dataLength) return this.finishBlocks(r, lastStart + length);
    }
    if (this.bodyRead === maxSymbols && this.lastTried >= BLOCK_BYTES) this.finishBlocks(null, this.bodyBytesMax);
  }

  private closeBlock(index: number, soft: Soft, pending: Soft[]) {
    const candidates = [soft];
    if (pending.length) candidates.push(summed([soft, ...pending]), surest([soft, ...pending]));
    for (const candidate of candidates) {
      const bytes = hardBytes(candidate);
      const leastSure = leastSureBytes(candidate, BLOCK_BYTES);
      for (const f of BLOCK_ERASURE_TRIES) {
        const r = decodeBlock(bytes, leastSure.slice(0, f));
        if (r && r.block.index === index && r.block.total === this.dataLength && r.block.chunk.length === 48) return r;
      }
    }
    return null;
  }

  /** The last block does not say its length: each length is tried with the combined reads of several repetitions. */
  private closeLast(readings: Soft[]) {
    for (const candidate of [summed(readings), surest(readings)]) {
      const bytes = hardBytes(candidate);
      for (let length = BLOCK_HEADER + BLOCK_PARITY + 1; length <= BLOCK_BYTES; length++) {
        const r = decodeBlock(bytes.slice(0, length));
        if (r && r.block.index === this.dataLength - 1 && r.block.total === this.dataLength) return r;
      }
    }
    return null;
  }

  private finishBlocks(last: { block: Block; corrected: number } | null, usedBytes: number) {
    const a = this.pool.blocks;
    const rate = this.r.rate;
    const n = this.dataLength;
    const end = this.frameStart + this.start(this.firstBody + this.symbolsFor(usedBytes));
    a.begin(n, this.frameStart, this.frameStart - COMBINE_SECONDS * rate, this.band.id);
    let gained = 0, corrected = 0;
    const lastStart = (n - 1) * BLOCK_BYTES;
    if (last) {
      if (!a.chunks.has(last.block.index)) gained++;
      a.chunks.set(last.block.index, last.block.chunk);
      a.knownBytes = usedBytes;
      corrected += last.corrected;
    } else if (!a.chunks.has(n - 1) && this.bodySoft.length > 2 * lastStart) {
      const soft = this.bodySoft.slice(2 * lastStart, 2 * (lastStart + BLOCK_BYTES));
      while (soft.length < BLOCK_BYTES * 2) soft.push(UNIFORM);
      a.pendingLast = a.pendingLast.concat([soft]).slice(-COMBINE_MAX);
      const r = a.pendingLast.length > 1 ? this.closeLast(a.pendingLast) : null;
      if (r) {
        a.chunks.set(n - 1, r.block.chunk);
        a.pendingLast = [];
        gained++;
        corrected += r.corrected;
      }
    }
    for (let i = 0; i < n - 1; i++) {
      if (a.chunks.has(i)) continue;
      if (2 * (i + 1) * BLOCK_BYTES > this.bodySoft.length) break;
      const soft = this.bodySoft.slice(2 * i * BLOCK_BYTES, 2 * (i + 1) * BLOCK_BYTES);
      const r = this.closeBlock(i, soft, a.pending.get(i) ?? []);
      if (r) {
        a.chunks.set(i, r.block.chunk);
        a.pending.delete(i);
        gained++;
        corrected += r.corrected;
      } else {
        a.pending.set(i, (a.pending.get(i) ?? []).concat([soft]).slice(-(COMBINE_MAX - 1)));
      }
    }
    a.loops++;
    this.resetSearch();
    // Without the last block there is no way to know where it ended: the next repetition may have started before the maximum.
    this.searchFrom = last ? end : Math.min(end, this.frameStart + this.start(this.firstBody + this.symbolsFor(this.bodyBytes)));
    if (gained > 0) this.events.onBlocks?.(a.chunks.size, n);
    if (a.chunks.size === n) {
      const text = assemble(Array.from({ length: n }, (_, i) => a.chunks.get(i)!));
      const loops = a.loops;
      a.reset();
      if (text !== null) return this.deliver({ kind: 'payload', payload: text, band: this.band.id, correctedBytes: corrected, combined: loops }, end);
      this.pool.failures++;
      return;
    }
    if (gained === 0) this.pool.failures++;
  }

  /** Not delivered if a frame on another band started while this one was sounding. */
  private deliver(h: Heard, end: number) {
    const preamble = this.start(PREAMBLE.length);
    const start = this.frameStart;
    if (this.pool.starts.some((s) => s.band !== this.band.id && s.at > start && s.at + preamble < end)) this.pool.clash();
    else this.events.onFrame?.({ ...h, score: this.frameScore, ...(this.band.profile === 'robust' ? { profile: 'robust' as const } : {}) });
  }

  private currentProgress(): number {
    if (this.state === State.Search) return -1;
    const clamp = (x: number) => Math.min(Math.max(x, 0), 1);
    const headerEnd = this.symbolEnd(this.firstBody - 1);
    const total = this.r.total;
    if (this.state === State.Header) return HEADER_SHARE * clamp((total - this.frameStart) / (headerEnd - this.frameStart));
    const known = this.isBlocks ? this.pool.blocks.knownBytes : 0;
    const end = this.symbolEnd(this.firstBody + this.symbolsFor(known || this.bodyBytes) - 1);
    return HEADER_SHARE + (1 - HEADER_SHARE) * clamp((total - headerEnd) / (end - headerEnd));
  }
}

/**
 * `profile`: robust listens only to robust frames (top, high, upper and mid), with double window and step. To hear both profiles use
 * two receivers on the same audio (see ReceiverHost), like the two banks of the native ports.
 */
export class Receiver {
  readonly winSize: number;
  readonly hopSize: number;
  readonly windowSeconds: number;
  readonly hopSeconds: number;
  /** Samples that can be re-read backwards (12 s). The ring also has room for the block in flight. */
  readonly ringSize: number;
  readonly windowed: Float64Array;
  /** FFT of the window if the grid tones fall exactly on its bins (48 kHz, 96 kHz); null otherwise (44.1 kHz: Goertzel). */
  readonly fft: Fft | null;
  /** FFT bin of grid tone b: b · binStride. */
  readonly binStride: number;
  private readonly taper: Float64Array;
  total = 0;
  private ring: Float32Array;
  private hops = 0;
  private sinceHop = 0;
  private highBank: ToneBank;
  private lowBank: ToneBank | null;
  private readers: ChannelReader[];
  private pool: Pool;

  constructor(readonly rate: number, events: ReceiverEvents = {}, readonly profile: Profile = 'normal') {
    const scale = profile === 'robust' ? 2 : 1;
    this.windowSeconds = WINDOW_SECONDS * scale;
    this.hopSeconds = HOP_SECONDS * scale;
    this.winSize = samples(this.windowSeconds, rate);
    this.hopSize = samples(this.hopSeconds, rate);
    this.ringSize = Math.round(RING_SECONDS * rate);
    this.ring = new Float32Array(this.ringSize + RING_SLACK);
    this.windowed = new Float64Array(this.winSize);
    this.taper = symbolWindow(this.winSize);
    const stride = (this.winSize * SPACING_HZ) / rate;
    const aligned = isPowerOfTwo(this.winSize) && Math.abs(stride - Math.round(stride)) < 1e-9 && Math.round(stride) >= 1;
    this.binStride = aligned ? Math.round(stride) : 0;
    this.fft = aligned && (BANDS.wide.bin + 6 * TONES) * this.binStride < this.winSize / 2 ? fftOf(this.winSize) : null;
    this.highBank = new ToneBank(BANDS.mid.bin, 6 * TONES, rate);
    // Audible has no robust profile: the robust receiver does not compute its bank.
    this.lowBank = profile === 'robust' ? null : new ToneBank(BANDS.audible.bin, 4 * TONES, rate);
    this.pool = new Pool(rate, events);
    const ids = profile === 'robust' ? ROBUST_BANDS : BAND_IDS;
    this.readers = ids.map((id) => new ChannelReader(this, inProfile(BANDS[id], profile), id === 'audible' ? this.lowBank! : this.highBank, this.pool, events));
  }

  feed(x: Float32Array) {
    // The block is stored at once (with the wrapping pointer, no modulo per sample) and then processed step by step.
    // While processing, the ring runs ahead by up to one block: it has room for the 12 s that are re-read plus that block.
    if (this.ringSize + x.length > this.ring.length) this.grow(this.ringSize + x.length);
    const size = this.ring.length;
    let p = this.total % size;
    for (let i = 0; i < x.length; i++) {
      this.ring[p] = x[i]!;
      if (++p === size) p = 0;
    }
    let left = x.length;
    while (left > 0) {
      // Samples until the next step: the same moment the sample-by-sample count gave.
      const n = Math.min(left, Math.max(this.hopSize - this.sinceHop, this.winSize - this.total, 1));
      this.total += n;
      this.sinceHop += n;
      left -= n;
      if (this.sinceHop >= this.hopSize && this.total >= this.winSize) {
        this.sinceHop = 0;
        this.onHop();
      }
    }
  }

  /** A block larger than the spare room: the ring grows keeping what is stored (sample p goes at p % length). */
  private grow(size: number) {
    const old = this.ring, ring = new Float32Array(size);
    for (let at = Math.max(0, this.total - old.length); at < this.total; at++) ring[at % size] = old[at % old.length]!;
    this.ring = ring;
  }

  /** Copies the window [start, start + winSize) into `windowed`, with smoothed edges (see symbolWindow). */
  window(start: number) {
    const size = this.ring.length;
    let p = ((start % size) + size) % size;
    for (let n = 0; n < this.winSize; n++) {
      this.windowed[n] = this.ring[p]! * this.taper[n]!;
      if (++p === size) p = 0;
    }
  }

  private onHop() {
    const index = this.hops++;
    this.window(this.winSize + index * this.hopSize - this.winSize);
    // With FFT, a single transform per step feeds both banks.
    if (this.fft) this.fft.analyze(this.windowed);
    this.highBank.compute(this, index, this.fft !== null);
    this.lowBank?.compute(this, index, this.fft !== null);
    for (const r of this.readers) r.onHop(index);
    this.pool.flushHeaderFails(this.total);
  }

  get failures() { return this.pool.failures; }
  /** The frame in flight: the one in its body if any; one in its header may be the preamble of another band. */
  private get active() {
    return this.readers.find((r) => r.state === State.Body) ?? this.readers.find((r) => r.state === State.Header);
  }
  get progress() { return this.active?.progress ?? -1; }
  get nibblesConfidence() { return this.readers.find((r) => r.nibbleConfidence.length)?.nibbleConfidence ?? []; }
  /** Known from the header; the first block only closes when the frame ends. */
  get blocksTotal() { return this.readers.find((r) => r.nibbleConfidence.length)?.blocksInFrame ?? 0; }
}
