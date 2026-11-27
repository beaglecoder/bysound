// The session layer on Web Audio: a speaker (AudioContext) and the microphone (listen() from ../audio), wired to the state machines.
import { FADE_SECONDS, listen, newContext, resumed, SoundError, type Listening } from '../audio';
import { localStorageCalibrationStore } from './adapt';
import { ControlListener } from './controlListener';
import type { AirEvent, Platform } from './ears';
import { ReceiverSession, type Confirmation, type Received, type ReceiverOptions, type ReceiverState } from './receiver';
import { SenderSession, type Delivered, type SenderOptions, type SenderState } from './sender';

/** Speaker and clock of the page. Create it inside a user gesture (click / tap). */
function webPlatform(): { platform: Platform; ready: Promise<void>; close(): void } {
  const ctx = newContext();
  const ready = resumed(ctx).then(() => {
    if (ctx.state !== 'running') throw new SoundError('UNSUPPORTED', 'The browser did not start audio: tap a button first');
  });
  const platform: Platform = {
    sampleRate: ctx.sampleRate,
    now: () => performance.now() / 1000,
    after(seconds, fn) {
      const id = setTimeout(fn, seconds * 1000);
      return () => clearTimeout(id);
    },
    play(audio, onEnd) {
      const buffer = ctx.createBuffer(1, audio.length, ctx.sampleRate);
      buffer.getChannelData(0).set(audio);
      const source = ctx.createBufferSource();
      const gain = ctx.createGain();
      source.buffer = buffer;
      source.connect(gain).connect(ctx.destination);
      let stopped = false;
      source.onended = () => { if (!stopped) onEnd(); };
      source.start();
      return () => {
        if (stopped) return;
        stopped = true;
        const t = ctx.currentTime;
        gain.gain.setValueAtTime(gain.gain.value, t);
        gain.gain.linearRampToValueAtTime(0, t + FADE_SECONDS); // an abrupt stop of a loud high tone clicks
        try { source.stop(t + FADE_SECONDS + 0.01); } catch { /* already stopped */ }
      };
    },
    random: Math.random,
  };
  return { platform, ready, close: () => { void ctx.close().catch(() => undefined); } };
}

/** The microphone as AirEvents: the modem receiver (in a Worker) plus the ack / risk listener (on this thread). */
async function webEars(hear: (e: AirEvent) => void, onLevel?: (rms: number) => void): Promise<Listening> {
  let control: ControlListener | null = null;
  const mic = await listen({
    ...(onLevel ? { onLevel: (e: { rms: number }) => onLevel(e.rms) } : {}),
    onFrame: (heard) => hear({ type: 'frame', heard }),
    onHeader: (band) => hear({ type: 'header', band }),
    onClash: () => hear({ type: 'clash' }),
    onSamples: (x) => control?.feed(x), // the first block or two arrive before the rate is known: nothing is lost that matters
  });
  control = new ControlListener(mic.sampleRate, (c) => hear(c.kind === 'ack' ? { type: 'ack', ack: c.ack, band: c.band } : { type: 'risk', risk: c.risk, band: c.band }),
    (noise) => hear({ type: 'noise', noise }));
  return mic;
}

export interface SendSession {
  /** Resolves when the receiver's ack arrives; fails with a SessionError otherwise (see docs/SESSION.md §8). */
  delivered: Promise<Delivered>;
  /** Resolves when nothing plays any more and the microphone is released. */
  finished: Promise<void>;
  readonly state: SenderState;
  /** Stops at once (also a risk frame being played). */
  stop(): void;
}

/**
 * Sends `text` as a session: listens first (other emitters, noise per channel), repeats it with listen windows down a channel ladder
 * until the receiver acknowledges, remembers where it was read (localStorage) and watches for other emitters. Call it from a user
 * gesture. Needs the microphone (HTTPS or localhost). Rejects with a RangeError for an empty text, a TooLongError if the text does
 * not fit one transmission, and a SoundError if the browser refuses audio or the microphone.
 */
export async function sendSession(text: string, options: SenderOptions = {}): Promise<SendSession> {
  const speaker = webPlatform();
  let session: SenderSession;
  try {
    // The calibration survives reloads: the speaker of this device does not change.
    session = new SenderSession(text, speaker.platform, { ...options, calibration: options.calibration === undefined ? localStorageCalibrationStore() : options.calibration });
  } catch (e) {
    speaker.close(); // empty or too long text, unknown security level
    throw e;
  }
  // Both audio contexts are created now, inside the user gesture: listen() makes its own before its first await.
  const opening = webEars((e) => session.hear(e));
  let mic: Listening | null = null;
  try {
    await speaker.ready;
    mic = await opening;
    const opened = mic;
    session.start();
    const finished = session.finished.then(() => { opened.stop(); speaker.close(); });
    return { delivered: session.delivered, finished, get state() { return session.state; }, stop: () => session.stop() };
  } catch (e) {
    (mic ?? (await opening.catch(() => null)))?.stop();
    speaker.close();
    throw e;
  }
}

export interface ReceiveSession {
  /** The data, after the final listen. Fails with a SessionError (MULTIPLE_TRANSMISSIONS, RISK_SIGNAL, TIMEOUT…). */
  result: Promise<Received>;
  /** The confirm gate: call it before acting on the data. Fails if another emitter or a risk frame appeared meanwhile. */
  confirmed(): Promise<Confirmation>;
  /** The person left: stop listening and acking. Never throws. */
  cancel(): void;
  /** true if the sender answered the ack with a receipt. */
  receipt: Promise<boolean>;
  finished: Promise<void>;
  readonly state: ReceiverState;
}

/** Listens for one session: delivers the data, acknowledges it by sound and keeps the confirm gate. Call it from a user gesture. */
export async function receiveSession(options: ReceiverOptions & { onLevel?(rms: number): void } = {}): Promise<ReceiveSession> {
  const speaker = webPlatform();
  let session: ReceiverSession;
  try {
    session = new ReceiverSession(speaker.platform, options);
  } catch (e) {
    speaker.close(); // unknown security level
    throw e;
  }
  // Both audio contexts are created now, inside the user gesture: listen() makes its own before its first await.
  const opening = webEars((e) => session.hear(e), options.onLevel);
  let mic: Listening | null = null;
  try {
    await speaker.ready;
    mic = await opening;
    const opened = mic;
    session.start();
    const finished = session.finished.then(() => { opened.stop(); speaker.close(); });
    return {
      result: session.result, receipt: session.receipt, finished,
      confirmed: () => session.confirmed(),
      cancel: () => session.cancel(),
      get state() { return session.state; },
    };
  } catch (e) {
    (mic ?? (await opening.catch(() => null)))?.stop();
    speaker.close();
    throw e;
  }
}
