// Executable reference for bysound v4 (frame and parity). The Swift (ios/Sources/BySound) and Kotlin (android/bysound)
// ports must produce exactly the same output.
// Usage: node v4-reference.mjs   (writes ../test-vectors/v4-frames.json, v4-acks.json and v4-risk.json)
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';

// The constants come from ../protocol-v4.json, the shared definition (Swift, Kotlin, C and the web compare against it in their tests).
const spec = JSON.parse(readFileSync(new URL('../protocol-v4.json', import.meta.url), 'utf8'));

/** Same shape as always: `spacing`, `tones`, `gap`, `bands` with { bin, groups, amp, sym }, etc. */
export const P4 = {
  spacing: spec.spacingHz, tones: spec.tonesPerGroup, gap: spec.gapSeconds, version: spec.version,
  refParity: spec.referenceParity, motherParity: spec.motherParity, blockData: spec.blockData, blockParity: spec.blockParity,
  refBytes: spec.referenceBytes, ackParity: spec.ackParity, riskMaxHops: spec.riskMaxHops,
  magic: { payload: spec.magicPayload, ack: spec.magicAck, reference: spec.magicReference, risk: spec.magicRisk },
  preamble: spec.preamble,
  // Band identifier carried in the header (3 bits). 7 is left free.
  channelIds: spec.channelIds,
  // Preamble key [column shift, nibble offset]: each band and profile has its own pattern, of the same length.
  // It comes from a search that minimizes how much one pattern resembles another at any shift: the worst case is 25 % (previously 100 %).
  preambleKeys: spec.preambleKeys,
  // bin: first grid bin (frequency = bin × spacing); groups: simultaneous tones; sym: seconds per symbol.
  bands: Object.fromEntries(Object.entries(spec.bands).map(([id, b]) => [id, { bin: b.bin, groups: b.groups, amp: b.toneAmplitude, sym: b.symbolSeconds }])),
};

const utf8 = (s) => Array.from(new TextEncoder().encode(s));
export const sha256 = (bytes) => Array.from(createHash('sha256').update(Buffer.from(bytes)).digest());

/** Text packing: digit pairs in one byte (0x80 + n), ASCII as is, everything else as 0xFF, length, UTF-8 bytes. */
export function pack(text) {
  const u = [];
  for (let i = 0; i < text.length; i++) u.push(text.charCodeAt(i));
  const dig = (c) => c >= 0x30 && c <= 0x39, asc = (c) => c >= 0x20 && c <= 0x7e;
  const out = [];
  let i = 0;
  while (i < u.length) {
    const c = u[i];
    if (dig(c) && i + 1 < u.length && dig(u[i + 1])) { out.push(0x80 + (c - 0x30) * 10 + (u[i + 1] - 0x30)); i += 2; }
    else if (asc(c)) { out.push(c); i += 1; }
    else {
      let j = i;
      while (j < u.length && !asc(u[j])) j++;
      // Runs of up to 255 bytes without splitting a character; the receiver joins consecutive 0xFF runs.
      let cur = [];
      for (const ch of text.slice(i, j)) {
        const b = utf8(ch);
        if (cur.length + b.length > 255) { out.push(0xff, cur.length, ...cur); cur = []; }
        cur.push(...b);
      }
      if (cur.length > 0) out.push(0xff, cur.length, ...cur);
      i = j;
    }
  }
  return out;
}

export function crc16(data) {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

/** Header CRC-8: polynomial 0x2F, init 0xFF. */
export function crc8(data) {
  let c = 0xff;
  for (const byte of data) {
    c ^= byte;
    for (let i = 0; i < 8; i++) c = c & 0x80 ? ((c << 1) ^ 0x2f) & 0xff : (c << 1) & 0xff;
  }
  return c;
}

export function crc32(data) {
  let c = 0xffffffff;
  for (const b of data) { c ^= b; for (let i = 0; i < 8; i++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1; }
  return (c ^ 0xffffffff) >>> 0;
}

/** Reed-Solomon over GF(2^8), polynomial 0x11D, roots from α^0 (systematic code). */
export const RS = (() => {
  const exp = new Array(512).fill(0), log = new Array(256).fill(0);
  let x = 1;
  for (let i = 0; i < 255; i++) { exp[i] = x; log[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11d; }
  for (let i = 255; i < 512; i++) exp[i] = exp[i - 255];
  const mul = (a, b) => (a === 0 || b === 0 ? 0 : exp[log[a] + log[b]]);
  const pow = (a, n) => { let e = (log[a] * n) % 255; if (e < 0) e += 255; return exp[e]; };
  const pmul = (p, q) => {
    const r = new Array(p.length + q.length - 1).fill(0);
    q.forEach((b, j) => p.forEach((a, i) => { r[i + j] ^= mul(a, b); }));
    return r;
  };
  const generator = (n) => { let g = [1]; for (let i = 0; i < n; i++) g = pmul(g, [1, pow(2, i)]); return g; };
  function encode(data, nsym) {
    const gen = generator(nsym);
    const out = data.concat(new Array(nsym).fill(0));
    for (let i = 0; i < data.length; i++) {
      const coef = out[i];
      if (coef) for (let j = 1; j < gen.length; j++) out[i + j] ^= mul(gen[j], coef);
    }
    return data.concat(out.slice(data.length));
  }
  return { encode };
})();

const toNibbles = (bytes) => bytes.flatMap((b) => [b >> 4, b & 15]);
export function toSymbols(bytes, groups) {
  const n = toNibbles(bytes);
  while (n.length % groups) n.push(0);
  const out = [];
  for (let i = 0; i < n.length; i += groups) out.push(n.slice(i, i + groups));
  return out;
}

/** Header byte 3: version, band and parity set. */
export const versionByte = (bandId, set = 0) => (P4.version << 4) | (P4.channelIds[bandId] << 1) | (set & 1);

/**
 * v4 header, 4 bytes: [type, length, CRC-8, version | band | set]. The CRC covers type, length and version, plus the
 * profile (0 normal, 1 robust), which is not transmitted: a receiver of the other profile sees a CRC that does not check out.
 */
export function buildHeader(magic, length, bandId, set = 0, profile = 'normal') {
  const version = versionByte(bandId, set);
  return [magic, length & 255, crc8([magic, length & 255, version, profile === 'robust' ? 1 : 0]), version];
}

/** Preamble row k: the base pattern with the column shifted and the nibble offset added according to the band and profile key. */
export function preambleSymbol(bandId, k, profile = 'normal') {
  const [shift, add] = P4.preambleKeys[profile][bandId];
  const row = P4.preamble[k];
  return Array.from({ length: P4.bands[bandId].groups }, (_, g) => (row[(g + shift) % row.length] + add) & 15);
}

const assemble = (header, body, bandId, profile) => {
  const band = P4.bands[bandId];
  const pre = P4.preamble.map((_, k) => preambleSymbol(bandId, k, profile));
  const symbols = pre.concat(toSymbols(header, band.groups), toSymbols(body, band.groups));
  return { header, body, symbols, frameSeconds: symbols.length * band.sym * (profile === 'robust' ? 2 : 1) };
};

/**
 * v4 frame as symbols. `mode`: reference (fingerprint) or payload (complete text). `set` (0 = A, 1 = B) selects the parity
 * set of the reference. `profile`: normal, or robust (top, high, upper and mid only), which doubles symbol, window and step.
 * The frequency of each tone is (bin + group × 16 + nibble) × 46.875 Hz.
 */
export function buildFrame(text, mode, bandId, set = 0, profile = 'normal') {
  let header, body = [];
  if (mode === 'reference') {
    const content = sha256(utf8(text)).slice(0, P4.refBytes);
    const c = crc16(content);
    const data = content.concat([c >> 8, c & 255]);
    const mother = RS.encode(data, P4.motherParity);
    const parity = mother.slice(data.length + set * P4.refParity, data.length + (set + 1) * P4.refParity);
    body = data.concat(parity);
    header = buildHeader(P4.magic.reference, data.length, bandId, set, profile);
  } else {
    const content = pack(text);
    const c = crc32(content);
    const data = content.concat([(c >>> 24) & 255, (c >>> 16) & 255, (c >>> 8) & 255, c & 255]);
    const total = Math.ceil(data.length / P4.blockData);
    for (let b = 0; b < total; b++) {
      const chunk = data.slice(b * P4.blockData, (b + 1) * P4.blockData);
      body.push(...RS.encode([b, total].concat(chunk), P4.blockParity));
    }
    header = buildHeader(P4.magic.payload, total, bandId, 0, profile);
  }
  return assemble(header, body, bandId, profile);
}

/**
 * Risk frame (0x58): reason, source and hops, short own fingerprint (3 bytes) and short intruder fingerprint (3 bytes), with CRC-16
 * and 8 parity bytes. Reason: 1 another transmission heard, 2 copy of our own transmission, 3 overlapping frames, 4 cancelled by the user.
 * Second byte: source (0 sender, 1 receiver) in the low 4 bits and hops in the high 4; each retransmission adds one and at
 * `riskMaxHops` nobody retransmits any more. Always the normal profile.
 */
export function buildRisk(reason, source, ownShort, intruderShort, bandId, hops = 0) {
  const data = [reason, (hops << 4) | source, ...ownShort, ...intruderShort];
  const c = crc16(data);
  const withCrc = data.concat([c >> 8, c & 255]);
  return assemble(buildHeader(P4.magic.risk, withCrc.length, bandId), RS.encode(withCrc, 8), bandId, 'normal');
}

/**
 * v4 ack (0x41): type (2 ack, 3 receipt) and the first 4 bytes of the payload's SHA-256, with CRC-16 and 8 parity bytes.
 * Always the normal profile.
 */
export function buildAck(text, receipt, bandId) {
  const data = [receipt ? 3 : 2, ...sha256(utf8(text)).slice(0, 4)];
  const c = crc16(data);
  const withCrc = data.concat([c >> 8, c & 255]);
  return assemble(buildHeader(P4.magic.ack, withCrc.length, bandId), RS.encode(withCrc, P4.ackParity), bandId, 'normal');
}

const EXAMPLE = '00020101021226470017com.example.pagos01220000003100099988877766520454115303032540812500.505802AR5918ALMACEN LA ESQUINA6007CORDOBA62250108F-0001230509PEDIDO-776304F4AF';
const LONG = EXAMPLE + EXAMPLE.slice(0, 40) + ' ñandú 🙂 ' + EXAMPLE.slice(40, 120);
const cases = [];
for (const [name, text] of [['ejemplo', EXAMPLE], ['largo', LONG]]) {
  for (const band of Object.keys(P4.bands)) {
    cases.push({ name, text, mode: 'reference', band, set: 0 });
    cases.push({ name, text, mode: 'reference', band, set: 1 });
    cases.push({ name, text, mode: 'payload', band, set: 0 });
  }
  for (const band of Object.keys(P4.preambleKeys.robust)) {
    cases.push({ name, text, mode: 'reference', band, set: 0, profile: 'robust' });
    cases.push({ name, text, mode: 'payload', band, set: 0, profile: 'robust' });
  }
}
const hex = (a) => a.map((b) => b.toString(16).padStart(2, '0')).join('');
const vectors = cases.map((c) => {
  const f = buildFrame(c.text, c.mode, c.band, c.set, c.profile);
  return { ...c, profile: c.profile ?? 'normal', header: hex(f.header), body: hex(f.body), symbols: f.symbols, frameSeconds: Number(f.frameSeconds.toFixed(6)) };
});
const own = sha256(utf8(EXAMPLE)).slice(0, 3), other = sha256(utf8(EXAMPLE.replace('12500.50', '99999.99'))).slice(0, 3);
const riskCases = [];
for (const band of ['top', 'mid', 'xhigh', 'audible']) {
  for (const [reason, source] of [[1, 0], [2, 0], [3, 1]]) riskCases.push({ band, reason, source, own, intruder: reason === 2 ? own : other });
  riskCases.push({ band, reason: 1, source: 0, own, intruder: [0, 0, 0] });
  riskCases.push({ band, reason: 3, source: 1, own, intruder: other, hops: 1 });
  riskCases.push({ band, reason: 1, source: 0, own, intruder: other, hops: 2 });
}
const ackCases = [];
for (const band of ['mid', 'high']) for (const receipt of [false, true]) ackCases.push({ band, receipt, text: EXAMPLE });
if (process.argv[1]?.endsWith('v4-reference.mjs')) {
  writeFileSync(new URL('../test-vectors/v4-risk.json', import.meta.url), JSON.stringify({ version: 4, vectors: riskCases.map((c) => { const f = buildRisk(c.reason, c.source, c.own, c.intruder, c.band, c.hops ?? 0); return { ...c, hops: c.hops ?? 0, header: hex(f.header), body: hex(f.body), symbols: f.symbols, frameSeconds: Number(f.frameSeconds.toFixed(6)) }; }) }, null, 1) + '\n');
  console.log(`${riskCases.length} risk vectors`);
  writeFileSync(new URL('../test-vectors/v4-acks.json', import.meta.url), JSON.stringify({ version: 4, vectors: ackCases.map((c) => { const f = buildAck(c.text, c.receipt, c.band); return { ...c, header: hex(f.header), body: hex(f.body), symbols: f.symbols, frameSeconds: Number(f.frameSeconds.toFixed(6)) }; }) }, null, 1) + '\n');
  console.log(`${ackCases.length} ack vectors`);
  writeFileSync(new URL('../test-vectors/v4-frames.json', import.meta.url), JSON.stringify({ version: 4, bands: P4.bands, channelIds: P4.channelIds, preambleKeys: P4.preambleKeys, parity: { set: P4.refParity, mother: P4.motherParity, block: P4.blockParity, blockData: P4.blockData }, vectors }, null, 1) + '\n');
  console.log(`${vectors.length} vectors`);
}
