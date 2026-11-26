// The pages' side of the reference backend (server.mjs), reached through Vite's proxy at /api.
// "Signing in" is a demo header; a real app sends its session token over TLS and the server derives the account from it.

export interface Resolved { payload: string; publisher: string; publishedAt: number; expiresAt: number }
export interface PaymentRequest { type: 'payment-request'; merchant: string; amount: string; currency: string; nonce: string }

export class ApiError extends Error {
  constructor(readonly status: number, readonly code: string) { super(`${status} ${code}`); }
}

async function call<T>(account: string, method: 'GET' | 'POST', path: string, body?: unknown, timeoutMs = 5000): Promise<T> {
  // Every call has its own timeout: a backend that hangs must not hang the exchange by sound.
  const res = await fetch(`/api${path}`, {
    method,
    headers: { 'x-demo-user': account, ...(body ? { 'content-type': 'application/json' } : {}) },
    body: body ? JSON.stringify(body) : undefined,
    signal: AbortSignal.timeout(timeoutMs),
  });
  const json = (await res.json().catch(() => ({}))) as T & { error?: string };
  if (!res.ok) throw new ApiError(res.status, json.error ?? 'error');
  return json;
}

/** Stores the payload before anything is played. Idempotent: retrying the same payload is safe. Only `true` allows emitting. */
export async function publish(account: string, fingerprint: string, payload: string): Promise<boolean> {
  try {
    const r = await call<{ ok: boolean }>(account, 'POST', '/publish', { fingerprint, payload }, 3000);
    return r.ok === true;
  } catch {
    return false;
  }
}

/** Lookup that never consumes: may run for a reception that is rejected later (a clash in the final listen). The backend only notes the first read. */
export const fetchPayload = (account: string, fingerprint: string) => call<Resolved>(account, 'GET', `/resolve/${fingerprint}`);

/** The side effect: consumes the request (single use) and tells the publisher. Only after `confirmed()`. */
export const markReceived = (account: string, fingerprint: string, band: string) => call<{ ok: boolean; publisher: string }>(account, 'POST', '/received', { fingerprint, band });

/** Blocks the fingerprints involved in a clash or an alert (full fingerprints and/or the 3-byte short ids of a risk frame). */
export const reportRisk = (account: string, reason: string, fingerprints: string[], shortIds: string[]) =>
  call<{ blocked: string[] }>(account, 'POST', '/report-risk', { reason, fingerprints, shortIds });

/** The publisher's view of its request: the "backend ack", in addition to the ack by sound. */
export const status = (account: string, fingerprint: string) =>
  call<{ state: 'live' | 'consumed' | 'expired' | 'blocked'; receivedBy: string | null; resolvedAt: number | null; blockedReason: string | null }>(account, 'GET', `/status/${fingerprint}`);

/** A payment request. The random nonce makes every request (and its fingerprint) unique and unguessable. */
export function paymentRequest(merchant: string, amount: string, currency = 'EUR'): string {
  const nonce = Array.from(crypto.getRandomValues(new Uint8Array(12)), (b) => b.toString(16).padStart(2, '0')).join('');
  const req: PaymentRequest = { type: 'payment-request', merchant, amount, currency, nonce };
  return JSON.stringify(req);
}

export function parsePaymentRequest(text: string): PaymentRequest | null {
  try {
    const r = JSON.parse(text) as Partial<PaymentRequest>;
    if (r.type !== 'payment-request' || typeof r.merchant !== 'string' || typeof r.amount !== 'string' || typeof r.currency !== 'string') return null;
    if (!/^\d{1,7}(\.\d{1,2})?$/.test(r.amount)) return null;
    return r as PaymentRequest;
  } catch {
    return null;
  }
}
