/**
 * Analysis window for v4 symbols (same as the Swift and Kotlin ports in ios/Sources/BySound and android/bysound). The tones fall exactly on the window's
 * grid: with a rectangular window they are orthogonal, but a strong nearby noise spills over the whole band. With slightly
 * smoothed edges almost no signal is lost and that spill is cut.
 */

/** Fraction of the window that is smoothed, split between the two edges. 0 is the rectangular window. */
export const TAPER_V4 = 0.1;

/** Tukey window of `size` samples: cosine at the edges and 1 elsewhere. */
export function symbolWindow(size: number, taper = TAPER_V4): Float64Array {
  const edge = (taper * (size - 1)) / 2;
  const out = new Float64Array(size).fill(1);
  if (edge <= 0) return out;
  for (let i = 0; i < size; i++) {
    const distance = Math.min(i, size - 1 - i);
    if (distance < edge) out[i] = 0.5 - 0.5 * Math.cos((Math.PI * distance) / edge);
  }
  return out;
}
