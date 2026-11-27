import { encode } from '../../web/src/index';
import { makeToken } from './token';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const store = $<HTMLInputElement>('store'), key = $<HTMLInputElement>('key'), every = $<HTMLInputElement>('every');
let ctx: AudioContext | null = null;
let timer: ReturnType<typeof setTimeout> | undefined;

function log(text: string) {
  const li = document.createElement('li');
  li.className = 'mine'; li.textContent = `${new Date().toLocaleTimeString()}  ${text}`;
  $('log').prepend(li);
  while ($('log').children.length > 8) $('log').lastElementChild?.remove();
}

/** One broadcast: a fresh token (the 30 s window may have changed) rendered with the plain modem on `top`. No session, no acks. */
async function beep() {
  const ac = ctx;
  if (!ac) return;
  const token = await makeToken(key.value, store.value.trim(), Date.now());
  if (ctx !== ac) return; // stopped (or restarted) while the token was being signed
  $('token').textContent = token;
  const audio = encode(token, { band: 'top', sampleRate: ac.sampleRate });
  const buffer = ac.createBuffer(1, audio.length, ac.sampleRate);
  buffer.getChannelData(0).set(audio);
  const src = ac.createBufferSource();
  src.buffer = buffer;
  src.connect(ac.destination);
  src.start();
  log(`played ${token} (${buffer.duration.toFixed(1)} s)`);
  const wait = Math.max(Number(every.value) * 1000, buffer.duration * 1000 + 500); // never overlap two frames
  timer = setTimeout(() => void beep(), wait);
}

$('start').onclick = async () => {
  if (!/^[A-Za-z0-9_-]{1,16}$/.test(store.value.trim())) { $('state').textContent = 'Store id: 1-16 letters, digits, - or _.'; return; }
  // One AudioContext made inside the click and kept: later beeps are not user gestures (Safari would keep a new context suspended).
  const Ctx = globalThis.AudioContext ?? (globalThis as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext;
  ctx = new Ctx();
  await ctx.resume();
  $<HTMLButtonElement>('start').disabled = true; $<HTMLButtonElement>('stop').disabled = false;
  $('state').textContent = 'Broadcasting on top. Turn the volume up and use the speaker, not headphones.';
  void beep();
};
$('stop').onclick = () => {
  clearTimeout(timer); void ctx?.close(); ctx = null;
  $<HTMLButtonElement>('start').disabled = false; $<HTMLButtonElement>('stop').disabled = true;
  $('state').textContent = 'Stopped.';
};
