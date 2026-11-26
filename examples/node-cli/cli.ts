// bysound in Node: text -> WAV and WAV -> text with the TypeScript library (no audio hardware).
//   node dist-node/cli.mjs encode "hello" -o hello.wav [--band top] [--profile normal|robust] [--rate 48000] [--level 1]
//   node dist-node/cli.mjs decode hello.wav [more.wav ...]
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { BAND_IDS, decode, encode, type BandId, type Profile } from '../../web/src/index';
import { readWav, writeWav } from '../shared/wav';

function usage(): never {
  console.error(`usage:
  cli encode "text" -o out.wav [--band ${BAND_IDS.join('|')}] [--profile normal|robust] [--rate 48000] [--level 0..1] [--lead 0.2]
  cli decode in.wav [more.wav ...]`);
  process.exit(2);
}

function cmdEncode(args: string[]) {
  let text: string | undefined, out: string | undefined, band: BandId = 'top', profile: Profile = 'normal', sampleRate = 48000, level = 1, lead = 0.2;
  for (let i = 0; i < args.length; i++) {
    const a = args[i]!, v = args[i + 1];
    if (a === '-o' && v) { out = v; i++; }
    else if (a === '--band' && v) { if (!BAND_IDS.includes(v as BandId)) usage(); band = v as BandId; i++; }
    else if (a === '--profile' && v) { if (v !== 'normal' && v !== 'robust') usage(); profile = v; i++; }
    else if (a === '--rate' && v) { sampleRate = Number(v); i++; }
    else if (a === '--level' && v) { level = Number(v); i++; }
    else if (a === '--lead' && v) { lead = Number(v); i++; }
    else if (!a.startsWith('-') && text === undefined) text = a;
    else usage();
  }
  if (text === undefined || !out) usage();
  let frame: Float32Array;
  try { frame = encode(text, { band, profile, sampleRate, level }); } catch (e) {
    if ((e as { code?: string }).code === 'TOO_LONG') { console.error(`text too long: one transmission cannot last more than 30 s on ${band}/${profile}`); process.exit(1); }
    throw e;
  }
  const samples = new Float32Array(Math.round(lead * sampleRate) + frame.length); // a little silence first, like a recording
  samples.set(frame, samples.length - frame.length);
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, writeWav(samples, sampleRate));
  console.log(`${out}: ${band}/${profile}, ${sampleRate} Hz, ${(samples.length / sampleRate).toFixed(2)} s`);
}

function cmdDecode(files: string[]) {
  if (!files.length) usage();
  let found = 0;
  for (const f of files) {
    const { samples, sampleRate } = readWav(new Uint8Array(readFileSync(f)));
    const heard = decode(samples, sampleRate);
    if (!heard.length) console.log(`${f}: nothing decoded`);
    for (const h of heard) {
      found++;
      console.log(`${f}: band=${h.band} profile=${h.profile ?? 'normal'} corrected=${h.correctedBytes} ${h.kind === 'payload' ? `text=${h.payload}` : `fingerprint=${h.fingerprint}`}`);
    }
  }
  process.exit(found ? 0 : 1);
}

const [cmd, ...rest] = process.argv.slice(2);
if (cmd === 'encode') cmdEncode(rest);
else if (cmd === 'decode') cmdDecode(rest);
else usage();
