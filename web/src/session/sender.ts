// Session sender (docs/SESSION.md §5, §9): pre-listen (other emitters + ambient), repetitions with listen windows over a channel ladder
// that adapts (noisy channels skipped, robust after a while, calibration remembered), ack → receipts, other emitters → risk frame,
// heard risk frames → stop and relay. A plain state machine over Platform and AirEvents: no Web Audio here.
import { maxPayloadBytes, pack, TooLongError } from '../modem/codec';
import { hasRobust, type BandId, type Profile } from '../modem/protocol';
import {
  RobustLadder, lapped, longestRepetitionSeconds, plan, readAmbient, sharedCalibrationStore, DEFAULT_CHANNELS,
  type Ambient, type Calibration, type CalibrationStore, type Plan,
} from './adapt';
import {
  CLONE_GUARD_SECONDS, LISTEN_WINDOW_SECONDS, RiskMemory, Scheduler, SessionError, loopFrames, receiptSeconds, renderRepetition, riskAudio, sharedRiskMemory, shortOf,
  type Alert, type QuarantineNotice, type SessionErrorCode,
} from './common';
import type { BandNoise } from './controlListener';
import type { AirEvent, Platform } from './ears';
import { RISK_MAX_HOPS, ackFrame, ackId, fingerprint, riskKey, shortId, type AckFrame, type RiskFrame, type RiskReason } from './frames';
import { resolveSecurity, type Security, type SecurityLevel, type SecurityOverrides } from './security';

export interface SenderOptions {
  /** A fixed band: no ladder, no calibration. */
  band?: BandId;
  /** The ladder, from the least audible band down. Default top, high, upper, mid. */
  channels?: BandId[];
  /** A fixed profile for the data frames. Without it the sender goes robust by itself (§9.3). Acks and risk frames are always normal. */
  profile?: Profile;
  /** 'payload' sends the text; 'reference' sends only its 12-byte fingerprint (the receiver resolves it locally). Default 'payload'. */
  mode?: 'payload' | 'reference';
  /** Output level 0..1 of everything this side plays. Default 1. */
  level?: number;
  /** Gives up this long after the first frame if no ack arrived. Default 60. */
  timeoutSeconds?: number;
  /** Measure the noise of each channel in the pre-listen and skip the noisy ones. Default true. */
  ambient?: boolean;
  /** Where the calibration is kept; null: none. Default: in memory, shared by the page (the browser API uses localStorage). */
  calibration?: CalibrationStore | null;
  /** Default 'maximum'. */
  securityLevel?: SecurityLevel;
  /** Explicit values that win over the level. */
  security?: SecurityOverrides;
  /** Risk incidents this device already emitted. Default: one shared by every session of the page. */
  memory?: RiskMemory;
  onAlert?(alert: Alert): void;
  onQuarantine?(notice: QuarantineNotice): void;
  onState?(state: SenderState): void;
  /** Each repetition as it starts. */
  onRepetition?(r: { index: number; band: BandId; profile: Profile }): void;
  /** What the pre-listen measured (null without noise reports). */
  onAmbient?(a: Ambient | null, route: BandId[]): void;
}

export type SenderState = 'pre-listen' | 'sending' | 'window' | 'receipt' | 'alerting' | 'done';
export interface Delivered {
  /** Where the receiver read it, from the ack; without that in the ack, the band of the last repetition. */
  band: BandId;
  profile: Profile;
  /** The ack said where it was read. */
  stated: boolean;
  /** From the first frame to the ack. */
  seconds: number;
  repetitions: number;
  /** The calibration saved for next time (null: nothing learned). */
  calibration: Calibration | null;
}

/**
 * The sender state machine (docs/SESSION.md §5). Platform-independent: `sendSession` wires it to Web Audio; tests drive it on simulated air.
 * Call start(), feed it what the microphone hears with hear(), and watch `delivered` and `finished`.
 */
export class SenderSession {
  state: SenderState = 'pre-listen';
  readonly delivered: Promise<Delivered>;
  /** Resolves when the session is over and plays nothing more (after receipts or the risk frame). */
  readonly finished: Promise<void>;
  readonly security: Security;
  private readonly mode: 'payload' | 'reference';
  private level: number;
  private readonly memory: RiskMemory;
  private readonly store: CalibrationStore | null;
  private readonly own: { short: string; ack: string; fingerprint: string };
  /** Packed length of the text (payload mode): decides whether a repetition fits the robust profile. */
  private readonly packed: number;
  private readonly audio = new Map<string, Float32Array>();
  private readonly s: Scheduler;
  private resolveDelivered!: (d: Delivered) => void;
  private rejectDelivered!: (e: SessionError) => void;
  private resolveFinished!: () => void;
  private pending = true;
  private plan: Plan = { route: [DEFAULT_CHANNELS[0]!], every: Infinity, calibration: null, startRobust: null };
  private ladder = new RobustLadder(false, false);
  private ambient: Ambient | null = null;
  private noise: BandNoise[] = [];
  private hop = 0;
  private onChannel = 0;
  private sent = 0;
  private played: BandId[] = [];
  private band: BandId;
  private profile: Profile = 'normal';
  private playing = false;
  private lastEnd = -Infinity;
  private firstAt = 0;
  private stopPayload: (() => void) | null = null;
  private seen = new Set<string>();
  /** Pre-listen heard a frame start or a clash. */
  private busy = false;

  /**
   * Throws a RangeError for an empty text, and a TooLongError (payload mode) if the text does not fit one transmission on a band
   * the session may play (the fixed band, or every channel of the ladder) in the normal profile, or in the fixed profile if given.
   */
  constructor(private readonly text: string, private readonly platform: Platform, private readonly options: SenderOptions = {}) {
    if (!text) throw new RangeError('Nothing to send: the text is empty');
    this.security = resolveSecurity(options.securityLevel, options.security);
    this.mode = options.mode ?? 'payload';
    this.level = options.level ?? 1;
    this.memory = options.memory ?? sharedRiskMemory;
    this.store = options.calibration === undefined ? sharedCalibrationStore : options.calibration;
    this.own = { short: shortId(text), ack: ackId(text), fingerprint: fingerprint(text) };
    const bands = options.band ? [options.band] : options.channels?.length ? options.channels : DEFAULT_CHANNELS;
    this.band = bands[0]!;
    this.packed = this.mode === 'payload' ? pack(text).length : 0;
    if (this.mode === 'payload') {
      for (const b of bands) {
        const p: Profile = options.profile === 'robust' && hasRobust(b) ? 'robust' : 'normal';
        const max = maxPayloadBytes(b, p);
        if (this.packed > max) throw new TooLongError(`The data is too long for one transmission on ${b}/${p}: ${this.packed} packed bytes, at most ${max}`);
      }
    }
    this.s = new Scheduler(platform);
    this.delivered = new Promise((resolve, reject) => { this.resolveDelivered = resolve; this.rejectDelivered = reject; });
    this.delivered.catch(() => undefined); // the app may only look at finished
    this.finished = new Promise((resolve) => { this.resolveFinished = resolve; });
  }

  start() {
    const sec = this.security;
    const detect = sec.detectMultipleEmitters;
    if (sec.listenBeforeSendingSeconds > 0 && (detect || (this.options.ambient ?? true))) {
      this.setState('pre-listen');
      // The jitter only matters for detection: two senders starting together must not end their pre-listen together.
      this.s.after(sec.listenBeforeSendingSeconds + (detect ? this.platform.random() * sec.scanJitterSeconds : 0), () => {
        if (this.busy) this.refuse('OTHER_TRANSMISSION', 'Another transmission is already sounding');
        else this.beginSending();
      });
    } else this.beginSending();
  }

  /** Stops everything at once, the risk frame included. Before the ack, `delivered` fails with STOPPED. */
  stop() {
    this.fail('STOPPED', 'Stopped by the app');
    this.end();
  }

  hear(e: AirEvent) {
    if (this.state === 'done') return;
    if (e.type === 'noise') { if (this.state === 'pre-listen') this.noise.push(e.noise); return; }
    if (e.type === 'risk') return this.heardRisk(e.risk, e.band);
    if (e.type === 'ack') {
      if ((this.state === 'sending' || this.state === 'window') && !e.ack.receipt && e.ack.id === this.own.ack) this.acked(e.ack);
      return;
    }
    const sec = this.security;
    if (!sec.detectMultipleEmitters) return;
    if (this.state === 'pre-listen') {
      // A decoded frame refuses at once; a frame start or a clash refuses at the end of the pre-listen, unless the frame is decoded
      // by then (so a replay of our own data is reported as a clone). Acks and risk frames are handled above.
      if (e.type !== 'frame') this.busy = true;
      else if (this.isOwn(e.heard)) this.refuse('CLONED_TRANSMISSION', 'Someone else is already playing this data');
      else this.refuse('OTHER_TRANSMISSION', 'Another transmission is already sounding');
      return;
    }
    if ((this.state !== 'sending' && this.state !== 'window') || !sec.listenWhileSending || e.type !== 'frame') return;
    const h = e.heard;
    if (!this.isOwn(h)) return this.detected('other', shortOf(h), h.band);
    // Our own data while we have been silent for a while, or on a band we are not playing: someone else is playing it.
    const silent = !this.playing && this.platform.now() - this.lastEnd >= CLONE_GUARD_SECONDS;
    if (silent || (this.playing && h.band !== this.band)) this.detected('clone', this.own.short, h.band);
  }

  private isOwn(h: Extract<AirEvent, { type: 'frame' }>['heard']) {
    return h.kind === 'payload' ? h.payload === this.text : h.fingerprint === this.own.fingerprint;
  }

  private setState(state: SenderState) {
    if (this.state === state) return;
    this.state = state;
    this.options.onState?.(state);
  }

  private beginSending() {
    const o = this.options;
    const channels = o.channels?.length ? o.channels : DEFAULT_CHANNELS;
    this.ambient = (o.ambient ?? true) ? readAmbient(this.noise, channels) : null;
    this.plan = plan({ ...(o.band ? { band: o.band } : {}), channels, ...(o.profile ? { profile: o.profile } : {}), calibration: this.store?.load() ?? null, ambient: this.ambient });
    if (this.plan.calibration) this.store?.save({ ...this.plan.calibration, uses: this.plan.calibration.uses + 1 });
    if (this.ambient?.loud && o.level === undefined) this.level = 1;
    const robustStart = this.plan.startRobust !== null || o.profile === 'robust';
    this.ladder = new RobustLadder(o.profile === undefined, robustStart, this.plan.startRobust === 'noise' ? 'noise' : null);
    o.onAmbient?.(this.ambient, this.plan.route);
    this.firstAt = this.platform.now();
    this.s.after(o.timeoutSeconds ?? 60, () => {
      if (this.state !== 'sending' && this.state !== 'window') return;
      this.fail('TIMEOUT', 'No ack arrived');
      this.end();
    });
    this.playRepetition();
  }

  /** Next repetition of the ladder: `every` repetitions per channel, then the next channel of the route, cycling. */
  private next(): { band: BandId; profile: Profile; set: number } {
    const route = this.plan.route;
    if (this.onChannel >= this.plan.every && route.length > 1) {
      this.onChannel = 0;
      this.hop = (this.hop + 1) % route.length;
    }
    this.onChannel++;
    const band = route[this.hop]!;
    let profile = this.ladder.next(band);
    // A text that fits the normal profile may not fit the robust one (twice the airtime): the normal profile is played then.
    if (profile === 'robust' && this.mode === 'payload' && this.packed > maxPayloadBytes(band, 'robust')) profile = 'normal';
    return { band, profile, set: this.mode === 'reference' ? this.sent % 2 : 0 };
  }

  private render(band: BandId, profile: Profile, set: number) {
    const key = `${band}/${profile}/${set}/${this.level}`;
    let a = this.audio.get(key);
    if (!a) this.audio.set(key, (a = renderRepetition(this.text, this.mode, band, this.platform.sampleRate, this.level, set, profile)));
    return a;
  }

  private playRepetition() {
    const r = this.next();
    this.band = r.band;
    this.profile = r.profile;
    this.played.push(r.band);
    this.options.onRepetition?.({ index: this.sent, band: r.band, profile: r.profile });
    this.sent++;
    this.setState('sending');
    this.playing = true;
    this.stopPayload = this.s.play(this.render(r.band, r.profile, r.set), () => {
      this.playing = false;
      this.stopPayload = null;
      this.lastEnd = this.platform.now();
      this.setState('window');
      this.s.after(LISTEN_WINDOW_SECONDS, () => { if (this.state === 'window') this.playRepetition(); });
    });
  }

  private silence() {
    this.stopPayload?.();
    this.stopPayload = null;
    this.playing = false;
  }

  private acked(ack: AckFrame) {
    this.silence();
    this.pending = false;
    const heard = ack.heard ?? { band: this.band, profile: this.profile };
    const calibration = this.learn(ack.heard);
    this.resolveDelivered({ band: heard.band, profile: heard.profile, stated: !!ack.heard, seconds: this.platform.now() - this.firstAt, repetitions: this.sent, calibration });
    this.setState('receipt');
    const receipt = ackFrame({ receipt: true, id: this.own.ack, ...(ack.heard ? { heard: ack.heard } : {}) });
    const longest = longestRepetitionSeconds(this.mode === 'payload' ? { kind: 'payload', text: this.text } : { kind: 'reference' });
    this.s.play(loopFrames([receipt], receiptSeconds(longest), this.platform.sampleRate, this.level), () => this.end());
  }

  /**
   * Calibration (§9.4): remember where the receiver read us, unless the band was fixed, the ack did not say, it was audible, the send
   * went all the way round the route first (lapped), or the ambient moved us off a calibrated band that was noisy here (kept).
   * A robust profile reached by the robust ladder belongs to this place, not to the speaker: the normal profile is saved.
   */
  private learn(heard: AckFrame['heard']): Calibration | null {
    if (!this.store || this.options.band || !heard || heard.band === 'audible') return null;
    if (lapped(this.played, this.plan.route.length)) return null;
    const cal = this.plan.calibration, a = this.ambient;
    if (cal && heard.band !== cal.band && a && (a.loud || a.noisy.includes(cal.band))) return null;
    const profile: Profile = heard.profile === 'robust' && this.ladder.reason !== null ? 'normal' : heard.profile;
    const next: Calibration = { band: heard.band, profile, uses: 0 };
    this.store.save(next);
    return next;
  }

  /** Pre-listen found something: nothing was sent, so there is nothing to warn about. */
  private refuse(code: SessionErrorCode, message: string) {
    this.fail(code, message);
    this.end();
  }

  /** Another emitter while sending: stop, and (emit 'auto') loop a risk frame so the receiver and others stop too. */
  private detected(reason: RiskReason, intruder: string, avoid: BandId) {
    this.silence();
    const code: SessionErrorCode = reason === 'clone' ? 'CLONED_TRANSMISSION' : 'OTHER_EMITTER';
    const message = reason === 'clone' ? 'Someone else is playing this data' : 'Another transmission was heard while sending';
    if (this.security.riskSignal.emit !== 'auto') {
      this.fail(code, message);
      return this.end();
    }
    const alert = this.emitRisk({ reason, source: 'sender', hops: 0, own: this.own.short, intruder }, avoid, false);
    this.fail(code, message, alert);
  }

  private heardRisk(r: RiskFrame, band: BandId) {
    const cfg = this.security.riskSignal;
    const key = riskKey(r), now = this.platform.now();
    if (!cfg.listen || this.state === 'alerting' || this.seen.has(key) || this.memory.isOwn(key, now, cfg.quarantineMs / 1000)) return;
    this.seen.add(key);
    const alert: Alert = { direction: 'heard', ...r, bands: [band], matchesMine: r.own === this.own.short, relay: false };
    this.options.onAlert?.(alert);
    this.quarantine(r);
    if (!cfg.honor) return;
    this.silence();
    // Any honored risk frame stops this sender, whoever sent it and whatever it reports. It is relayed once, one hop further.
    if (r.hops < RISK_MAX_HOPS) this.emitRisk({ ...r, hops: r.hops + 1 }, band, true);
    else this.end();
    this.fail('RISK_SIGNAL', 'A risk signal was heard', alert);
  }

  private emitRisk(r: RiskFrame, avoid: BandId, relay: boolean): Alert {
    const cfg = this.security.riskSignal;
    this.s.clear(); // stops receipts and timers; the payload was already silenced
    this.memory.note(riskKey(r), this.platform.now());
    const { bands, audio } = riskAudio(r, this.band, avoid, cfg.detectorDurationMs / 1000, this.platform.sampleRate, this.level);
    const alert: Alert = { direction: 'emitted', ...r, bands, matchesMine: true, relay };
    this.options.onAlert?.(alert);
    if (!relay) this.quarantine(r);
    this.setState('alerting');
    this.s.play(audio, () => this.end());
    return alert;
  }

  private quarantine(r: RiskFrame) {
    const ms = this.security.riskSignal.quarantineMs;
    if (ms > 0) this.options.onQuarantine?.({ durationMs: ms, reason: r.reason, source: r.source });
  }

  private fail(code: SessionErrorCode, message: string, alert?: Alert) {
    if (!this.pending) return;
    this.pending = false;
    this.rejectDelivered(new SessionError(code, message, alert));
  }

  private end() {
    this.s.clear();
    this.playing = false;
    this.setState('done');
    this.resolveFinished();
  }
}
