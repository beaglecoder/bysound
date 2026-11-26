// Two browsers chatting by sound with the session layer: sendSession / receiveSession (docs/SESSION.md).
import { SessionError, receiveSession, sendSession, type ReceiveSession, type SendSession } from '../../web/src/index';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const log = $('log'), status = $('status'), level = $('level'), text = $<HTMLInputElement>('text');
const startBtn = $<HTMLButtonElement>('start'), stopBtn = $<HTMLButtonElement>('stop'), sendBtn = $<HTMLButtonElement>('send');

type Kind = 'mine' | 'theirs' | 'alert' | 'error';
function line(kind: Kind, html: string, existing?: HTMLElement): HTMLElement {
  const li = existing ?? document.createElement('li');
  li.className = kind;
  li.textContent = html;
  if (!existing) log.prepend(li);
  return li;
}
const setStatus = (s: string) => { status.textContent = s; };

// What a SessionError means for a person. The codes are in docs/SESSION.md section 8.
function explain(e: unknown): { kind: Kind; message: string } {
  if (!(e instanceof SessionError)) return { kind: 'error', message: `Error: ${(e as Error).message}` };
  switch (e.code) {
    case 'OTHER_TRANSMISSION': return { kind: 'alert', message: '⚠ Someone else is already transmitting. Nothing was sent; try again in a moment.' };
    case 'OTHER_EMITTER':
    case 'CLONED_TRANSMISSION': return { kind: 'alert', message: '⚠ Two senders at once: another transmission was heard while sending. Sending stopped.' };
    case 'MULTIPLE_TRANSMISSIONS': return { kind: 'alert', message: '⚠ Two senders at once: two different messages arrived together, nothing was delivered. Ask them to resend.' };
    case 'RISK_SIGNAL': return { kind: 'alert', message: '⚠ Another device warned of a clash by sound; stopped to stay safe.' };
    case 'TIMEOUT': return { kind: 'error', message: 'No acknowledgement: the other side did not hear it (volume? distance? try again).' };
    case 'STOPPED': case 'CANCELLED': return { kind: 'error', message: 'Stopped.' };
    default: return { kind: 'error', message: `${e.code}: ${e.message}` };
  }
}

let running = false;
let rx: ReceiveSession | null = null;
let tx: SendSession | null = null;

/** Listens for one incoming message, shows it, then listens again. Only a Stop or a send interrupts the loop. */
async function listenLoop() {
  while (running) {
    try {
      setStatus('listening…');
      const session = rx = await receiveSession({ onLevel: (rms) => { level.style.width = `${Math.min(100, rms * 600)}%`; }, onHeard: (band) => setStatus(`hearing a message on ${band}…`) });
      const got = await session.result;
      line('theirs', `${got.text}   (read on ${got.band})`);
      // The confirm gate: before acting on data, ask whether anything suspicious happened while we kept listening.
      const c = await session.confirmed().catch((e) => { const x = explain(e); line(x.kind, `${x.message} (the message above is not trustworthy)`); return null; });
      if (c) setStatus(`message confirmed (watch: ${c.watch})`);
      await session.finished;
    } catch (e) {
      if (!running) return;
      if (e instanceof SessionError && e.code === 'CANCELLED') return; // a send is taking over the microphone
      const x = explain(e);
      if (e instanceof SessionError && e.code === 'TIMEOUT') continue; // nothing heard in 60 s: just keep listening
      line(x.kind, x.message);
      if (!(e instanceof SessionError)) { stop(); return; }
    }
  }
}

async function send() {
  const message = text.value.trim();
  if (!message || !running || tx) return;
  text.value = '';
  sendBtn.disabled = true;
  const mine = line('mine', `${message}   … sending`);
  rx?.cancel(); // half duplex: one microphone user at a time
  await rx?.finished.catch(() => undefined);
  try {
    // Default options = the automatic ladder (top, high, upper, mid) with calibration remembered in localStorage.
    const session = tx = await sendSession(message, {
      onState: (s) => setStatus(`sending: ${s}`),
      onRepetition: (r) => line('mine', `${message}   … repetition ${r.index + 1} on ${r.band}${r.profile === 'robust' ? ' (robust)' : ''}`, mine),
    });
    const d = await session.delivered;
    line('mine', `${message}   ✓ delivered on ${d.band}${d.profile === 'robust' ? ' (robust)' : ''} in ${d.seconds.toFixed(1)} s, ${d.repetitions} repetition(s)`, mine);
    await session.finished; // receipts are played after the ack
  } catch (e) {
    const x = explain(e);
    line(x.kind, `${message}   ✗ ${x.message}`, mine);
  } finally {
    tx = null;
    sendBtn.disabled = !running;
    if (running) void listenLoop();
  }
}

function stop() {
  running = false;
  rx?.cancel(); tx?.stop();
  startBtn.disabled = false; stopBtn.disabled = true; sendBtn.disabled = true; text.disabled = true;
  setStatus('idle'); level.style.width = '0';
}

startBtn.onclick = () => {
  running = true;
  startBtn.disabled = true; stopBtn.disabled = false; sendBtn.disabled = false; text.disabled = false;
  void listenLoop(); // called straight from the click: browsers only start audio from a user gesture
};
stopBtn.onclick = stop;
sendBtn.onclick = () => void send();
text.onkeydown = (e) => { if (e.key === 'Enter') void send(); };
