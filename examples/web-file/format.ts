// The text a file travels as: one line of printable ASCII, so it packs 1 byte per character (see web/README.md, limits).
//   BSF1|<name>|<size>|<sha256 hex>|<base64 of the bytes>
// The hash lets the receiver prove the bytes arrived intact (the modem already has CRC-32 + Reed-Solomon; this checks the whole file end to end).

export const MAGIC = 'BSF1';

export async function sha256Hex(bytes: Uint8Array): Promise<string> {
  const d = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes as BufferSource));
  return Array.from(d, (b) => b.toString(16).padStart(2, '0')).join('');
}

export const toBase64 = (b: Uint8Array) => { let s = ''; for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000)); return btoa(s); };
export const fromBase64 = (s: string) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

const safeName = (n: string) => n.replace(/[|\r\n]/g, '_').replace(/[^\x20-\x7e]/g, '_').slice(0, 40);

export async function packFile(name: string, bytes: Uint8Array): Promise<string> {
  return [MAGIC, safeName(name), bytes.length, await sha256Hex(bytes), toBase64(bytes)].join('|');
}

/** Characters of text that a file of `size` bytes takes, for a given name. */
export const textLength = (name: string, size: number) => MAGIC.length + safeName(name).length + String(size).length + 64 + 4 + 4 * Math.ceil(size / 3);

/** Largest file (bytes) whose text fits in `capacity` characters. */
export function maxFileBytes(name: string, capacity: number): number {
  let lo = 0, hi = capacity;
  while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (textLength(name, mid) <= capacity) lo = mid; else hi = mid - 1; }
  return lo;
}

export interface Unpacked { name: string; bytes: Uint8Array; hash: string; verified: boolean }

export async function unpackFile(text: string): Promise<Unpacked | null> {
  const parts = text.split('|');
  if (parts.length !== 5 || parts[0] !== MAGIC) return null;
  const [, name, size, hash, b64] = parts as [string, string, string, string, string];
  let bytes: Uint8Array;
  try { bytes = fromBase64(b64); } catch { return null; }
  const actual = await sha256Hex(bytes);
  return { name, bytes, hash: actual, verified: actual === hash && bytes.length === Number(size) };
}
