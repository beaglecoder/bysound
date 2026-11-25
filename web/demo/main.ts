import { listen, send, type Listening, type Sending, type BandId } from '../src/index';

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

let sending: Sending | null = null;
sendBtn.onclick = async () => {
  if (sending) return sending.stop();
  try {
    sendBtn.textContent = 'Stop';
    sendBtn.classList.add('active');
    const s = (sending = await send(text.value, { band: band.value as BandId, repeat: 'forever' }));
    say('Sending…');
    await s.done;
  } catch (e) {
    fail(e);
  } finally {
    sending = null;
    sendBtn.textContent = 'Send';
    sendBtn.classList.remove('active');
    if (status.textContent === 'Sending…') say('');
  }
};

let listening: Listening | null = null;
listenBtn.onclick = async () => {
  if (listening) {
    listening.stop();
    listening = null;
    listenBtn.textContent = 'Listen';
    listenBtn.classList.remove('active');
    level.style.width = '0';
    return say('');
  }
  try {
    listening = await listen({
      onLevel: (e) => { level.style.width = `${Math.min(100, Math.sqrt(e.rms) * 150)}%`; },
      onHeader: (b) => say(`Receiving on ${b}…`),
      onFrame: (h) => {
        if (h.kind !== 'payload') return;
        received.querySelector('.empty')?.remove();
        const li = document.createElement('li');
        li.textContent = h.payload;
        const meta = document.createElement('small');
        meta.textContent = `${h.band} · ${new Date().toLocaleTimeString()} · ${h.correctedBytes} bytes corrected`;
        li.append(meta);
        received.prepend(li);
        say('Listening…');
      },
    });
    listenBtn.textContent = 'Stop';
    listenBtn.classList.add('active');
    say('Listening…');
  } catch (e) {
    fail(e);
  }
};
