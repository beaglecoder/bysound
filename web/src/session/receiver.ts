// Session receiver (docs/SESSION.md §6): first frame → final listen → delivery → acks until the receipt, and the confirm gate that
// keeps watching for other emitters until the app calls confirmed(). A plain state machine over Platform and AirEvents.
import type { BandId } from '../modem/protocol';
import { longestRepetitionSeconds } from './adapt';
import type { Heard } from '../modem/receiver';
import {
  ACK_TOTAL_SECONDS, FINAL_EXTEND_SECONDS, FINAL_LISTEN_MAX_SECONDS, FINAL_LISTEN_SECONDS, RECEIPT_LISTEN_SECONDS, RESOLVE_TIMEOUT_SECONDS, RiskMemory, Scheduler,
  SessionError, ackRunSeconds, loopFrames, riskAudio, sharedRiskMemory, shortOf, type Alert, type QuarantineNotice, type SessionErrorCode,
} from './common';
import type { AirEvent, Platform } from './ears';
import { UNKNOWN_SHORT, ackFrame, ackId, payloadId, riskKey, shortId, type RiskFrame, type RiskReason } from './frames';
import { resolveSecurity, type Security, type SecurityLevel, type SecurityOverrides } from './security';

export interface ReceiverOptions {
  /** Fails with TIMEOUT if no frame was heard by then. Default 60. */
  timeoutSeconds?: number;
  /**
   * Reference mode: returns the text whose fingerprint (24 hex) was heard, from data the app already has (a Map, a local file…).
   * bysound checks that SHA-256(text) starts with the fingerprint.
   */
  resolve?: (fingerprint: string) => string | null | undefined | Promise<string | null | undefined>;
  /** Acknowledge by sound after delivery. Default true. */
  ack?: boolean;
  /** Output level 0..1 of acks and risk frames. Default 1. */
  level?: number;
  securityLevel?: SecurityLevel;
  security?: SecurityOverrides;
  memory?: RiskMemory;
  onAlert?(alert: Alert): void;
  onQuarantine?(notice: QuarantineNotice): void;
  /** The first frame was heard (delivery follows after the final listen). */
  onHeard?(band: BandId): void;
  onState?(state: ReceiverState): void;
}

export type ReceiverState = 'listening' | 'final-listen' | 'delivered' | 'closed' | 'failed';
export interface Received { text: string; band: BandId; via: 'payload' | 'reference' }
export interface Confirmation {
  /** clean: watched until now. expired: the watch hit confirmWatchSeconds first. off: no watch at this level. */
  watch: 'clean' | 'expired' | 'off';
  watchedSeconds: number;
  /** From the end of the watch to the confirmed() call (expired only). */
  uncoveredSeconds: number;
}

export class ReceiverSession {
  state: ReceiverState = 'listening';
  readonly result: Promise<Received>;
  /** true when the sender's receipt was heard; false when acking ended without one (or acks are off). */
  readonly receipt: Promise<boolean>;
  /** Resolves when the session needs neither the microphone nor the speaker any more. */
  readonly finished: Promise<void>;
  readonly security: Security;
  private readonly s: Scheduler;
  private readonly memory: RiskMemory;
  private first: Heard | null = null;
  private firstAt = 0;
  private finalEnd = 0;
  private cancelFinal: (() => void) | null = null;
  private text: string | null = null;
  private resolved: Promise<void> | null = null;
  private received: Received | null = null;
  private conflict: SessionError | null = null;
  private closed: 'confirmed' | 'cancelled' | null = null;
  private confirmation: Confirmation | null = null;
  private watching = false;
  private watchStart = 0;
  private watchEnd: number | null = null;
  private watchExpired = false;
  private acking = false;
  private alerting = false;
  private seen = new Set<string>();
  private settle!: { resolve: (r: Received) => void; reject: (e: SessionError) => void };
  private settleReceipt!: (heard: boolean) => void;
  private resolveFinished!: () => void;
  private stopAcks: (() => void) | null = null;

  constructor(private readonly platform: Platform, private readonly options: ReceiverOptions = {}) {
    this.security = resolveSecurity(options.securityLevel, options.security);
    this.memory = options.memory ?? sharedRiskMemory;
    this.s = new Scheduler(platform);
    this.result = new Promise((resolve, reject) => { this.settle = { resolve, reject }; });
    this.result.catch(() => undefined);
    this.receipt = new Promise((resolve) => { this.settleReceipt = resolve; });
    this.finished = new Promise((resolve) => { this.resolveFinished = resolve; });
  }

  start() {
    this.s.after(this.options.timeoutSeconds ?? 60, () => { if (this.state === 'listening') this.fail('TIMEOUT', 'No transmission was heard'); });
  }

  /** The confirm gate: call it before acting on the data. Fails if another emitter or a risk frame showed up while watching. */
  async confirmed(): Promise<Confirmation> {
    await this.result;
    if (this.closed === 'cancelled') throw new SessionError('CANCELLED', 'The session was cancelled before confirming');
    if (this.conflict) throw this.conflict;
    if (this.confirmation) return this.confirmation;
    const now = this.platform.now();
    const sec = this.security;
    let c: Confirmation;
    if (!sec.confirmWatch) c = { watch: 'off', watchedSeconds: 0, uncoveredSeconds: 0 };
    else if (this.watchExpired) c = { watch: 'expired', watchedSeconds: this.watchEnd! - this.watchStart, uncoveredSeconds: now - this.watchEnd! };
    else c = { watch: 'clean', watchedSeconds: (this.watchEnd ?? now) - this.watchStart, uncoveredSeconds: 0 };
    this.closed = 'confirmed';
    this.confirmation = c;
    this.stopWatch();
    this.setState('closed');
    this.maybeFinished();
    return c;
  }

  /** The person left without confirming: stop listening and acking. Never throws. A risk frame already sounding finishes. */
  cancel() {
    if (this.closed) return;
    this.closed = 'cancelled';
    this.fail('CANCELLED', 'Cancelled by the app');
    this.stopWatch();
    this.endAcks(false);
    if (this.state === 'delivered') this.setState('closed');
    this.maybeFinished();
  }

  hear(e: AirEvent) {
    switch (e.type) {
      case 'frame': return this.heardFrame(e.heard);
      case 'header': return this.heardHeader(e.band);
      case 'clash': return this.heardClash();
      case 'ack': return this.heardAck(e.ack.receipt, e.ack.id);
      case 'risk': return this.heardRisk(e.risk, e.band);
    }
  }

  private setState(state: ReceiverState) {
    if (this.state === state) return;
    this.state = state;
    this.options.onState?.(state);
  }

  private same(a: Heard, b: Heard) {
    return a.kind === 'payload' ? b.kind === 'payload' && a.payload === b.payload : b.kind === 'reference' && a.fingerprint === b.fingerprint;
  }

  private heardFrame(h: Heard) {
    const sec = this.security;
    if (this.state === 'listening') {
      this.first = h;
      this.firstAt = this.platform.now();
      this.options.onHeard?.(h.band);
      if (h.kind === 'payload') this.text = h.payload;
      else if (!this.options.resolve) return this.fail('NO_RESOLVER', 'A reference was heard and there is no resolver');
      else this.resolved = this.resolve(h.fingerprint);
      if (!sec.detectMultipleEmitters) return this.deliverWhenReady();
      this.setState('final-listen');
      this.finalEnd = this.firstAt + FINAL_LISTEN_SECONDS;
      this.cancelFinal = this.s.after(FINAL_LISTEN_SECONDS, () => this.deliverWhenReady());
      return;
    }
    if (!this.first || this.same(this.first, h)) return; // failed before any frame, or the sender repeating
    if (this.state === 'final-listen') return this.fail('MULTIPLE_TRANSMISSIONS', 'Two different transmissions were heard', { risk: { reason: 'other', intruder: shortOf(h), avoid: h.band } });
    if (this.state === 'delivered' && this.watching) this.conflictWith('MULTIPLE_TRANSMISSIONS', 'Another transmission appeared before confirming', 'other', shortOf(h), h.band);
  }

  /** A frame starting on another band during the final listen: wait for it, up to FINAL_LISTEN_MAX_SECONDS. */
  private heardHeader(band: BandId) {
    if (this.state !== 'final-listen' || band === this.first!.band) return;
    const end = Math.min(Math.max(this.finalEnd, this.platform.now() + FINAL_EXTEND_SECONDS), this.firstAt + FINAL_LISTEN_MAX_SECONDS);
    if (end <= this.finalEnd) return;
    this.finalEnd = end;
    this.cancelFinal?.();
    this.cancelFinal = this.s.after(end - this.platform.now(), () => this.deliverWhenReady());
  }

  private heardClash() {
    if (!this.security.detectMultipleEmitters || (this.state !== 'listening' && this.state !== 'final-listen')) return;
    this.fail('MULTIPLE_TRANSMISSIONS', 'Two transmissions overlapped', { risk: { reason: 'overlap', intruder: UNKNOWN_SHORT, avoid: this.first?.band ?? null } });
  }

  private heardAck(receipt: boolean, id: string) {
    if (receipt && this.acking && this.text !== null && id === ackId(this.text)) this.endAcks(true);
  }

  private heardRisk(r: RiskFrame, band: BandId) {
    const cfg = this.security.riskSignal;
    const key = riskKey(r), now = this.platform.now();
    if (!cfg.listen || this.seen.has(key) || this.memory.isOwn(key, now, cfg.quarantineMs / 1000)) return;
    const before = this.state === 'listening' || this.state === 'final-listen';
    const watchingRisk = this.state === 'delivered' && this.watching && cfg.listenUntil === 'confirm';
    if (!before && !watchingRisk) return;
    this.seen.add(key);
    const mine = this.first ? shortOf(this.first) : null;
    const alert: Alert = { direction: 'heard', ...r, bands: [band], matchesMine: r.own === mine, relay: false };
    this.options.onAlert?.(alert);
    this.quarantine(r);
    if (!cfg.honor) return;
    // The receiver never relays: the sender that hears it does.
    if (before) this.fail('RISK_SIGNAL', 'A risk signal was heard', { heard: alert });
    else this.conflictWith('RISK_SIGNAL', 'A risk signal was heard before confirming', null, '', band, alert);
  }

  private async resolve(fp: string): Promise<void> {
    let timer = () => {};
    try {
      const timeout = new Promise<'timeout'>((r) => { timer = this.s.after(RESOLVE_TIMEOUT_SECONDS, () => r('timeout')); });
      const text = await Promise.race([Promise.resolve().then(() => this.options.resolve!(fp)), timeout]);
      if (text === 'timeout') return this.fail('TIMEOUT', 'The resolver did not answer in time');
      if (text === null || text === undefined) return this.fail('NOT_FOUND', `Unknown fingerprint ${fp}`);
      if (!payloadId(text).startsWith(fp)) return this.fail('MISMATCH', 'The resolved text does not match the fingerprint');
      this.text = text;
    } catch (e) {
      this.fail('NOT_FOUND', 'The resolver failed', { cause: e });
    } finally {
      timer();
    }
  }

  private async deliverWhenReady() {
    this.cancelFinal = null;
    if (this.state !== 'listening' && this.state !== 'final-listen') return;
    if (this.resolved) await this.resolved;
    if ((this.state !== 'listening' && this.state !== 'final-listen') || this.text === null) return;
    const h = this.first!;
    this.received = { text: this.text, band: h.band, via: h.kind };
    this.s.clear(); // the listening timeout
    this.setState('delivered');
    this.settle.resolve(this.received);
    const sec = this.security;
    if (sec.confirmWatch) {
      this.watching = true;
      this.watchStart = this.platform.now();
      this.s.after(sec.confirmWatchSeconds, () => {
        if (!this.watching) return;
        this.watchExpired = true;
        this.stopWatch();
        this.maybeFinished();
      });
    }
    if (this.options.ack ?? true) this.startAcks(h);
    else this.settleReceipt(false);
    this.maybeFinished();
  }

  /** Ack runs and receipt listens, alternating, until the receipt or ACK_TOTAL_SECONDS. */
  private startAcks(h: Heard) {
    this.acking = true;
    const rate = this.platform.sampleRate;
    // Timed with the longest repetition the sender may be playing now (it may have hopped or gone robust since this one).
    const run = ackRunSeconds(longestRepetitionSeconds(h.kind === 'payload' ? { kind: 'payload', text: h.payload } : { kind: 'reference' }));
    const heard = { band: h.band, profile: h.profile ?? 'normal' } as const;
    const audio = loopFrames([ackFrame({ receipt: false, id: ackId(this.text!), heard })], run, rate, this.options.level ?? 1);
    const until = this.platform.now() + ACK_TOTAL_SECONDS;
    const round = () => {
      if (!this.acking) return;
      if (this.platform.now() >= until) return this.endAcks(false);
      this.stopAcks = this.s.play(audio, () => { this.stopAcks = null; this.s.after(RECEIPT_LISTEN_SECONDS, round); });
    };
    round();
  }

  private endAcks(receipt: boolean) {
    if (!this.acking) { if (!receipt) this.settleReceipt(false); return; }
    this.acking = false;
    this.stopAcks?.();
    this.stopAcks = null;
    this.settleReceipt(receipt);
    this.maybeFinished();
  }

  private stopWatch() {
    if (!this.watching) return;
    this.watching = false;
    this.watchEnd = this.platform.now();
  }

  /** Something showed up during the confirm watch: the gate closes for good. A different transmission also gets a risk frame. */
  private conflictWith(code: SessionErrorCode, message: string, reason: RiskReason | null, intruder: string, avoid: BandId, heard?: Alert) {
    this.stopWatch();
    const alert = reason ? this.emitRisk(reason, intruder, avoid) : heard; // before endAcks: `finished` must wait for the risk frame
    this.endAcks(false);
    this.conflict = new SessionError(code, message, alert);
    this.maybeFinished();
  }

  /** Before delivery: the result fails. `risk`: also loop a risk frame (emit 'auto'). */
  private fail(code: SessionErrorCode, message: string, o: { risk?: { reason: RiskReason; intruder: string; avoid: BandId | null }; heard?: Alert; cause?: unknown } = {}) {
    if (this.state !== 'listening' && this.state !== 'final-listen') return;
    this.s.clear();
    const alert = o.risk ? this.emitRisk(o.risk.reason, o.risk.intruder, o.risk.avoid) : o.heard;
    this.setState('failed');
    this.settle.reject(new SessionError(code, message, alert, o.cause === undefined ? undefined : { cause: o.cause }));
    this.settleReceipt(false);
    this.maybeFinished();
  }

  private emitRisk(reason: RiskReason, intruder: string, avoid: BandId | null): Alert | undefined {
    const cfg = this.security.riskSignal;
    if (cfg.emit !== 'auto') return undefined;
    const r: RiskFrame = { reason, source: 'receiver', hops: 0, own: this.first ? shortOf(this.first) : UNKNOWN_SHORT, intruder };
    this.memory.note(riskKey(r), this.platform.now());
    const work = this.first?.band ?? avoid ?? 'top';
    const { bands, audio } = riskAudio(r, work, avoid, cfg.durationMs / 1000, this.platform.sampleRate, this.options.level ?? 1);
    const alert: Alert = { direction: 'emitted', ...r, bands, matchesMine: true, relay: false };
    this.options.onAlert?.(alert);
    this.quarantine(r);
    this.alerting = true;
    this.s.play(audio, () => { this.alerting = false; this.maybeFinished(); });
    return alert;
  }

  private quarantine(r: RiskFrame) {
    const ms = this.security.riskSignal.quarantineMs;
    if (ms > 0) this.options.onQuarantine?.({ durationMs: ms, reason: r.reason, source: r.source });
  }

  private maybeFinished() {
    const listening = this.state === 'listening' || this.state === 'final-listen' || this.watching || this.acking;
    if (!listening && !this.alerting) {
      this.s.clear();
      this.resolveFinished();
    }
  }

  /** Short id of what was received, for apps that log alerts. */
  get short() { return this.text !== null ? shortId(this.text) : null; }
}
