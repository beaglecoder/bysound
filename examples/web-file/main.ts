// A small file over sound: base64 text + SHA-256, with the per-band size limit and progress on both sides.
import { decode, encode, listen, maxPayloadBytes, receiveSession, sendSession, send, type BandId, type Listening, type Profile } from '../../web/src/index';
import { maxFileBytes, packFile, unpackFile } from './format';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const bandSel = $<HTMLSelectElement>('band'), robust = $<HTMLInputElement>('robust'), sessionMode = $<HTMLInputElement>('session');
const fileIn = $<HTMLInputElement>('file'), sendBtn = $<HTMLButtonElement>('send'), loopBtn = $<HTMLButtonElement>('loop');
const listenBtn = $<HTMLButtonElement>('listen'), stopBtn = $<HTMLButtonElement>('stop');
const bar = (id: string, p: number) => { $(id).style.width = `${Math.round(Math.max(0, Math.min(1, p)) * 100)}%`; };
const profile = (): Profile => (robust.checked ? 'robust' : 'normal');
const band = () => bandSel.value as BandId;
let file: { name: string; bytes: Uint8Array } | null = null;

function capacity(): number {
  try { return maxPayloadBytes(band(), profile()); } catch { return 0; } // robust does not exist on audible/xhigh/wide
}
function refresh() {
  if (robust.checked && !['top', 'high', 'upper', 'mid'].includes(band())) robust.checked = false;
  const cap = capacity();
  $('limits').textContent = `${band()} / ${profile()}: up to ${cap} text characters per transmission, so a file of about ${maxFileBytes(file?.name ?? 'file.bin', cap)} bytes.`;
  const fits = !!file && file.bytes.length <= maxFileBytes(file.name, cap);
  $('fileinfo').textContent = file ? `${file.name}: ${file.bytes.length} bytes ${fits ? 'fits' : 'is too big for this band: pick a faster band or a smaller file'}` : '';
  sendBtn.disabled = loopBtn.disabled = !fits;
}
bandSel.onchange = robust.onchange = refresh;
fileIn.onchange = async () => {
  const f = fileIn.files?.[0];
  file = f ? { name: f.name, bytes: new Uint8Array(await f.arrayBuffer()) } : null;
  refresh();
};
refresh();

const SAMPLE_RATE = 48000;
async function prepared() {
  const text = await packFile(file!.name, file!.bytes);
  const audio = encode(text, { band: band(), profile: profile(), sampleRate: SAMPLE_RATE }); // throws TOO_LONG if it does not fit
  return { text, seconds: audio.length / SAMPLE_RATE };
}

const failure = (e: unknown) => (e as { code?: string }).code === 'TOO_LONG' ? 'Too long for one transmission (30 s cap): pick a faster band or a smaller file.' : (e as Error).message;

sendBtn.onclick = async () => {
  try {
    const { text, seconds } = await prepared();
    $('txstatus').textContent = `Sending ${text.length} characters, ${seconds.toFixed(1)} s per repetition…`;
    const t0 = performance.now();
    const tick = setInterval(() => bar('txbar', (performance.now() - t0) / 1000 / seconds), 100);
    try {
      if (sessionMode.checked) {
        const s = await sendSession(text, { band: band(), profile: profile() });
        const d = await s.delivered;
        $('txstatus').textContent = `✓ acknowledged: read on ${d.band} in ${d.seconds.toFixed(1)} s`;
        await s.finished;
      } else {
        const s = await send(text, { band: band(), profile: profile(), repeat: 2 });
        await s.done;
        $('txstatus').textContent = 'Played twice. The receiver shows whether the hash matched.';
      }
    } finally { clearInterval(tick); bar('txbar', 1); }
  } catch (e) { $('txstatus').textContent = `✗ ${failure(e)}`; }
};

// Offline: encode, decode and verify inside the page. Shows the whole chain without a speaker or microphone.
loopBtn.onclick = async () => {
  try {
    const { text } = await prepared();
    const heard = decode(encode(text, { band: band(), profile: profile(), sampleRate: SAMPLE_RATE }), SAMPLE_RATE)[0];
    if (heard?.kind !== 'payload') { $('txstatus').textContent = '✗ loopback: nothing decoded'; return; }
    show(await unpackFile(heard.payload), heard.band, heard.correctedBytes);
    $('txstatus').textContent = '✓ loopback decoded (see Receive below)';
  } catch (e) { $('txstatus').textContent = `✗ ${failure(e)}`; }
};

function show(u: Awaited<ReturnType<typeof unpackFile>>, heardBand: BandId, corrected: number) {
  const li = document.createElement('li');
  if (!u) { li.className = 'error'; li.textContent = 'Heard text that is not a bysound file.'; }
  else {
    li.className = u.verified ? 'theirs' : 'error';
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([u.bytes as BlobPart]));
    a.download = u.name; a.textContent = `download ${u.name}`;
    li.append(`${u.verified ? '✓ SHA-256 verified' : '✗ HASH MISMATCH, do not trust'} · ${u.bytes.length} bytes · read on ${heardBand} · ${corrected} byte(s) corrected · ${u.hash.slice(0, 16)}… `, a);
  }
  $('got').prepend(li);
}

let ear: Listening | null = null;
let stopSession: (() => void) | null = null;
function ended() { ear = null; stopSession = null; listenBtn.disabled = false; stopBtn.disabled = true; bar('rxbar', 0); }
stopBtn.onclick = () => { ear?.stop(); stopSession?.(); ended(); $('rxstatus').textContent = 'stopped'; };

listenBtn.onclick = async () => {
  listenBtn.disabled = false; stopBtn.disabled = false; listenBtn.disabled = true;
  $('rxstatus').textContent = 'listening…';
  try {
    if (sessionMode.checked) {
      const r = await receiveSession({ onHeard: (b) => { $('rxstatus').textContent = `receiving on ${b}…`; } });
      stopSession = () => r.cancel();
      const got = await r.result;
      show(await unpackFile(got.text), got.band, 0);
      await r.finished;
    } else {
      ear = await listen({
        // LevelEvent.progress is the decoding progress 0..1 of the frame being read; blocksTotal is known once the header is read.
        onLevel: (e) => { bar('rxbar', e.progress); if (e.blocksTotal) $('rxstatus').textContent = `receiving… ${e.blocksTotal} block(s)`; },
        onFrame: async (h) => { if (h.kind === 'payload') show(await unpackFile(h.payload), h.band, h.correctedBytes); },
      });
      return; // keeps listening until Stop
    }
  } catch (e) { $('rxstatus').textContent = `✗ ${(e as Error).message}`; }
  ended();
};
