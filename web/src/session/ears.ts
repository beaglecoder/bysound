// What the session layer hears and what it needs from the device. Both are interfaces so the state machines (sender.ts, receiver.ts)
// run the same on Web Audio (browser.ts) and on simulated air (testAir.ts).
import { ReceiverHost } from '../modem/host';
import type { BandId } from '../modem/protocol';
import type { Heard } from '../modem/receiver';
import { ControlListener, type BandNoise } from './controlListener';
import type { AckFrame, RiskFrame } from './frames';

/** Everything the session layer reacts to. */
export type AirEvent =
  /** A payload or reference frame, fully decoded. */
  | { type: 'frame'; heard: Heard }
  /** A payload or reference header read with near certainty: a frame is starting on `band`. */
  | { type: 'header'; band: BandId }
  /** The modem saw two overlapping frames and delivered neither. */
  | { type: 'clash' }
  | { type: 'ack'; ack: AckFrame; band: BandId }
  | { type: 'risk'; risk: RiskFrame; band: BandId }
  /** Every NOISE_REPORT_SECONDS: what each band measures (signal included), for the sender's ambient check. */
  | { type: 'noise'; noise: BandNoise };

/** The device, as the session layer sees it. Times are seconds on one monotonic clock. */
export interface Platform {
  readonly sampleRate: number;
  now(): number;
  /** Calls `fn` once after `seconds`. Returns a function that cancels it. */
  after(seconds: number, fn: () => void): () => void;
  /** Plays `audio` from now. `onEnd` runs when it finished on its own (not after stop). Returns stop. */
  play(audio: Float32Array, onEnd: () => void): () => void;
  /** Uniform in [0, 1). Injected so tests are reproducible. */
  random(): number;
}

/**
 * Microphone samples in, AirEvents out: the modem receiver for payload and reference frames, plus ControlListener for acks and
 * risk frames.
 *
 * When the modem receiver reports ack and risk frames itself (docs/SESSION.md §11), ControlListener goes away: forward its new
 * event here instead and drop the `control` field.
 */
export class Ears {
  private host: ReceiverHost;
  private control: ControlListener;

  constructor(rate: number, onEvent: (e: AirEvent) => void, { robust = true }: { robust?: boolean } = {}) {
    this.host = new ReceiverHost(rate, {
      onFrame: (heard) => onEvent({ type: 'frame', heard }),
      onHeader: (band) => onEvent({ type: 'header', band }),
      onClash: () => onEvent({ type: 'clash' }),
    }, robust);
    this.control = new ControlListener(rate, (c) => onEvent(c.kind === 'ack' ? { type: 'ack', ack: c.ack, band: c.band } : { type: 'risk', risk: c.risk, band: c.band }),
      (noise) => onEvent({ type: 'noise', noise }));
  }

  feed(x: Float32Array) {
    this.control.feed(x);
    this.host.feed(x);
  }
}
