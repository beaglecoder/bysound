// Shared by the sender and the receiver: timings, errors, alerts and the per-device memory of risk incidents. See docs/SESSION.md.
import { GAP_SECONDS, type Band, type BandId, type Profile } from '../modem/protocol';
import { referenceFrame, payloadFrame } from '../modem/codec';
import type { Heard } from '../modem/receiver';
import { render, fitLevel } from '../modem/modulator';
import { CONTROL_GAP_SECONDS, fingerprint, riskBands, riskFrame, shortId, type RiskFrame, type RiskReason, type Role } from './frames';
import type { Platform } from './ears';
import { fromHex } from './sha256';

/** After every repetition the sender is silent and listens this long (acks, risk frames, other emitters). */
export const LISTEN_WINDOW_SECONDS = 2;
/** Own content heard this long after the last own repetition ended is someone else playing it (a clone). */
export const CLONE_GUARD_SECONDS = 0.5;
/** Receiver: keeps listening this long after the first frame before delivering. */
export const FINAL_LISTEN_SECONDS = 1.5;
/** A header on another band during the final listen extends it to at least this long after the header… */
export const FINAL_EXTEND_SECONDS = 2.5;
/** …but never past this, counted from the first frame. */
export const FINAL_LISTEN_MAX_SECONDS = 6;
/** Ack run: one repetition plus two listen windows plus this margin, clamped to [ACK_RUN_MIN, ACK_RUN_MAX]. */
export const ACK_MARGIN_SECONDS = 0.5;
export const ACK_RUN_MIN_SECONDS = 3;
export const ACK_RUN_MAX_SECONDS = 20;
/** Receiver: after each ack run it listens this long for the receipt. */
export const RECEIPT_LISTEN_SECONDS = 3;
/** Receiver: stops acking this long after delivery even without a receipt. */
export const ACK_TOTAL_SECONDS = 30;
/** Receiver: a reference resolver that has not answered by then fails with TIMEOUT. */
export const RESOLVE_TIMEOUT_SECONDS = 15;

/** How long the receiver loops acks: both sides compute it from the repetition length `repetitionSeconds` (frame + gap). */
export const ackRunSeconds = (repetitionSeconds: number) =>
  Math.min(ACK_RUN_MAX_SECONDS, Math.max(ACK_RUN_MIN_SECONDS, repetitionSeconds + 2 * LISTEN_WINDOW_SECONDS + ACK_MARGIN_SECONDS));
/** How long the sender loops receipts after hearing an ack: covers the end of the ack run and the receiver's receipt listen. */
export const receiptSeconds = (repetitionSeconds: number) => ackRunSeconds(repetitionSeconds) + RECEIPT_LISTEN_SECONDS;

export type SessionErrorCode =
  /** Sender, pre-listen: another transmission is already sounding. Nothing was sent. */
  | 'OTHER_TRANSMISSION'
  /** Sender: its own data is being played by someone else. */
  | 'CLONED_TRANSMISSION'
  /** Sender: another transmission was heard while sending. */
  | 'OTHER_EMITTER'
  /** A risk frame was heard (and honored). */
  | 'RISK_SIGNAL'
  /** Receiver: two different transmissions, or two overlapping frames. */
  | 'MULTIPLE_TRANSMISSIONS'
  | 'TIMEOUT'
  /** Sender: stop() was called before the ack. */
  | 'STOPPED'
  /** Receiver: cancel() was called. */
  | 'CANCELLED'
  /** Receiver, reference mode: the resolver did not know the fingerprint (or failed). */
  | 'NOT_FOUND'
  /** Receiver, reference mode: the resolver returned a text whose SHA-256 does not start with the fingerprint. */
  | 'MISMATCH'
  /** Receiver: a reference frame was heard and there is no resolver. */
  | 'NO_RESOLVER';

export class SessionError extends Error {
  constructor(readonly code: SessionErrorCode, message: string, readonly alert?: Alert, options?: { cause?: unknown }) {
    super(message, options);
    this.name = 'SessionError';
  }
}

/** A risk frame this device emitted or heard. */
export interface Alert {
  direction: 'emitted' | 'heard';
  reason: RiskReason;
  /** Who originated the alert. */
  source: Role;
  hops: number;
  /** Short ids (6 hex) carried by the frame. */
  own: string;
  intruder: string;
  /** Heard: the band it was heard on. Emitted: the bands it is looped on. */
  bands: BandId[];
  /** Heard: the frame's `own` is the short id of this side's data. Informative only. */
  matchesMine: boolean;
  /** Emitted as a relay of a heard alert. */
  relay: boolean;
}

/** Suggested quarantine after a risk frame. bysound never blocks anything because of it: what to do is up to the app. */
export interface QuarantineNotice { durationMs: number; reason: RiskReason; source: Role }

/** Per device: the incidents it emitted or relayed, so it ignores its own frames and relays an incident at most once. */
export class RiskMemory {
  private incidents = new Map<string, number>();
  /** Remembers that this device emitted (or relayed) `key` at `now` (seconds). */
  note(key: string, now: number) { this.incidents.set(key, now); }
  /** This device emitted `key` within the last `windowSeconds`. */
  isOwn(key: string, now: number, windowSeconds: number) {
    const at = this.incidents.get(key);
    return at !== undefined && now - at < windowSeconds;
  }
}
export const sharedRiskMemory = new RiskMemory();

/** Short id (6 hex) of what a frame carries. */
export const shortOf = (h: Heard) => (h.kind === 'payload' ? shortId(h.payload) : h.fingerprint.slice(0, 6));

/** One repetition of a frame, with its gap, at the device rate. */
export function renderRepetition(text: string, mode: 'payload' | 'reference', band: BandId, rate: number, level: number, set = 0, profile: Profile = 'normal'): Float32Array {
  const f = mode === 'payload' ? payloadFrame(text, band, profile) : referenceFrame(fromHex(fingerprint(text)), band, set, profile);
  return fitLevel(render(f.symbols, f.band, rate, GAP_SECONDS), level);
}

/** A control frame looped back to back (CONTROL_GAP_SECONDS after each), alternating `frames`, whole frames only, at least `seconds`. */
export function loopFrames(frames: { symbols: number[][]; band: Band }[], seconds: number, rate: number, level: number): Float32Array {
  const audio = frames.map((f) => fitLevel(render(f.symbols, f.band, rate, CONTROL_GAP_SECONDS), level));
  const parts: Float32Array[] = [];
  let length = 0;
  for (let i = 0; length < seconds * rate; i++) {
    const a = audio[i % audio.length]!;
    parts.push(a);
    length += a.length;
  }
  const out = new Float32Array(length);
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}

/** The risk frame looped on its bands (see riskBands). */
export function riskAudio(r: RiskFrame, work: BandId, avoid: BandId | null, seconds: number, rate: number, level: number) {
  const bands = riskBands(work, avoid);
  return { bands, audio: loopFrames(bands.map((b) => riskFrame(r, b)), seconds, rate, level) };
}

/** What both state machines share: timers and sounds that a stop cancels at once. */
export class Scheduler {
  private timers = new Set<() => void>();
  private sounds = new Set<() => void>();
  constructor(readonly platform: Platform) {}
  after(seconds: number, fn: () => void): () => void {
    const cancel = this.platform.after(seconds, () => { this.timers.delete(cancel); fn(); });
    this.timers.add(cancel);
    return () => { if (this.timers.delete(cancel)) cancel(); };
  }
  play(audio: Float32Array, onEnd: () => void): () => void {
    const stop = this.platform.play(audio, () => { this.sounds.delete(stop); onEnd(); });
    this.sounds.add(stop);
    return () => { if (this.sounds.delete(stop)) stop(); };
  }
  get playing() { return this.sounds.size > 0; }
  clear() {
    for (const c of this.timers) c();
    for (const s of this.sounds) s();
    this.timers.clear();
    this.sounds.clear();
  }
}

