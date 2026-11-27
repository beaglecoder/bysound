// Wi-Fi credentials as compact text, plain or protected with a one-time code. Pure functions (no DOM), so they also run in Node.
//
// Plain:      the standard Wi-Fi QR syntax:  WIFI:T:WPA;S:<ssid>;P:<password>;;      (special characters \ ; , : " escaped with \)
// Protected:  BSW1|<salt b64>|<iv b64>|<AES-GCM ciphertext b64 of the plain text>
//             key = PBKDF2-SHA256(code, salt, ITERATIONS)

export interface Wifi { ssid: string; password: string; security: 'WPA' | 'WEP' | 'nopass' }

const esc = (s: string) => s.replace(/([\\;,:"])/g, '\\$1');
const unesc = (s: string) => s.replace(/\\(.)/g, '$1');

export function wifiText({ ssid, password, security }: Wifi): string {
  return `WIFI:T:${security};S:${esc(ssid)};${security === 'nopass' ? '' : `P:${esc(password)};`};`;
}

export function parseWifi(text: string): Wifi | null {
  if (!text.startsWith('WIFI:')) return null;
  const field = (k: string) => new RegExp(`(?:^|;)${k}:((?:\\\\.|[^;\\\\])*)`).exec(text.slice(5))?.[1];
  const ssid = field('S'), t = field('T') ?? 'WPA';
  if (ssid === undefined || !['WPA', 'WEP', 'nopass'].includes(t)) return null;
  return { ssid: unesc(ssid), password: unesc(field('P') ?? ''), security: t as Wifi['security'] };
}

export const ITERATIONS = 200_000;
export const CODE_ALPHABET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789'; // 31 symbols, no look-alikes: 8 characters are about 40 bits
export const CODE_LENGTH = 8;
export const CODE_VALID_SECONDS = 120;

/** The one-time code a device shows on its screen. */
export function newCode(): string {
  const r = crypto.getRandomValues(new Uint8Array(CODE_LENGTH));
  return Array.from(r, (b) => CODE_ALPHABET[b % CODE_ALPHABET.length]).join('');
}

const b64 = (b: Uint8Array) => btoa(String.fromCharCode(...b));
const unb64 = (s: string) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
const normalize = (code: string) => code.toUpperCase().replace(/[^A-Z0-9]/g, '');

async function key(code: string, salt: Uint8Array): Promise<CryptoKey> {
  const material = await crypto.subtle.importKey('raw', new TextEncoder().encode(normalize(code)), 'PBKDF2', false, ['deriveKey']);
  return crypto.subtle.deriveKey({ name: 'PBKDF2', hash: 'SHA-256', salt: salt as BufferSource, iterations: ITERATIONS }, material, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}

export async function protect(plain: string, code: string): Promise<string> {
  const salt = crypto.getRandomValues(new Uint8Array(8)), iv = crypto.getRandomValues(new Uint8Array(12));
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, await key(code, salt), new TextEncoder().encode(plain)));
  return ['BSW1', b64(salt), b64(iv), b64(ct)].join('|');
}

/** null: wrong code or tampered text (AES-GCM authenticates). */
export async function unprotect(text: string, code: string): Promise<string | null> {
  const [magic, salt, iv, ct] = text.split('|');
  if (magic !== 'BSW1' || !salt || !iv || !ct) return null;
  try {
    return new TextDecoder().decode(await crypto.subtle.decrypt({ name: 'AES-GCM', iv: unb64(iv) as BufferSource }, await key(code, unb64(salt)), unb64(ct) as BufferSource));
  } catch { return null; }
}
