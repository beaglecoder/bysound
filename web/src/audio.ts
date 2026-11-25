// Browser audio for the modem: speaker playback and microphone capture. The modem itself (src/modem) never touches Web Audio.
import { GAP_SECONDS, type BandId, type Profile } from './modem/protocol';
import { payloadFrame } from './modem/codec';
import { render, fitLevel } from './modem/modulator';
import { ReceiverHost, type Analyzer, type AnalyzerEvents, type LevelEvent } from './modem/host';
import type { Heard } from './modem/receiver';

export interface EncodeOptions {
  band?: BandId;
  profile?: Profile;
  /** Output sample rate in Hz. Default 48000. */
  sampleRate?: number;
  /** Linear gain, 0..1. Default 1. */
  level?: number;
}

/** One frame of audio (preamble, header, data) followed by a short gap, as mono Float32 in -1..1. */
export function encode(text: string, { band = 'top', profile = 'normal', sampleRate = 48000, level = 1 }: EncodeOptions = {}): Float32Array {
  const frame = payloadFrame(text, band, profile);
  return fitLevel(render(frame.symbols, frame.band, sampleRate, GAP_SECONDS), level);
}

/** Decodes recorded audio offline (for example a WAV file). Returns every frame heard, in order. */
export function decode(samples: Float32Array, sampleRate: number): Heard[] {
  const heard: Heard[] = [];
  const host = new ReceiverHost(sampleRate, { onFrame: (h) => heard.push(h) });
  const feed = (x: Float32Array) => { for (let i = 0; i < x.length; i += 1024) host.feed(x.subarray(i, Math.min(i + 1024, x.length))); };
  feed(samples);
  feed(new Float32Array(sampleRate)); // one second of silence so a frame that ends the file still closes
  host.close();
  return heard;
}

/** Thrown for anything the browser refuses: no Web Audio, no microphone permission, no user gesture. */
export class SoundError extends Error {
  constructor(public code: 'UNSUPPORTED' | 'MIC_DENIED', message: string, public cause?: unknown) { super(message); }
}

type ContextClass = typeof AudioContext;
function newContext(): AudioContext {
  const Ctx = globalThis.AudioContext ?? (globalThis as unknown as { webkitAudioContext?: ContextClass }).webkitAudioContext;
  if (!Ctx) throw new SoundError('UNSUPPORTED', 'This browser has no Web Audio');
  return new Ctx(); // device sample rate: asking for another one makes the browser resample, which hurts near 20 kHz
}

const FADE_SECONDS = 0.015; // stopping a loud high tone abruptly leaves an audible broadband click; a short fade avoids it
const RESUME_WAIT_MS = 1500;
const resumed = (ctx: AudioContext) => Promise.race([ctx.resume().catch(() => undefined), new Promise((r) => setTimeout(r, RESUME_WAIT_MS))]);

/** Repeats `frame` end to end over the length of `backing` (whole frames only) and adds the two. */
export function mix(frame: Float32Array, backing: Float32Array): Float32Array {
  const out = Float32Array.from(backing);
  for (let at = 0; at + frame.length <= out.length; at += frame.length) for (let i = 0; i < frame.length; i++) out[at + i]! += frame[i]!;
  return out;
}

export interface SendOptions extends EncodeOptions {
  /** How many times to play the frame, or 'forever' until stop(). Default 1. */
  repeat?: number | 'forever';
  /**
   * Music (or any sound, peak <= 0.5) to play underneath. The frame is repeated over it; the music loops.
   * The message stays inaudible on the 'top' band because the music lives far below it.
   */
  backing?: (sampleRate: number) => Float32Array;
}
export interface Sending {
  stop(): void;
  /** Resolves when playback ended (all repeats played, or stop() was called). */
  done: Promise<void>;
}

/**
 * Plays the frame through the speaker, looping it. Call it from a user gesture (click/tap), otherwise browsers keep audio suspended.
 * For best range turn the device volume up and use the speaker, not headphones.
 */
export async function send(text: string, options: SendOptions = {}): Promise<Sending> {
  const ctx = newContext(); // created synchronously, inside the gesture
  const ready = resumed(ctx);
  const audio = options.backing ? mix(encode(text, { ...options, level: 0.5, sampleRate: ctx.sampleRate }), options.backing(ctx.sampleRate)) : encode(text, { ...options, sampleRate: ctx.sampleRate });
  await ready;
  if (ctx.state !== 'running') { void ctx.close(); throw new SoundError('UNSUPPORTED', 'The browser did not start audio: tap a button first'); }
  const repeat = options.repeat ?? 1;
  const buffer = ctx.createBuffer(1, audio.length, ctx.sampleRate);
  buffer.getChannelData(0).set(audio);
  const gain = ctx.createGain();
  const source = ctx.createBufferSource();
  source.buffer = buffer;
  source.loop = repeat !== 1; // the frame already ends with a gap, so looping is seamless
  source.connect(gain).connect(ctx.destination);
  let finished = false;
  let finish!: () => void;
  const done = new Promise<void>((r) => { finish = r; });
  const close = () => { if (finished) return; finished = true; void ctx.close().catch(() => undefined); finish(); };
  source.onended = close;
  const t0 = ctx.currentTime + 0.05;
  source.start(t0);
  if (repeat !== 'forever') source.stop(t0 + buffer.duration * repeat);
  return {
    done,
    stop() {
      if (finished) return;
      const t = ctx.currentTime;
      gain.gain.setValueAtTime(gain.gain.value, t);
      gain.gain.linearRampToValueAtTime(0, t + FADE_SECONDS);
      setTimeout(close, FADE_SECONDS * 1000 + 30);
    },
  };
}

export interface ListenOptions {
  onFrame(heard: Heard): void;
  /** About every 60 ms: RMS level of the input, decoding progress 0..1, etc. */
  onLevel?(level: LevelEvent): void;
  /** A valid frame header was read on that band (the frame is still being received). */
  onHeader?(band: BandId): void;
}
export interface Listening {
  stop(): void;
  sampleRate: number;
}

// Pulls the microphone into 1024-sample chunks on the audio thread.
const WORKLET = `
class SoundTap extends AudioWorkletProcessor {
  constructor() { super(); this.buffer = new Float32Array(1024); this.fill = 0; }
  process(inputs) {
    const channel = inputs[0] && inputs[0][0];
    if (!channel) return true;
    for (let i = 0; i < channel.length; i++) {
      this.buffer[this.fill++] = channel[i];
      if (this.fill === this.buffer.length) {
        this.port.postMessage(this.buffer, [this.buffer.buffer]);
        this.buffer = new Float32Array(1024);
        this.fill = 0;
      }
    }
    return true;
  }
}
registerProcessor('sound-tap', SoundTap);
`;

/** Runs the receiver in a Worker; if the Worker cannot start, falls back to the main thread. */
function analyzer(rate: number, events: AnalyzerEvents): Analyzer {
  let worker: Worker;
  try {
    worker = new Worker(new URL('./modem/receiverWorker.ts', import.meta.url), { type: 'module' });
  } catch {
    return new ReceiverHost(rate, events);
  }
  let fallback: Analyzer | null = null;
  worker.onmessage = (e) => {
    const { name, args } = e.data as { name: keyof AnalyzerEvents; args: unknown[] };
    (events[name] as ((...a: unknown[]) => void) | undefined)?.(...args);
  };
  worker.onerror = () => { worker.terminate(); fallback = new ReceiverHost(rate, events); };
  worker.postMessage({ rate });
  return {
    feed(x) { if (fallback) fallback.feed(x); else worker.postMessage({ samples: x }, [x.buffer]); },
    close() { worker.terminate(); },
  };
}

/**
 * Opens the microphone (echo cancellation, noise suppression and AGC off: they destroy the tones) and decodes what it hears.
 * Call it from a user gesture. Needs HTTPS or localhost.
 */
export async function listen({ onFrame, onLevel, onHeader }: ListenOptions): Promise<Listening> {
  const ctx = newContext();
  const fail = (code: SoundError['code'], message: string, cause?: unknown) => { void ctx.close().catch(() => undefined); return new SoundError(code, message, cause); };
  if (!navigator.mediaDevices?.getUserMedia) throw fail('UNSUPPORTED', 'The microphone needs HTTPS (or localhost) and a modern browser');
  if (!ctx.audioWorklet) throw fail('UNSUPPORTED', 'This browser has no AudioWorklet');
  let stream: MediaStream;
  try {
    stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 1 } });
  } catch (e) {
    const name = (e as { name?: string }).name;
    throw fail(name === 'NotAllowedError' || name === 'SecurityError' ? 'MIC_DENIED' : 'UNSUPPORTED', 'Could not open the microphone', e);
  }
  const stopTracks = () => stream.getTracks().forEach((t) => t.stop());
  const url = URL.createObjectURL(new Blob([WORKLET], { type: 'text/javascript' }));
  try {
    await ctx.audioWorklet.addModule(url);
  } catch (e) {
    stopTracks();
    throw fail('UNSUPPORTED', 'Could not load the audio processor', e);
  } finally {
    URL.revokeObjectURL(url);
  }
  await resumed(ctx);
  if (ctx.state !== 'running') { stopTracks(); throw fail('UNSUPPORTED', 'The browser did not start audio: tap a button first'); }
  const analysis = analyzer(ctx.sampleRate, { onFrame, onLevel, onHeader });
  const source = ctx.createMediaStreamSource(stream);
  const tap = new AudioWorkletNode(ctx, 'sound-tap', { numberOfInputs: 1, numberOfOutputs: 1, channelCount: 1 });
  tap.port.onmessage = (e) => analysis.feed(e.data as Float32Array);
  const mute = ctx.createGain(); // Safari only processes nodes that reach the destination: route silently
  mute.gain.value = 0;
  source.connect(tap);
  tap.connect(mute).connect(ctx.destination);
  return {
    sampleRate: ctx.sampleRate,
    stop() {
      tap.port.onmessage = null;
      source.disconnect();
      tap.disconnect();
      stopTracks();
      analysis.close();
      void ctx.close().catch(() => undefined);
    },
  };
}
