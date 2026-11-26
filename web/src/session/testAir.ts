// Simulated air for the session tests: several devices, each with a speaker and a microphone, on one sample clock. What each
// device hears is the sum of what every device plays (scaled by a gain per pair) plus white noise.
import { fftOf } from '../modem/fft';
import { gaussian } from '../modem/testAudio';
import { Ears, type AirEvent, type Platform } from './ears';

interface Sound { from: Device; audio: Float32Array; start: number; onEnd: () => void; stopped: boolean }
interface Timer { due: number; fn: () => void; cancelled: boolean }

export class Device implements Platform {
  ears: Ears | null = null;
  /** What the speaker does to the sound (a broken tweeter, for example). */
  speaker: ((audio: Float32Array) => Float32Array) | null = null;
  readonly heard: AirEvent[] = [];
  private seed: number;
  constructor(readonly air: Air, readonly name: string, seed: number) { this.seed = seed; }
  get sampleRate() { return this.air.rate; }
  now() { return this.air.t / this.air.rate; }
  after(seconds: number, fn: () => void) {
    const t: Timer = { due: this.air.t + Math.max(0, Math.round(seconds * this.air.rate)), fn, cancelled: false };
    this.air.timers.push(t);
    return () => { t.cancelled = true; };
  }
  play(audio: Float32Array, onEnd: () => void) {
    const s: Sound = { from: this, audio: this.speaker ? this.speaker(audio) : audio, start: this.air.t, onEnd, stopped: false };
    this.air.sounds.push(s);
    return () => { s.stopped = true; };
  }
  random() {
    this.seed = (this.seed * 1103515245 + 12345) % 2147483648;
    return this.seed / 2147483648;
  }
  /** Sends what this device's microphone decodes to `hear` (a session's hear, or several). */
  listen(hear: (e: AirEvent) => void, { robust = false }: { robust?: boolean } = {}) {
    this.ears = new Ears(this.air.rate, (e) => { if (e.type !== 'noise') this.heard.push(e); hear(e); }, { robust });
  }
  /** Plays raw audio in a loop for `seconds`: a device that does not follow the session protocol (a rogue sender, a replay). */
  loop(audio: Float32Array, seconds: number) {
    let stopped = false;
    const until = this.now() + seconds;
    const again = () => { if (!stopped && this.now() < until) this.play(audio, again); };
    again();
    return () => { stopped = true; };
  }
}

export class Air {
  t = 0;
  readonly devices: Device[] = [];
  readonly sounds: Sound[] = [];
  readonly timers: Timer[] = [];
  private gains = new Map<string, number>();
  private noise: () => number;

  constructor(readonly rate = 48000, readonly noiseLevel = 0.003, seed = 1, readonly chunk = 1024) {
    this.noise = gaussian(seed);
  }

  device(name: string): Device {
    const d = new Device(this, name, this.devices.length * 7919 + 17);
    this.devices.push(d);
    return d;
  }

  /** How loud `from` is in the microphone of `to` (default 1; 0: out of range). Symmetric unless `oneWay`. */
  gain(from: Device, to: Device, g: number, oneWay = false) {
    this.gains.set(`${from.name}>${to.name}`, g);
    if (!oneWay) this.gains.set(`${to.name}>${from.name}`, g);
  }

  /** Advances the clock by `seconds`, or until `until()` holds (checked every chunk). Yields to pending promises every chunk. */
  async run(seconds: number, until?: () => boolean) {
    const end = this.t + Math.round(seconds * this.rate);
    while (this.t < end && !until?.()) {
      const n = Math.min(this.chunk, end - this.t);
      const from = this.t;
      this.t += n;
      const noise = Float32Array.from({ length: n }, () => this.noise() * this.noiseLevel);
      for (const d of this.devices) {
        if (!d.ears) continue;
        const x = Float32Array.from(noise);
        for (const s of this.sounds) {
          if (s.stopped) continue;
          const g = this.gains.get(`${s.from.name}>${d.name}`) ?? 1;
          if (g === 0) continue;
          for (let i = 0; i < n; i++) {
            const k = from + i - s.start;
            if (k >= 0 && k < s.audio.length) x[i]! += g * s.audio[k]!;
          }
        }
        d.ears.feed(x);
      }
      for (const s of this.sounds.splice(0)) {
        if (s.stopped) continue;
        if (this.t >= s.start + s.audio.length) s.onEnd();
        else this.sounds.push(s);
      }
      for (const t of this.timers.splice(0).sort((a, b) => a.due - b.due)) {
        if (t.cancelled) continue;
        if (t.due <= this.t) t.fn();
        else this.timers.push(t);
      }
      await Promise.resolve();
    }
  }
}

/** A speaker that plays nothing above `cutoffHz` (an ideal low-pass, done on the whole sound at once). */
export function brokenTweeter(cutoffHz: number, rate = 48000) {
  return (audio: Float32Array): Float32Array => {
    let n = 2;
    while (n < audio.length) n <<= 1;
    const f = fftOf(n), re = f.re, im = f.im;
    re.fill(0);
    re.set(audio);
    im.fill(0);
    f.transform();
    const k0 = Math.ceil((cutoffHz * n) / rate);
    for (let k = k0; k <= n - k0; k++) { re[k] = 0; im[k] = 0; }
    for (let k = 0; k < n; k++) im[k] = -im[k]!; // inverse FFT through the conjugate
    f.transform();
    return Float32Array.from({ length: audio.length }, (_, i) => re[i]! / n);
  };
}

/** A steady interferer: equal-power tones on every grid bin from `fromHz` to `toHz` with random phases, for `seconds`. */
export function hiss(fromHz: number, toHz: number, amplitude: number, seconds: number, rate = 48000, seed = 5) {
  const rnd = gaussian(seed);
  const out = new Float32Array(Math.round(seconds * rate));
  for (let hz = fromHz; hz <= toHz; hz += 46.875) {
    const phase = rnd() * Math.PI, w = (2 * Math.PI * hz) / rate;
    for (let i = 0; i < out.length; i++) out[i]! += amplitude * Math.sin(w * i + phase);
  }
  return out;
}
