// The receiver plus the level meter: runs the same in a Worker or on the main thread.
import type { BandId } from './protocol';
import { Receiver, type Heard } from './receiver';

export interface LevelEvent { rms: number; progress: number; failures: number; nibblesConfidence: number[]; blocksTotal?: number }
export interface AnalyzerEvents {
  onLevel?(e: LevelEvent): void;
  onHeader?(band: BandId): void;
  onBlocks?(closed: number, total: number): void;
  onClash?(): void;
  onFrame?(h: Heard): void;
}
export interface Analyzer { feed(x: Float32Array): void; close(): void }

const LEVEL_SECONDS = 0.06;

/**
 * Listens to both profiles, like the native ports: a normal receiver (all 7 bands) and a robust one (top, high, upper and mid, with double
 * window and step), on the same audio. A native sender switches to robust only with heavy noise or if nobody reads it (see ROBUST_THRESHOLDS). The
 * robust one adds a 12 s ring and a 2,048-point FFT every 8 ms; the level and progress come from the normal one.
 */
export class ReceiverHost implements Analyzer {
  private receiver: Receiver;
  private robust: Receiver | null;
  private every: number;
  private count = 0;
  private energy = 0;

  constructor(rate: number, private events: AnalyzerEvents, robust = true) {
    const forward = {
      onFrame: (h: Heard) => events.onFrame?.(h),
      onHeader: (b: BandId) => events.onHeader?.(b),
      onBlocks: (c: number, t: number) => events.onBlocks?.(c, t),
      onClash: () => events.onClash?.(),
    };
    this.receiver = new Receiver(rate, forward);
    this.robust = robust ? new Receiver(rate, forward, 'robust') : null;
    this.every = Math.round(LEVEL_SECONDS * rate);
  }

  feed(x: Float32Array) {
    this.receiver.feed(x);
    this.robust?.feed(x);
    for (let i = 0; i < x.length; i++) this.energy += x[i]! * x[i]!;
    this.count += x.length;
    if (this.count < this.every) return;
    const r = this.receiver;
    this.events.onLevel?.({ rms: Math.sqrt(this.energy / this.count), progress: r.progress, failures: r.failures, nibblesConfidence: r.nibblesConfidence, blocksTotal: r.blocksTotal });
    this.count = 0;
    this.energy = 0;
  }

  close() { /* nothing to release */ }
}
