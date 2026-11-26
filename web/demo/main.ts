import { renderMelody } from '../src/music';
import { receiveSession, send, sendSession, SessionError, type ReceiveSession, type SendSession, type Sending, type BandId } from '../src/index';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const text = $<HTMLTextAreaElement>('text');
const band = $<HTMLSelectElement>('band');
const sendBtn = $<HTMLButtonElement>('send');
const listenBtn = $<HTMLButtonElement>('listen');
const level = $('level');
const status = $('status');
const received = $('received');

const say = (m: string) => { status.textContent = m; };
const fail = (e: unknown) => say(e instanceof Error ? e.message : String(e));

// Send as a session: listen first, repeat until the other device acknowledges, stop if another sender shows up.
const failure = (e: unknown) => {
  if (!(e instanceof SessionError)) return e instanceof Error ? e.message : String(e);
  switch (e.code) {
    case 'OTHER_TRANSMISSION': case 'CLONED_TRANSMISSION': return '⚠ Another device is already sending: try again in a moment';
    case 'OTHER_EMITTER': case 'MULTIPLE_TRANSMISSIONS': return '⚠ Two senders at once: stopped';
    case 'RISK_SIGNAL': return '⚠ Another device raised an alert: stopped';
    case 'TIMEOUT': return 'Nobody acknowledged it';
    default: return '';
  }
};

let sending: SendSession | null = null;
sendBtn.onclick = async () => {
  if (sending) return sending.stop();
  try {
    sendBtn.textContent = 'Stop';
    sendBtn.classList.add('active');
    say('Listening before sending…');
    const fixed = band.value === 'auto' ? {} : { band: band.value as BandId };
    const s = (sending = await sendSession(text.value, { ...fixed, onRepetition: (r) => say(`Sending on ${r.band}${r.profile === 'robust' ? ' (robust)' : ''}…`) }));
    s.delivered.then((d) => say(`✓ Received on ${d.band} by the other device`), (e: unknown) => say(failure(e)));
    await s.finished;
  } catch (e) {
    fail(e);
  } finally {
    sending = null;
    sendBtn.textContent = 'Send';
    sendBtn.classList.remove('active');
  }
};

const musicBtn = $<HTMLButtonElement>('music');
let playing: Sending | null = null;
musicBtn.onclick = async () => {
  if (playing) return playing.stop();
  try {
    musicBtn.classList.add('active');
    musicBtn.textContent = 'Stop music';
    const s = (playing = await send(text.value, { band: 'top', repeat: 'forever', backing: renderMelody }));
    say('Playing music with the message on top…');
    await s.done;
  } catch (e) {
    fail(e);
  } finally {
    playing = null;
    musicBtn.textContent = '♪ Music + hidden message';
    musicBtn.classList.remove('active');
    if (status.textContent?.startsWith('Playing music')) say('');
  }
};

// Receive as a session: one message at a time, acknowledged by sound, then the confirm gate; then listen again.
let listening: ReceiveSession | null = null;
let wanted = false;
const addReceived = (message: string, meta: string) => {
  received.querySelector('.empty')?.remove();
  const li = document.createElement('li');
  li.textContent = message;
  const small = document.createElement('small');
  small.textContent = meta;
  li.append(small);
  received.prepend(li);
  return small;
};
const stopListening = () => {
  wanted = false;
  listening?.cancel();
  listening = null;
  listenBtn.textContent = 'Listen';
  listenBtn.classList.remove('active');
  level.style.width = '0';
};

async function receiveOne() {
  const r = (listening = await receiveSession({
    onLevel: (rms) => { level.style.width = `${Math.min(100, Math.sqrt(rms) * 150)}%`; },
    onHeard: (b) => say(`Receiving on ${b}…`),
    onAlert: (a) => { if (a.direction === 'heard') say('⚠ Another device raised an alert'); },
  }));
  say('Listening…');
  try {
    const got = await r.result;
    const meta = addReceived(got.text, `${got.band} · ${new Date().toLocaleTimeString()}`);
    say('Received. Acknowledging…');
    await r.receipt; // the confirm gate keeps watching while acking
    await r.confirmed();
    meta.textContent += ' · ✓ confirmed';
  } catch (e) {
    const message = failure(e);
    if (message && !(e instanceof SessionError && e.code === 'TIMEOUT')) addReceived(message, new Date().toLocaleTimeString());
  }
  await r.finished;
}

listenBtn.onclick = async () => {
  if (wanted) return stopListening();
  wanted = true;
  listenBtn.textContent = 'Stop';
  listenBtn.classList.add('active');
  try {
    while (wanted) await receiveOne();
  } catch (e) {
    fail(e); // browsers may refuse to reopen audio without a new tap
    stopListening();
  }
};
