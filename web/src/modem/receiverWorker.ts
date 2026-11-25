// Analysis off the UI thread: the energies of 160 tones every 4 ms (FFT at 48 kHz, Goertzel at 44.1 kHz; see receiver.ts).
import { ReceiverHost, type AnalyzerEvents } from './host';

const scope = self as unknown as { onmessage: ((e: MessageEvent) => void) | null; postMessage(m: unknown): void };
let host: ReceiverHost | null = null;

const forward = (name: keyof AnalyzerEvents) => (...args: unknown[]) => scope.postMessage({ name, args });

scope.onmessage = (e) => {
  const m = e.data as { rate?: number; samples?: Float32Array };
  if (m.rate) {
    host = new ReceiverHost(m.rate, {
      onLevel: forward('onLevel'), onHeader: forward('onHeader'), onBlocks: forward('onBlocks'), onClash: forward('onClash'), onFrame: forward('onFrame'),
    });
  } else if (m.samples) host?.feed(m.samples);
};
