// The customer: hears a fingerprint, resolves it on the backend (without consuming it), shows the verified publisher and the amount,
// and pays (POST /received) only after the person confirms and the confirm gate is clean.
import { fingerprint, receiveSession, SessionError, type ReceiveSession } from '../../web/src/index';
import { ApiError, fetchPayload, markReceived, parsePaymentRequest, reportRisk, type Resolved } from './api';
import { $, alertShortIds, describe, isRisk, line } from './ui';

const account = $<HTMLInputElement>('account');
const listenBtn = $<HTMLButtonElement>('listen'), cancelBtn = $<HTMLButtonElement>('cancel'), statusText = $('status'), level = $('level');
const card = $('card'), confirmBtn = $<HTMLButtonElement>('confirm'), rejectBtn = $<HTMLButtonElement>('reject');

let rx: ReceiveSession | null = null;

listenBtn.onclick = async () => {
  const me = account.value.trim();
  /** Everything the resolver fetched in this session, by fingerprint: the payload and who published it. */
  const resolved = new Map<string, Resolved>();
  const heard: string[] = [];
  listenBtn.disabled = true;
  card.hidden = true;
  try {
    const session = rx = await receiveSession({
      // No ack by sound: the sender learns from the backend that its request was read and stops playing.
      ack: false,
      // Lookup that never consumes. bysound calls it on the first frame, in parallel with the final listen, so it may run for a reception
      // that is rejected a moment later (a second fingerprint, a clash): it must not consume anything.
      resolve: async (fp) => {
        heard.push(fp);
        try {
          const r = await fetchPayload(me, fp);
          resolved.set(fp, r);
          return r.payload; // bysound checks that SHA-256(payload) starts with fp, or fails with MISMATCH
        } catch (e) {
          line('error', `backend: ${fp} ${e instanceof ApiError ? e.code : 'unreachable'}`);
          return null; // → NOT_FOUND
        }
      },
      onHeard: (band) => { statusText.textContent = `heard a frame on ${band}, final listen…`; },
      onLevel: (rms) => { level.style.width = `${Math.min(100, rms * 600)}%`; },
      onAlert: (a) => line('alert', `⚠ alert ${a.direction}: ${a.reason} (from a ${a.source})`),
    });
    cancelBtn.disabled = false;
    statusText.textContent = 'listening…';
    const got = await session.result;

    // Accept only what the backend resolved. A plain payload frame (via 'payload') never went through the backend: anyone can play one.
    const fp = fingerprint(got.text);
    const meta = got.via === 'reference' ? resolved.get(fp) : undefined;
    const req = parsePaymentRequest(got.text);
    if (!meta || !req) {
      session.cancel();
      line('error', got.via === 'payload' ? 'a plain payload was played, not a reference: not accepted' : 'not a valid payment request: not accepted');
      return;
    }
    statusText.textContent = `read on ${got.band}; confirm watch running`;
    $('amount').textContent = `${req.amount} ${req.currency}`;
    $('publisher').textContent = meta.publisher;
    $('merchant').textContent = req.merchant;
    card.hidden = false;

    const choice = await new Promise<boolean>((done) => { confirmBtn.onclick = () => done(true); rejectBtn.onclick = () => done(false); });
    card.hidden = true;
    if (!choice) { session.cancel(); line('error', 'rejected by you: nothing was paid'); return; }

    // The confirm gate, right before acting: fails if another transmission or a risk frame was heard since delivery.
    try {
      const c = await session.confirmed();
      if (c.watch === 'expired') line('alert', `the watch ended ${c.uncoveredSeconds.toFixed(0)} s before you confirmed (unwatched gap)`);
    } catch (e) {
      line('alert', `not paid: ${describe(e)}`);
      await report(me, e, [fp, ...heard]);
      return;
    }
    // The only side effect, after the gate: consume the request (single use) and tell the shop.
    const r = await markReceived(me, fp, got.band).catch((e: unknown) => e as Error);
    if (r instanceof Error) line('error', `backend refused: ${r instanceof ApiError ? r.code : r.message} (nothing paid)`);
    else line('theirs', `✓ paid ${req.amount} ${req.currency} to ${r.publisher}`);
    await session.finished;
  } catch (e) {
    line(isRisk(e) ? 'alert' : 'error', describe(e));
    if (isRisk(e)) await report(me, e, heard);
  } finally {
    rx = null;
    listenBtn.disabled = false;
    cancelBtn.disabled = true;
    statusText.textContent = 'idle';
  }
};

cancelBtn.onclick = () => { rx?.cancel(); rejectBtn.click(); }; // also answers a pending Confirm / Reject

/** A clash or an alert: ask the backend to block every fingerprint involved (full ones we heard, short ids from the alert). */
async function report(me: string, e: unknown, fingerprints: string[]) {
  if (!(e instanceof SessionError)) return;
  const r = await reportRisk(me, e.code, [...new Set(fingerprints)], alertShortIds(e.alert)).catch(() => null);
  line('alert', r ? `reported to the backend; blocked: ${r.blocked.join(', ') || 'nothing'}` : 'could not report to the backend');
}
