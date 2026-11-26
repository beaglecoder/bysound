// Renders the "music + hidden message" demo with different channels and message levels into WAVs and reports which ones decode.
//   node dist-node/music-variants.mjs [--text "hello from the music"] [--rate 48000] [--out out/music-variants] [top:0.5 top:0.15 mid:0.3 ...]
import { mkdirSync, writeFileSync } from 'node:fs';
import { decode, encode, mix, renderMelody, type BandId } from '../../web/src/index';
import { writeWav } from '../shared/wav';

let text = 'hello from the music', rate = 48000, out = 'out/music-variants';
const variants: { band: BandId; level: number }[] = [];
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const a = argv[i]!;
  if (a === '--text') text = argv[++i]!;
  else if (a === '--rate') rate = Number(argv[++i]);
  else if (a === '--out') out = argv[++i]!;
  else { const [b, l] = a.split(':'); variants.push({ band: b as BandId, level: Number(l ?? 0.5) }); }
}
if (!variants.length) {
  for (const level of [0.5, 0.3, 0.15, 0.08]) variants.push({ band: 'top', level });
  for (const level of [0.5, 0.3, 0.15]) variants.push({ band: 'mid', level });
  variants.push({ band: 'high', level: 0.3 }, { band: 'upper', level: 0.3 });
}

mkdirSync(out, { recursive: true });
const melody = renderMelody(rate);
console.log(`message "${text}", ${rate} Hz, melody ${(melody.length / rate).toFixed(1)} s, files in ${out}/\n`);
console.log('variant      peak(msg)  decoded  band   corrected  file');
let missed = 0;
for (const { band, level } of variants) {
  // `level` is the peak of the message alone (the demo uses 0.5). The melody peaks at <= 0.5, so the mix never clips.
  const audio = mix(encode(text, { band, sampleRate: rate, level }), melody);
  const file = `${out}/${band}-${String(level).replace('.', '_')}.wav`;
  writeFileSync(file, writeWav(audio, rate));
  const h = decode(audio, rate).find((x) => x.kind === 'payload' && x.payload === text);
  if (!h) missed++;
  console.log(`${`${band}:${level}`.padEnd(12)} ${String(level).padEnd(10)} ${(h ? 'yes' : 'NO').padEnd(8)} ${(h?.band ?? '-').padEnd(6)} ${String(h?.correctedBytes ?? '-').padEnd(10)} ${file}`);
}
console.log(`\n${variants.length - missed}/${variants.length} variants decoded. Lower message levels are quieter in the mix and harder to read; mid is the most audible channel, top the least.`);
process.exit(0);
