// Minimal 16-bit PCM mono WAV reader/writer for the Node examples.
export function writeWav(samples: Float32Array, sampleRate: number): Uint8Array {
  const out = new Uint8Array(44 + samples.length * 2);
  const v = new DataView(out.buffer);
  const tag = (at: number, s: string) => { for (let i = 0; i < s.length; i++) out[at + i] = s.charCodeAt(i); };
  tag(0, 'RIFF'); v.setUint32(4, 36 + samples.length * 2, true); tag(8, 'WAVEfmt ');
  v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
  v.setUint32(24, sampleRate, true); v.setUint32(28, sampleRate * 2, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true);
  tag(36, 'data'); v.setUint32(40, samples.length * 2, true);
  for (let i = 0; i < samples.length; i++) v.setInt16(44 + i * 2, Math.round(Math.max(-1, Math.min(1, samples[i]!)) * 32767), true);
  return out;
}

/** Reads 16-bit PCM WAV (first channel of a multichannel file). Walks the RIFF chunks. */
export function readWav(bytes: Uint8Array): { samples: Float32Array; sampleRate: number } {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const tag = (at: number) => String.fromCharCode(...bytes.subarray(at, at + 4));
  if (bytes.length < 12 || tag(0) !== 'RIFF' || tag(8) !== 'WAVE') throw new Error('not a RIFF/WAVE file');
  let rate = 0, channels = 0, bits = 0, format = 0;
  for (let pos = 12; pos + 8 <= bytes.length;) {
    const size = v.getUint32(pos + 4, true);
    const body = pos + 8;
    if (tag(pos) === 'fmt ') {
      format = v.getUint16(body, true); channels = v.getUint16(body + 2, true); rate = v.getUint32(body + 4, true); bits = v.getUint16(body + 14, true);
    } else if (tag(pos) === 'data') {
      if (format !== 1 || bits !== 16 || channels < 1) throw new Error('only 16-bit PCM WAV is supported');
      const frames = Math.floor(Math.min(size, bytes.length - body) / (2 * channels));
      const samples = new Float32Array(frames);
      for (let i = 0; i < frames; i++) samples[i] = v.getInt16(body + i * 2 * channels, true) / 32768;
      return { samples, sampleRate: rate };
    }
    pos = body + size + (size & 1);
  }
  throw new Error('no data chunk');
}
