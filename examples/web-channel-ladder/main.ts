import { receiveSession, sendSession, SessionError } from '../../web/src/index';
import { ladder, tryRepetition } from './ladder';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const broken = $<HTMLInputElement>('broken'), cutoff = $<HTMLInputElement>('cutoff'), noise = $<HTMLInputElement>('noise');
const sync = () => { $('cutoffv').textContent = cutoff.value; $('noisev').textContent = noise.value; cutoff.disabled = !broken.checked; };
cutoff.oninput = noise.oninput = broken.onchange = sync;
sync();

const RATE = 48000, MAX_REPETITIONS = 30, TEXT = 'hello ladder';
const nextFrame = () => new Promise((r) => setTimeout(r, 0));

$('sim').onclick = async () => {
  const body = $('simtable').querySelector('tbody')!;
  body.textContent = ''; $('simresult').textContent = '';
  $<HTMLButtonElement>('sim').disabled = true;
  const steps = ladder();
  let result = 'No repetition was read in 30 tries: the model says this speaker cannot be used (try a lower cutoff or less noise).';
  for (let i = 0; i < MAX_REPETITIONS; i++) {
    const step = steps.next().value!;
    await nextFrame(); // let the page paint between decodes
    const o = tryRepetition(TEXT, step, RATE, broken.checked ? Number(cutoff.value) : null, Number(noise.value));
    const tr = body.insertRow();
    for (const t of [String(i + 1), step.band, step.profile, o.heard ? `✓ ${o.heard}${o.profile === 'robust' ? ' (robust)' : ''}` : '–']) tr.insertCell().textContent = t;
    tr.className = o.heard ? 'ok' : 'muted';
    if (o.heard) { result = `Delivered on ${step.band}${step.profile === 'robust' ? ' (robust)' : ''} after ${i + 1} repetition(s). A real sender stores this as its calibration and starts there next time.`; break; }
  }
  $('simresult').textContent = result;
  $<HTMLButtonElement>('sim').disabled = false;
};

// ---- live ----
const live = $('live');
function line(cls: string, text: string, el?: HTMLElement) {
  const li = el ?? document.createElement('li');
  li.className = cls; li.textContent = text;
  if (!el) live.prepend(li);
  return li;
}

$('send').onclick = async () => {
  const text = $<HTMLInputElement>('text').value;
  const li = line('mine', 'starting (listening to the room first)…');
  const reps: string[] = [];
  try {
    const s = await sendSession(text, {
      onAmbient: (_a, route) => line('mine', `route after the noise check: ${route.join(' → ')}`, li),
      onRepetition: (r) => { reps.push(`${r.band}${r.profile === 'robust' ? 'R' : ''}`); line('mine', `repetitions: ${reps.join(' ')}`, li); },
    });
    const d = await s.delivered;
    line('mine', `✓ delivered, read on ${d.band}${d.profile === 'robust' ? ' (robust)' : ''} after ${d.repetitions} repetition(s)${d.stated ? '' : ' (ack did not say where)'}; saved: ${d.calibration ? `${d.calibration.band}/${d.calibration.profile}` : 'nothing'}   [${reps.join(' ')}]`, li);
    await s.finished;
  } catch (e) { line('error', `✗ ${e instanceof SessionError ? e.code : ''} ${(e as Error).message}   [${reps.join(' ')}]`, li); }
};

$('listen').onclick = async () => {
  const li = line('theirs', 'listening…');
  try {
    const r = await receiveSession({ onHeard: (band) => line('theirs', `first frame heard on ${band}…`, li) });
    const got = await r.result;
    line('theirs', `received "${got.text}" on ${got.band}`, li);
    await r.finished;
  } catch (e) { line('error', `✗ ${(e as Error).message}`, li); }
};

$('forget').onclick = () => { try { localStorage.removeItem('bysound.calibration'); } catch { /* no storage */ } line('alert', 'calibration forgotten: the next send starts at the top again'); };
