// Test helpers: synthetic air, no browser.
export function gaussian(seed: number) {
  let s = seed >>> 0;
  const u = () => { s = (s + 0x6d2b79f5) >>> 0; let t = Math.imul(s ^ (s >>> 15), 1 | s); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
  return () => Math.sqrt(-2 * Math.log(1 - u())) * Math.cos(2 * Math.PI * u());
}
export interface Air { noise: number; gain?: number; echo?: number; echoSeconds?: number; lead?: number; seed?: number }
/** Joins the repetitions with leading silence, and adds echo, gain and white noise. */
export function air(laps: Float32Array[], rate: number, a: Air): Float32Array {
  const lead = Math.round((a.lead ?? 0.5) * rate);
  const out = new Float32Array(lead + laps.reduce((s, l) => s + l.length, 0) + Math.round(0.3 * rate));
  let at = lead;
  for (const l of laps) { out.set(l, at); at += l.length; }
  const delay = Math.round((a.echoSeconds ?? 0.005) * rate);
  if (a.echo) for (let i = out.length - 1; i >= delay; i--) out[i]! += a.echo * out[i - delay]!;
  const g = a.gain ?? 1, n = gaussian(a.seed ?? 1);
  for (let i = 0; i < out.length; i++) out[i] = out[i]! * g + n() * a.noise;
  return out;
}
/** In chunks, as it arrives from the worklet. */
export function feedChunks(x: Float32Array, feed: (c: Float32Array) => void, size = 1024) {
  for (let i = 0; i < x.length; i += size) feed(x.subarray(i, i + size));
}
