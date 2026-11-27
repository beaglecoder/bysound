// A small receiver for the two session frames (ack 0x41, risk 0x58) on the six high bands. The modem receiver (src/modem/receiver.ts)
// already recognises their headers but skips their bodies; until it reports them itself (see docs/SESSION.md §11), this reads them.
// It is the same algorithm in a reduced form: per-tone noise floor and whitening for the preamble search, a plateau to place the start,
// the most likely of the two valid headers, and Reed–Solomon with erasures plus CRC-16 for the body. No echo removal, no lap combining:
// control frames are short and looped, so a lost one is followed by another.
import { fftOf, isPowerOfTwo, type Fft } from '../modem/fft';
import {
  BANDS, HOP_SECONDS, MAGIC_ACK, MAGIC_RISK, PREAMBLE, PREAMBLE_THRESHOLD, SPACING_HZ, TONES, WINDOW_SECONDS, headerBytes, preambleSymbol, type BandId,
} from '../modem/protocol';
import { symbolWindow } from '../modem/symbolWindow';
import { ACK_DATA, CONTROL_PARITY, RISK_DATA, decodeControlBody, parseAck, parseRisk, type AckFrame, type RiskFrame } from './frames';

export type ControlHeard = { kind: 'ack'; ack: AckFrame; band: BandId } | { kind: 'risk'; risk: RiskFrame; band: BandId };
/** Mean energy per tone of each band over the last NOISE_REPORT_SECONDS, in dB relative to a full-scale tone of the grid. */
export type BandNoise = Partial<Record<BandId, number>>;
export const NOISE_REPORT_SECONDS = 0.25;

/** Control frames are never sent on audible. */
export const CONTROL_BANDS: readonly BandId[] = ['top', 'high', 'upper', 'mid', 'xhigh', 'wide'];
const LOW_BIN = BANDS.mid.bin;
const TONE_COUNT = 6 * TONES; // bins 320..415: every high band
const HISTORY = 128;
const RING_SECONDS = 4;
const HEADER_MIN_AGREEMENT = 6;
const PLATEAU_SHARE = 0.97;
const PLATEAU_MAX_HOPS = 40;
const ERASURE_TRIES = [0, 2, 4];
const FLOOR_SECONDS = 0.5;
const FLOOR_WARM_SECONDS = 0.3;
const FLOOR_WARM_TAU = 0.08;
const FLOOR_OUTLIER = 4;
const WHITEN_MIN = 0.05;

const nibbles = (bytes: number[]) => bytes.flatMap((b) => [b >> 4, b & 15]);
const symbolsFor = (bytes: number, groups: number) => Math.ceil((bytes * 2) / groups);

interface Read { start: number; magic?: number; dataLength?: number }
interface BandState {
  id: BandId;
  /** Expected tone (0..95) of each preamble symbol and group. */
  preamble: number[][];
  candidates: { start: number; score: number }[];
  reads: Read[];
  /** No new search before this sample: the frame being read is still sounding. */
  quietUntil: number;
  headers: { magic: number; length: number; nibbles: number[] }[];
}

/** Feed it microphone audio; it calls `onHeard` for every ack, receipt or risk frame decoded and `onNoise` every NOISE_REPORT_SECONDS. */
export class ControlListener {
  private readonly win: number;
  private readonly hop: number;
  private readonly taper: Float64Array;
  private readonly fft: Fft | null;
  private readonly stride: number;
  private readonly ring: Float32Array;
  private readonly windowed: Float64Array;
  private readonly energy = new Float64Array(TONE_COUNT);
  private readonly history: Float64Array[] = Array.from({ length: HISTORY }, () => new Float64Array(TONE_COUNT));
  private readonly floor = new Float64Array(TONE_COUNT);
  private total = 0;
  private hops = 0;
  private bands: BandState[];
  private readonly noiseSum = new Float64Array(TONE_COUNT);
  private noiseHops = 0;
  private readonly noiseEvery: number;
  /** Energy of a full-scale tone on the grid in this window: (Σ window / 2)². */
  private readonly fullScale: number;

  constructor(readonly rate: number, private onHeard: (c: ControlHeard) => void, private onNoise?: (n: BandNoise) => void) {
    this.win = Math.round(WINDOW_SECONDS * rate);
    this.hop = Math.round(HOP_SECONDS * rate);
    this.taper = symbolWindow(this.win);
    this.fullScale = (this.taper.reduce((a, b) => a + b, 0) / 2) ** 2;
    this.noiseEvery = Math.round(NOISE_REPORT_SECONDS / HOP_SECONDS);
    this.windowed = new Float64Array(this.win);
    this.ring = new Float32Array(Math.round(RING_SECONDS * rate));
    const stride = (this.win * SPACING_HZ) / rate;
    const aligned = isPowerOfTwo(this.win) && Math.abs(stride - Math.round(stride)) < 1e-9;
    this.stride = aligned ? Math.round(stride) : 0;
    this.fft = aligned ? fftOf(this.win) : null;
    this.bands = CONTROL_BANDS.map((id) => {
      const b = BANDS[id];
      const tone = (g: number, v: number) => b.bin - LOW_BIN + g * TONES + v;
      return {
        id, candidates: [], reads: [], quietUntil: 0,
        preamble: PREAMBLE.map((_, k) => preambleSymbol(b, k).map((v, g) => tone(g, v))),
        headers: [[MAGIC_ACK, ACK_DATA], [MAGIC_RISK, RISK_DATA]].map(([magic, length]) => ({ magic: magic!, length: length!, nibbles: nibbles(headerBytes(magic!, length!, id)) })),
      };
    });
  }

  feed(x: Float32Array) {
    const step = 4096; // the ring keeps 4 s: a larger block is processed in pieces
    for (let at = 0; at < x.length; at += step) {
      const piece = x.subarray(at, Math.min(at + step, x.length));
      const size = this.ring.length;
      for (let i = 0; i < piece.length; i++) this.ring[(this.total + i) % size] = piece[i]!;
      this.total += piece.length;
      while (this.hops * this.hop + this.win <= this.total) this.onHop(this.hops++);
      for (const b of this.bands) this.readFrames(b);
    }
  }

  /** Energies of the 96 tones in the window that starts at sample `start`. */
  private measure(start: number, out: Float64Array) {
    const size = this.ring.length;
    for (let n = 0; n < this.win; n++) this.windowed[n] = this.ring[(start + n) % size]! * this.taper[n]!;
    if (this.fft) {
      this.fft.analyze(this.windowed);
      for (let t = 0; t < TONE_COUNT; t++) out[t] = this.fft.energy((LOW_BIN + t) * this.stride);
      return;
    }
    for (let t = 0; t < TONE_COUNT; t++) {
      const c = 2 * Math.cos((2 * Math.PI * (LOW_BIN + t) * SPACING_HZ) / this.rate);
      let s1 = 0, s2 = 0;
      for (let n = 0; n < this.win; n++) { const s0 = this.windowed[n]! + c * s1 - s2; s2 = s1; s1 = s0; }
      out[t] = s1 * s1 + s2 * s2 - c * s1 * s2;
    }
  }

  private onHop(h: number) {
    const w = this.history[h % HISTORY]!;
    this.measure(h * this.hop, this.energy);
    const busy = this.bands.some((b) => b.reads.length > 0 || b.quietUntil > this.total);
    if (h === 0) {
      const sorted = Array.from(this.energy).sort((a, b) => a - b);
      this.floor.fill(sorted[sorted.length >> 1]!);
    } else if (!busy) {
      const tau = h * HOP_SECONDS < FLOOR_WARM_SECONDS ? FLOOR_WARM_TAU : FLOOR_SECONDS;
      const a = Math.min(1, HOP_SECONDS / tau);
      for (let t = 0; t < TONE_COUNT; t++) this.floor[t]! += a * (Math.min(this.energy[t]!, FLOOR_OUTLIER * this.floor[t]!) - this.floor[t]!);
    }
    let mean = 0;
    for (let t = 0; t < TONE_COUNT; t++) mean += this.floor[t]!;
    const least = Math.max((WHITEN_MIN * mean) / TONE_COUNT, 1e-12);
    for (let t = 0; t < TONE_COUNT; t++) w[t] = this.energy[t]! / Math.max(this.floor[t]!, least);
    for (const b of this.bands) this.search(b, h);
    if (this.onNoise) this.noteNoise();
  }

  private noteNoise() {
    for (let t = 0; t < TONE_COUNT; t++) this.noiseSum[t]! += this.energy[t]!;
    if (++this.noiseHops < this.noiseEvery) return;
    const out: BandNoise = {};
    for (const id of CONTROL_BANDS) {
      const b = BANDS[id], from = b.bin - LOW_BIN, n = b.groups * TONES;
      let sum = 0;
      for (let t = from; t < from + n; t++) sum += this.noiseSum[t]!;
      out[id] = 10 * Math.log10(sum / n / this.noiseHops / this.fullScale + 1e-30);
    }
    this.noiseSum.fill(0);
    this.noiseHops = 0;
    this.onNoise!(out);
  }

  /** Tests the hypothesis "a preamble of this band ended in hop h", and closes a plateau of candidates. */
  private search(b: BandState, h: number) {
    const band = BANDS[b.id];
    const Fs = this.rate;
    const start = h * this.hop - Math.round((7 * band.sym + band.windowStart) * Fs);
    const first = Math.round((start + Math.round(band.windowStart * Fs)) / this.hop);
    let score = -1;
    if (start >= 0 && first > h - HISTORY && start >= b.quietUntil) {
      let sum = 0;
      for (let k = 0; k < PREAMBLE.length; k++) {
        const hk = Math.round((start + Math.round(k * band.sym * Fs) + Math.round(band.windowStart * Fs)) / this.hop);
        const w = this.history[hk % HISTORY]!;
        b.preamble[k]!.forEach((tone, g) => {
          let all = 0;
          const base = band.bin - LOW_BIN + g * TONES;
          for (let v = 0; v < TONES; v++) all += w[base + v]!;
          sum += all > 0 ? w[tone]! / all : 0;
        });
      }
      score = sum / (PREAMBLE.length * band.groups);
    }
    if (score >= PREAMBLE_THRESHOLD && b.candidates.length < PLATEAU_MAX_HOPS) {
      b.candidates.push({ start, score });
      return;
    }
    if (!b.candidates.length) return;
    const best = Math.max(...b.candidates.map((c) => c.score));
    const plateau = b.candidates.filter((c) => c.score >= PLATEAU_SHARE * best);
    const at = Math.round(plateau.reduce((s, c) => s + c.start, 0) / plateau.length);
    b.candidates = [];
    b.reads.push({ start: at });
    // The longest control frame on this band: no new search inside it.
    b.quietUntil = at + this.symbolStart(band.sym, PREAMBLE.length + symbolsFor(4, band.groups) + symbolsFor(RISK_DATA + CONTROL_PARITY, band.groups));
  }

  private symbolStart(sym: number, k: number) { return Math.round(k * sym * this.rate); }

  /** Probabilities of the nibbles carried by `count` symbols from symbol `first` of the frame at `start`. */
  private soft(b: BandState, start: number, first: number, count: number): number[][] {
    const band = BANDS[b.id];
    const out: number[][] = [];
    const e = new Float64Array(TONE_COUNT);
    for (let k = first; k < first + count; k++) {
      this.measure(start + this.symbolStart(band.sym, k) + Math.round(band.windowStart * this.rate), e);
      for (let g = 0; g < band.groups; g++) {
        const base = band.bin - LOW_BIN + g * TONES;
        let all = 0;
        for (let v = 0; v < TONES; v++) all += e[base + v]!;
        out.push(Array.from({ length: TONES }, (_, v) => (all > 0 ? e[base + v]! / all : 1 / TONES)));
      }
    }
    return out;
  }

  private readFrames(b: BandState) {
    const band = BANDS[b.id];
    const headerSymbols = symbolsFor(4, band.groups);
    const endOf = (start: number, symbols: number) => start + this.symbolStart(band.sym, symbols) + Math.round(band.windowStart * this.rate) + this.win;
    b.reads = b.reads.filter((r) => {
      if (r.magic === undefined) {
        if (this.total < endOf(r.start, PREAMBLE.length + headerSymbols)) return true;
        const soft = this.soft(b, r.start, PREAMBLE.length, headerSymbols).slice(0, 8);
        let best = b.headers[0]!, bestScore = -1;
        for (const c of b.headers) {
          const s = c.nibbles.reduce((sum, n, i) => sum + soft[i]![n]!, 0);
          if (s > bestScore) { bestScore = s; best = c; }
        }
        const agree = best.nibbles.filter((n, i) => argmax(soft[i]!) === n).length;
        if (agree < HEADER_MIN_AGREEMENT) { b.quietUntil = 0; return false; }
        r.magic = best.magic;
        r.dataLength = best.length;
      }
      const bodyBytes = r.dataLength! + CONTROL_PARITY;
      const bodySymbols = symbolsFor(bodyBytes, band.groups);
      if (this.total < endOf(r.start, PREAMBLE.length + headerSymbols + bodySymbols)) return true;
      const soft = this.soft(b, r.start, PREAMBLE.length + headerSymbols, bodySymbols);
      const bytes: number[] = [], confidence: number[] = [];
      for (let i = 0; i < bodyBytes; i++) {
        const hi = soft[2 * i]!, lo = soft[2 * i + 1]!;
        bytes.push((argmax(hi) << 4) | argmax(lo));
        confidence.push(Math.min(margin(hi), margin(lo)));
      }
      const doubtful = confidence.map((c, i) => [c, i] as const).sort((x, y) => x[0] - y[0]).map(([, i]) => i);
      for (const n of ERASURE_TRIES) {
        const data = decodeControlBody(bytes, doubtful.slice(0, n).sort((x, y) => x - y));
        if (!data) continue;
        if (r.magic === MAGIC_ACK) {
          const ack = parseAck(data);
          if (ack) { this.onHeard({ kind: 'ack', ack, band: b.id }); break; }
        } else {
          const risk = parseRisk(data);
          if (risk) { this.onHeard({ kind: 'risk', risk, band: b.id }); break; }
        }
      }
      return false;
    });
  }
}

function argmax(p: number[]) {
  let best = 0;
  for (let i = 1; i < p.length; i++) if (p[i]! > p[best]!) best = i;
  return best;
}
function margin(p: number[]) {
  let a = 0, b = 0;
  for (const v of p) { if (v > a) { b = a; a = v; } else if (v > b) b = v; }
  return a - b;
}
