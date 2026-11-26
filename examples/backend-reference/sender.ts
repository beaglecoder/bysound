// The shop: publish the payment request on the backend, then play only its fingerprint (reference mode).
import { fingerprint, sendSession, type SendSession } from '../../web/src/index';
import { paymentRequest, publish, reportRisk, status } from './api';
import { $, alertShortIds, describe, isRisk, line } from './ui';

const account = $<HTMLInputElement>('account'), merchant = $<HTMLInputElement>('merchant'), amount = $<HTMLInputElement>('amount');
const go = $<HTMLButtonElement>('go'), stop = $<HTMLButtonElement>('stop'), statusText = $('status');
let tx: SendSession | null = null;

go.onclick = async () => {
  const me = account.value.trim();
  const payload = paymentRequest(merchant.value.trim(), amount.value.trim());
  const fp = fingerprint(payload);
  go.disabled = true;
  const entry = line('mine', `${amount.value} EUR: publishing ${fp}…`);
  try {
    // 1. Publish first, with its own timeout. Without a `true` nothing is played: a fingerprint the backend does not know is useless.
    //    Keep it fast: the browser lets a click start audio only for a few seconds.
    if (!(await publish(me, fp, payload))) { line('error', `${amount.value} EUR: the backend did not store it, nothing was played`, entry); return; }
    line('mine', `${amount.value} EUR: published, playing fingerprint ${fp}`, entry);
    watchBackend(me, fp);

    // 2. Play only the 12-byte fingerprint. Default security level (maximum): pre-listen, listen windows, risk frames.
    const session = tx = await sendSession(payload, {
      mode: 'reference',
      onState: (s) => { statusText.textContent = `sound: ${s}`; },
      onRepetition: (r) => { statusText.textContent = `sound: repetition ${r.index + 1} on ${r.band}`; },
    });
    stop.disabled = false;
    const d = await session.delivered;
    // The ack by sound means "a bysound receiver nearby read this fingerprint". The payment is confirmed by the backend, below.
    line('mine', `ack by sound: read on ${d.band} after ${d.seconds.toFixed(1)} s (not a payment yet: waiting for the backend)`);
    await session.finished;
  } catch (e) {
    line(isRisk(e) ? 'alert' : 'error', `sound: ${describe(e)}`);
    if (isRisk(e)) {
      // Tell the backend: this request was involved in a clash, so it must not be paid. Issue a new one when the air is clear.
      const r = await reportRisk(me, e.code, [fp], alertShortIds(e.alert)).catch(() => null);
      if (r) line('alert', `reported to the backend; blocked: ${r.blocked.join(', ') || 'nothing'}`);
    }
  } finally {
    tx = null;
    go.disabled = false;
    stop.disabled = true;
    statusText.textContent = 'idle';
  }
};

stop.onclick = () => tx?.stop();

/** The backend side of the ack: polls the request until it is used, expires or is blocked. A real app would get a push. */
function watchBackend(me: string, fp: string) {
  const started = Date.now();
  const timer = setInterval(async () => {
    const s = await status(me, fp).catch(() => null);
    if (!s || s.state === 'live') { if (Date.now() - started > 90_000) clearInterval(timer); return; }
    clearInterval(timer);
    if (s.state === 'consumed') line('theirs', `✓ backend: paid by ${s.receivedBy} (${fp})`);
    else if (s.state === 'blocked') line('alert', `⚠ backend: request blocked (${s.blockedReason}). Issue a new one.`);
    else line('error', `backend: request ${s.state}`);
  }, 1000);
}
