// Radix-2 FFT shared by the receiver, ambient measurement and the "can be heard" detector. The twiddles of each stage, the permutation
// and the Hann window are computed once per size (with the same arithmetic the loop used to do, so the result does not
// change) and the buffers are reused: no memory is allocated per window.

const plans = new Map<number, Fft>();
const hanns = new Map<number, Float64Array>();

/** The `n`-point FFT (power of 2), built once. Use from a single thread: the `re` and `im` buffers are shared. */
export function fftOf(n: number): Fft {
  let f = plans.get(n);
  if (!f) plans.set(n, (f = new Fft(n)));
  return f;
}

/** Hann window of `n` samples (0.5 − 0.5·cos(2πi/n)), built once. */
export function hannOf(n: number): Float64Array {
  let w = hanns.get(n);
  if (!w) {
    w = new Float64Array(n);
    for (let i = 0; i < n; i++) w[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / n);
    hanns.set(n, w);
  }
  return w;
}

export const isPowerOfTwo = (n: number) => n >= 2 && (n & (n - 1)) === 0;

export class Fft {
  /** Working buffers, for callers that have none of their own. */
  readonly re: Float64Array;
  readonly im: Float64Array;
  /** Pairs (i, j) of the bit-reversal reordering. */
  private swaps: Uint32Array;
  /** Twiddles of each stage (len = 2, 4, …, n), one after the other: stage len starts at len/2 − 1. */
  private cos: Float64Array;
  private sin: Float64Array;

  constructor(readonly n: number) {
    if (!isPowerOfTwo(n)) throw new Error(`${n}-point FFT: must be a power of 2`);
    this.re = new Float64Array(n);
    this.im = new Float64Array(n);
    const swaps: number[] = [];
    for (let i = 1, j = 0; i < n; i++) {
      let bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) swaps.push(i, j);
    }
    this.swaps = Uint32Array.from(swaps);
    this.cos = new Float64Array(n);
    this.sin = new Float64Array(n);
    for (let len = 2; len <= n; len <<= 1) {
      const a = (-2 * Math.PI) / len;
      for (let k = 0; k < len / 2; k++) {
        this.cos[len / 2 - 1 + k] = Math.cos(a * k);
        this.sin[len / 2 - 1 + k] = Math.sin(a * k);
      }
    }
  }

  /** In-place transform of (re, im), of length n. */
  transform(re: Float64Array = this.re, im: Float64Array = this.im) {
    const n = this.n, s = this.swaps, cs = this.cos, sn = this.sin;
    for (let p = 0; p < s.length; p += 2) {
      const i = s[p]!, j = s[p + 1]!;
      const tr = re[i]!; re[i] = re[j]!; re[j] = tr;
      const ti = im[i]!; im[i] = im[j]!; im[j] = ti;
    }
    for (let len = 2; len <= n; len <<= 1) {
      const half = len >> 1, base = half - 1;
      for (let i = 0; i < n; i += len) {
        for (let k = 0; k < half; k++) {
          const wr = cs[base + k]!, wi = sn[base + k]!;
          const x = i + k, y = x + half;
          const tr = re[y]! * wr - im[y]! * wi, ti = re[y]! * wi + im[y]! * wr;
          re[y] = re[x]! - tr; im[y] = im[x]! - ti;
          re[x] = re[x]! + tr; im[x] = im[x]! + ti;
        }
      }
    }
  }

  /** Transforms the real window `x` (of length n) into the own buffers; afterwards `energy(k)` gives each bin. */
  analyze(x: Float64Array) {
    this.re.set(x);
    this.im.fill(0);
    this.transform();
  }

  /** Energy of bin k from the last `analyze`, on the Goertzel scale: |X(k)|², unnormalized. */
  energy(k: number) {
    const r = this.re[k]!, i = this.im[k]!;
    return r * r + i * i;
  }
}
