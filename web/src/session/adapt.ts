// Automatic adaptation of the sender (docs/SESSION.md §9): channel ladder, ambient check, robust ladder and calibration memory.
// Pure functions and small classes: the sender state machine calls them.
import { pack, payloadMicros, referenceFrame } from '../modem/codec';
import { BANDS, GAP_SECONDS, hasRobust, inProfile, type BandId, type Profile } from '../modem/protocol';
import type { BandNoise } from './controlListener';

/** The default ladder, from the least audible band down. mid, the most audible, is last. */
export const DEFAULT_CHANNELS: readonly BandId[] = ['top', 'high', 'upper', 'mid'];
/** Repetitions on a channel before hopping to the next one (doubled when starting from a calibration). */
export const HOP_REPETITIONS = 3;
export const CALIBRATED_HOP_FACTOR = 2;
/** Repetitions in the normal profile without an ack before the rest of the ladder goes robust. */
export const ROBUST_AFTER_REPETITIONS = 6;
/** A channel this much noisier than the quietest one is skipped. */
export const NOISY_CHANNEL_DB = 10;
/** Quietest channel at or above this: a loud place (dB per tone relative to a full-scale grid tone; not calibrated in the field). */
export const LOUD_PLACE_DB = -63;
/** Noise of the starting channel at or above this: start robust (same scale; not calibrated in the field). */
export const ROBUST_NOISE_DB = -68;
/** Sends that may start from a calibration without a read on its channel before it is forgotten. */
export const CALIBRATION_USES = 10;

/** From `start` down the chain, then the channels above it, nearest first: the most audible ones come last. */
export function channelOrder(start: BandId, chain: readonly BandId[]): BandId[] {
  const at = chain.indexOf(start);
  if (at < 0) return [start];
  return [...chain.slice(at), ...chain.slice(0, at).reverse()];
}

export interface Ambient {
  /** Median of the noise reports heard during the pre-listen, per band. */
  noise: BandNoise;
  /** Channels skipped: NOISY_CHANNEL_DB above the quietest. */
  noisy: BandId[];
  /** Even the quietest channel is at LOUD_PLACE_DB or more. */
  loud: boolean;
}

/** Reads the noise reports of the pre-listen. null without reports (no microphone). */
export function readAmbient(reports: readonly BandNoise[], channels: readonly BandId[]): Ambient | null {
  if (!reports.length) return null;
  const noise: BandNoise = {};
  for (const b of channels) {
    const v = reports.map((r) => r[b]).filter((x): x is number => x !== undefined).sort((x, y) => x - y);
    if (v.length) noise[b] = v[v.length >> 1]!;
  }
  const levels = channels.filter((b) => noise[b] !== undefined);
  if (!levels.length) return null;
  const quietest = Math.min(...levels.map((b) => noise[b]!));
  return { noise, noisy: levels.filter((b) => noise[b]! > quietest + NOISY_CHANNEL_DB), loud: quietest >= LOUD_PLACE_DB };
}

export interface Calibration {
  /** Where (and how) the last receiver read this device. */
  band: BandId;
  profile: Profile;
  /** Sends that started from it since that read. */
  uses: number;
}
/** Where calibrations live. In memory by default; the browser API uses localStorage. */
export interface CalibrationStore {
  load(): Calibration | null;
  save(c: Calibration | null): void;
}
/** A calibration store that lives in memory (lost on reload). */
export function memoryCalibrationStore(initial: Calibration | null = null): CalibrationStore {
  let c = initial;
  return { load: () => c, save: (next) => { c = next; } };
}
/** A calibration store in localStorage under `key`; falls back to memory where storage is unavailable (private mode, workers). */
export function localStorageCalibrationStore(key = 'bysound.calibration'): CalibrationStore {
  const storage = (): Storage | null => { try { return globalThis.localStorage ?? null; } catch { return null; } };
  const fallback = memoryCalibrationStore();
  return {
    load() {
      try {
        const raw: unknown = JSON.parse(storage()?.getItem(key) ?? 'null');
        return parseCalibration(raw) ?? fallback.load();
      } catch { return fallback.load(); }
    },
    save(c) {
      fallback.save(c);
      try { if (c) storage()?.setItem(key, JSON.stringify(c)); else storage()?.removeItem(key); } catch { /* memory only */ }
    },
  };
}
/** Validates a calibration read from storage. */
export function parseCalibration(raw: unknown): Calibration | null {
  const c = raw as Partial<Calibration> | null;
  if (!c || typeof c !== 'object' || !DEFAULT_CHANNELS.concat(['xhigh', 'wide']).includes(c.band as BandId)) return null;
  if (typeof c.uses !== 'number' || !Number.isInteger(c.uses) || c.uses < 0) return null;
  return { band: c.band as BandId, profile: c.profile === 'robust' ? 'robust' : 'normal', uses: c.uses };
}
export const sharedCalibrationStore = memoryCalibrationStore();

/** Robust after ROBUST_AFTER_REPETITIONS normal repetitions without an ack (only on bands that have it), or from the start. */
export class RobustLadder {
  reason: 'noise' | 'repetitions' | null = null;
  private normal = 0;
  constructor(private auto: boolean, private robust: boolean, reason: 'noise' | null = null) { this.reason = reason; }
  next(band: BandId): Profile {
    if (!this.robust && this.auto && this.normal >= ROBUST_AFTER_REPETITIONS) {
      this.robust = true;
      this.reason = 'repetitions';
    }
    const p: Profile = this.robust && hasRobust(band) ? 'robust' : 'normal';
    if (p === 'normal') this.normal++;
    return p;
  }
}

export interface Plan {
  /** Channels in the order they are played, cycling. */
  route: BandId[];
  /** Repetitions per channel. */
  every: number;
  /** The calibration it starts from, if any. */
  calibration: Calibration | null;
  startRobust: 'calibration' | 'noise' | null;
}

/**
 * Where to play: a fixed `band` is played as is. Otherwise the ladder starts at a valid calibration (fewer than CALIBRATION_USES uses,
 * its band in the chain) or at the top of the chain, skips the noisy channels (the calibrated one stays: it is where this speaker
 * works) and, without an explicit profile, starts robust if the calibration was robust or the starting channel is very noisy.
 */
export function plan(o: { band?: BandId; channels?: readonly BandId[]; profile?: Profile; calibration: Calibration | null; ambient: Ambient | null }): Plan {
  if (o.band) return { route: [o.band], every: Infinity, calibration: null, startRobust: null };
  const chain = o.channels?.length ? o.channels : DEFAULT_CHANNELS;
  const cal = o.calibration && o.calibration.uses < CALIBRATION_USES && chain.includes(o.calibration.band) ? o.calibration : null;
  let route = channelOrder(cal?.band ?? chain[0]!, chain);
  const a = o.ambient;
  if (a) {
    const kept = route.filter((b) => !a.noisy.includes(b) || b === cal?.band);
    route = kept.length ? kept : [chain.slice().sort((x, y) => (a.noise[x] ?? 0) - (a.noise[y] ?? 0))[0]!];
  }
  const startRobust = o.profile !== undefined ? null : cal?.profile === 'robust' ? 'calibration' : a && (a.noise[route[0]!] ?? -Infinity) >= ROBUST_NOISE_DB ? 'noise' : null;
  return { route, every: HOP_REPETITIONS * (cal ? CALIBRATED_HOP_FACTOR : 1), calibration: cal, startRobust };
}

/** The send went through every channel of its route and came back to one: a late read says nothing about the speaker. */
export function lapped(played: readonly BandId[], routeLength: number): boolean {
  if (routeLength < 2) return false;
  let changes = 0;
  let last: BandId | null = null;
  for (const b of played) if (b !== last) { changes++; last = b; }
  return changes > routeLength;
}

/**
 * The longest repetition (frame + gap) this content can have on the default ladder: top in the robust profile. The receiver's ack
 * run and the sender's receipts are timed with it, so they cover a listen window even after a hop or a switch to robust.
 * Computed, not rendered: a text too long for robust top (it was sent normal, or on another band) still gets a duration.
 */
export function longestRepetitionSeconds(content: { kind: 'payload'; text: string } | { kind: 'reference' }): number {
  if (content.kind === 'payload') return payloadMicros(inProfile(BANDS.top, 'robust'), pack(content.text).length) / 1e6;
  return referenceFrame(new Array<number>(12).fill(0), 'top', 0, 'robust').frameSeconds + GAP_SECONDS;
}
