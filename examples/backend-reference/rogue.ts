// The attacker: plays its own fingerprint over the real one. It turns every protection off on its side (as a real attacker would);
// the point is what the receiver and the backend do about it.
import { fingerprint, sendSession, type SendSession } from '../../web/src/index';
import { paymentRequest, publish, status } from './api';
import { $, describe, line } from './ui';

const account = $<HTMLInputElement>('account'), merchant = $<HTMLInputElement>('merchant'), amount = $<HTMLInputElement>('amount');
const publishFirst = $<HTMLInputElement>('publishFirst');
const go = $<HTMLButtonElement>('go'), stop = $<HTMLButtonElement>('stop'), statusText = $('status');
let tx: SendSession | null = null;

go.onclick = async () => {
  const me = account.value.trim();
  const payload = paymentRequest(merchant.value.trim(), amount.value.trim());
  const fp = fingerprint(payload);
  go.disabled = true;
  try {
    if (publishFirst.checked) {
      const ok = await publish(me, fp, payload);
      line(ok ? 'mine' : 'error', ok ? `published as ${me}: ${fp}` : 'the backend refused it (blocked?)');
    } else line('mine', `not published: ${fp} will resolve to nothing`);
    const session = tx = await sendSession(payload, {
      mode: 'reference',
      securityLevel: 'basic', // no detection, no risk frames, does not honor alerts
      security: { listenBeforeSendingSeconds: 0 }, // starts at once
      ambient: false,
      calibration: null,
      timeoutSeconds: 30,
      onRepetition: (r) => { statusText.textContent = `repetition ${r.index + 1} on ${r.band}`; },
    });
    stop.disabled = false;
    await session.delivered.then(
      (d) => line('alert', `a receiver acked it on ${d.band}: it was read`),
      (e) => line('error', describe(e)),
    );
    await session.finished;
    if (publishFirst.checked) {
      const s = await status(me, fp).catch(() => null);
      if (s) line(s.state === 'consumed' ? 'alert' : 'mine', `backend says: ${s.state}${s.receivedBy ? ` by ${s.receivedBy}` : ''}${s.blockedReason ? ` (${s.blockedReason})` : ''}`);
    }
  } catch (e) {
    line('error', describe(e));
  } finally {
    tx = null;
    go.disabled = false;
    stop.disabled = true;
    statusText.textContent = 'idle';
  }
};

stop.onclick = () => tx?.stop();
