// The presence token and its verification. Shared by the beacon page and the customer page.
//
//   P1|<storeId>|<window>|<mac>      e.g. P1|cafe42|58123456|9f3a1c07be22   (31 characters)
//
// window = floor(unix_seconds / 30); mac = first 6 bytes (12 hex characters) of HMAC-SHA256(storeKey, "<storeId>|<window>").
// The same computation is in esp32-beacon/main/main.c (mbedTLS); README.md has a test vector both must reproduce.

export const WINDOW_SECONDS = 30;
export const TOKEN_VERSION = 'P1';
export const MAC_HEX_CHARS = 12;

const enc = new TextEncoder();
const hex = (b: ArrayBuffer) => [...new Uint8Array(b)].map((x) => x.toString(16).padStart(2, '0')).join('');

export const windowOf = (unixMs: number) => Math.floor(unixMs / 1000 / WINDOW_SECONDS);

export async function mac(storeKey: string, storeId: string, window: number): Promise<string> {
  const key = await crypto.subtle.importKey('raw', enc.encode(storeKey), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return hex(await crypto.subtle.sign('HMAC', key, enc.encode(`${storeId}|${window}`))).slice(0, MAC_HEX_CHARS);
}

export async function makeToken(storeKey: string, storeId: string, unixMs: number): Promise<string> {
  const w = windowOf(unixMs);
  return `${TOKEN_VERSION}|${storeId}|${w}|${await mac(storeKey, storeId, w)}`;
}

export type Rejection = 'malformed' | 'unknown store' | 'too old' | 'from the future' | 'bad signature' | 'already used';
export type Verdict =
  | { ok: true; storeId: string; window: number; token: string }
  | { ok: false; reason: Rejection; detail: string; storeId?: string };

/**
 * Checks tokens against a key. In a real deployment this runs on the store's BACKEND (which holds the key): the phone only forwards
 * the token it heard. `used` is this verifier's own memory of tokens it already accepted for a check-in; it knows nothing about
 * what other verifiers (or other customers) saw.
 */
export class Verifier {
  private readonly used = new Set<string>();
  constructor(private readonly keyOf: (storeId: string) => string | undefined, private readonly tolerance = 1) {}

  async verify(token: string, unixMs: number): Promise<Verdict> {
    const m = /^P1\|([A-Za-z0-9_-]{1,16})\|(\d{1,10})\|([0-9a-f]{12})$/.exec(token);
    if (!m) return { ok: false, reason: 'malformed', detail: 'not a P1|store|window|mac token' };
    const [, storeId, w, tag] = m as unknown as [string, string, string, string];
    const window = Number(w);
    const key = this.keyOf(storeId);
    if (key === undefined) return { ok: false, reason: 'unknown store', detail: `no key for store "${storeId}"`, storeId };
    const drift = windowOf(unixMs) - window; // windows behind now
    if (drift > this.tolerance) return { ok: false, reason: 'too old', detail: `window ${drift} behind (about ${drift * WINDOW_SECONDS}-${(drift + 1) * WINDOW_SECONDS} s old; accepted: ±${this.tolerance})`, storeId };
    if (drift < -this.tolerance) return { ok: false, reason: 'from the future', detail: `window ${-drift} ahead: the beacon's clock is off, or the token is forged`, storeId };
    if ((await mac(key, storeId, window)) !== tag) return { ok: false, reason: 'bad signature', detail: 'the MAC does not match this store key', storeId };
    if (this.used.has(token)) return { ok: false, reason: 'already used', detail: 'this exact token was already accepted for a check-in', storeId };
    return { ok: true, storeId, window, token };
  }

  /** The check-in consumed this token: from now on it is rejected as "already used". */
  consume(token: string) { this.used.add(token); }
}
