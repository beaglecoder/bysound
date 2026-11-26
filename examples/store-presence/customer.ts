import { listen, type Listening } from '../../web/src/index';
import { makeToken, Verifier, WINDOW_SECONDS, type Verdict } from './token';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const key = $<HTMLInputElement>('key'), names = $<HTMLTextAreaElement>('names');

function stores(): Map<string, string> {
  const m = new Map<string, string>();
  for (const line of names.value.split('\n')) { const [id, ...n] = line.split('='); if (id?.trim() && n.length) m.set(id.trim(), n.join('=').trim()); }
  return m;
}
const verifier = new Verifier((id) => (stores().has(id) ? key.value : undefined));

let current: { token: string; storeId: string; heardAt: number } | null = null;
function say(kind: 'theirs' | 'error' | 'alert', text: string) {
  const li = document.createElement('li');
  li.className = kind; li.textContent = `${new Date().toLocaleTimeString()}  ${text}`;
  $('got').prepend(li);
  while ($('got').children.length > 12) $('got').lastElementChild?.remove();
}
function render() {
  const card = $('card');
  if (!current) { $('where').textContent = ear ? 'Listening… nothing verified yet.' : 'Not listening.'; card.style.borderLeftColor = 'var(--muted)'; $<HTMLButtonElement>('checkin').disabled = true; return; }
  const age = Math.round((Date.now() - current.heardAt) / 1000);
  const stale = age > 2 * WINDOW_SECONDS;
  $('where').textContent = stale ? `📍 Last seen at ${stores().get(current.storeId)} (${age} s ago): no longer in range?` : `📍 You are at ${stores().get(current.storeId)} (verified, ${age} s ago)`;
  card.style.borderLeftColor = stale ? 'var(--warn)' : 'var(--ok)';
  $<HTMLButtonElement>('checkin').disabled = stale;
}
setInterval(render, 1000);

async function handle(token: string, quiet = false) {
  const v: Verdict = await verifier.verify(token, Date.now());
  if (v.ok) {
    if (current?.token !== token) say('theirs', `✓ ${stores().get(v.storeId)}: valid token (window ${v.window})`);
    current = { token, storeId: v.storeId, heardAt: Date.now() };
  } else if (!quiet || v.reason !== 'already used') {
    say('error', `✗ rejected (${v.reason}): ${v.detail}  [${token}]`);
  }
  render();
}

$('checkin').onclick = async () => {
  if (!current) return;
  // In a real app this is where the phone POSTs { token } to the store backend, which verifies it and answers with the coupon / queue ticket.
  const v = await verifier.verify(current.token, Date.now());
  if (!v.ok) { say('error', `✗ check-in refused (${v.reason}): ${v.detail}`); return; }
  verifier.consume(current.token);
  say('alert', `Checked in at ${stores().get(v.storeId)}. Demo only: nothing was sent anywhere. A real backend would now issue the coupon or queue ticket.`);
  current = null; render();
};

let ear: Listening | null = null;
$('listen').onclick = async () => {
  try {
    ear = await listen({ onFrame: (h) => { if (h.kind === 'payload') void handle(h.payload, true); } });
    $<HTMLButtonElement>('listen').disabled = true; $<HTMLButtonElement>('stop').disabled = false; render();
  } catch (e) { say('error', (e as Error).message); }
};
$('stop').onclick = () => { ear?.stop(); ear = null; $<HTMLButtonElement>('listen').disabled = false; $<HTMLButtonElement>('stop').disabled = true; render(); };

$('selftest').onclick = async () => {
  const now = Date.now(), id = [...stores().keys()][0] ?? 'cafe42';
  if (!stores().has(id)) names.value = 'cafe42 = Café Demo';
  say('alert', '--- self-test ---');
  const fresh = await makeToken(key.value, id, now);
  await handle(fresh);                                                   // valid
  await handle(await makeToken(key.value, id, now - 5 * WINDOW_SECONDS * 1000)); // a recording replayed minutes later
  await handle(await makeToken('some other key', id, now));              // forged: wrong key
  await handle(await makeToken(key.value, 'ghost9', now));               // a store this phone does not know
  await handle(await makeToken(key.value, id, now + 4 * WINDOW_SECONDS * 1000)); // beacon clock ahead
  await handle('P1|nonsense');                                           // garbage
  verifier.consume(fresh);
  await handle(fresh);                                                   // the same token again after a check-in
  current = null; render();
};
