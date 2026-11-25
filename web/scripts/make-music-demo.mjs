// Renders the demo melody with a hidden message on the 'top' band into ../docs/assets/music-demo.wav (16-bit mono, 48 kHz).
// Run from web/: npm run make:music-demo
import { createServer } from 'vite';
import { writeFileSync } from 'node:fs';

const MESSAGE = 'hello from the music';
const RATE = 48000;
const vite = await createServer({ server: { middlewareMode: true }, appType: 'custom', logLevel: 'error' });
try {
  const { renderMusicDemo } = await vite.ssrLoadModule('/src/music-demo.ts');
  const x = renderMusicDemo(MESSAGE, RATE);
  const data = Buffer.alloc(x.length * 2);
  x.forEach((v, i) => data.writeInt16LE(Math.round(Math.max(-1, Math.min(1, v)) * 32767), i * 2));
  const head = Buffer.alloc(44);
  head.write('RIFF', 0); head.writeUInt32LE(36 + data.length, 4); head.write('WAVEfmt ', 8);
  head.writeUInt32LE(16, 16); head.writeUInt16LE(1, 20); head.writeUInt16LE(1, 22);
  head.writeUInt32LE(RATE, 24); head.writeUInt32LE(RATE * 2, 28); head.writeUInt16LE(2, 32); head.writeUInt16LE(16, 34);
  head.write('data', 36); head.writeUInt32LE(data.length, 40);
  const out = new URL('../../docs/assets/music-demo.wav', import.meta.url);
  writeFileSync(out, Buffer.concat([head, data]));
  console.log(`wrote ${out.pathname} (${head.length + data.length} bytes, ${(x.length / RATE).toFixed(1)} s)`);
} finally {
  await vite.close();
}
