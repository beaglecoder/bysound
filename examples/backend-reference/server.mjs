// A tiny reference backend for bysound's reference mode (docs/SECURITY.md, "Using a backend").
// Dependency-free: node:http, node:crypto and an in-memory Map. For a local demo only: no TLS, no real accounts, no persistence.
//
//   node server.mjs            # listens on http://localhost:8787 (PORT to change it)
//
// Endpoints (all need the demo identity header `x-demo-user: <account>`; a real backend derives it from a session token):
//   POST /publish       {fingerprint, payload}   store the payload under its fingerprint (idempotent, TTL, single use)
//   GET  /resolve/:fp                            read-only: the payload and the verified publisher (does not consume it)
//   POST /received      {fingerprint, band}      consume it, once, after the receiver's confirm gate; tells the publisher
//   POST /report-risk   {fingerprints, shortIds, reason}   block fingerprints involved in a clash or an alert
//   GET  /status/:fp                             the publisher's view: live / consumed / expired / blocked (the "backend ack")
//   GET  /audit                                  the audit log (demo only: a real one is not public)
import { createHash } from 'node:crypto';
import { createServer as httpServer } from 'node:http';
import { fileURLToPath } from 'node:url';

const HEX24 = /^[0-9a-f]{24}$/;
const HEX6 = /^[0-9a-f]{6}$/;
const MAX_PAYLOAD_BYTES = 2048;

/** The fingerprint bysound plays in a reference frame: the first 12 bytes of SHA-256 of the UTF-8 text, as 24 hex (docs/SESSION.md §3.3). */
export const fingerprintOf = (text) => createHash('sha256').update(text, 'utf8').digest('hex').slice(0, 24);

/**
 * @param {{ ttlMs?: number, now?: () => number, rateLimit?: { perMinute: number }, log?: (line: string) => void }} [options]
 */
export function createBackend(options = {}) {
  const ttlMs = options.ttlMs ?? 60_000;
  const now = options.now ?? Date.now;
  const perMinute = options.rateLimit?.perMinute ?? 60;
  const log = options.log ?? (() => undefined);

  /** fingerprint → { payload, publisher, createdAt, expiresAt, state: 'live' | 'consumed', receivedBy, blockedReason } */
  const entries = new Map();
  /** Fingerprints refused from now on, also ones never published (a rogue's). */
  const blocked = new Map();
  /** account → timestamps of its recent requests (a fixed one-minute window). */
  const hits = new Map();
  const audit = [];

  const record = (event, account, fingerprint, detail = {}) => {
    const line = { at: new Date(now()).toISOString(), event, account, fingerprint, ...detail };
    audit.push(line);
    if (audit.length > 1000) audit.shift();
    log(`${line.at} ${event} by ${account} ${fingerprint ?? ''} ${JSON.stringify(detail)}`);
  };

  const stateOf = (fp) => {
    if (blocked.has(fp)) return 'blocked';
    const e = entries.get(fp);
    if (!e) return 'unknown';
    if (e.state === 'consumed') return 'consumed';
    return now() >= e.expiresAt ? 'expired' : 'live';
  };

  const limited = (account) => {
    const t = now();
    const recent = (hits.get(account) ?? []).filter((x) => t - x < 60_000);
    recent.push(t);
    hits.set(account, recent);
    return recent.length > perMinute;
  };

  // Forget entries well after they expired (a real store would use its own TTL index).
  const sweep = setInterval(() => {
    for (const [fp, e] of entries) if (now() > e.expiresAt + 10 * 60_000) entries.delete(fp);
  }, 30_000);
  sweep.unref();

  /** One request → [status, body]. Kept apart from HTTP so the logic reads top to bottom. */
  function handle(method, path, account, body) {
    if (!account) return [401, { error: 'unauthenticated', hint: 'send x-demo-user: <account>' }];
    if (limited(account)) return [429, { error: 'rate-limited' }];

    if (method === 'POST' && path === '/publish') {
      const { fingerprint, payload } = body ?? {};
      if (typeof payload !== 'string' || Buffer.byteLength(payload) > MAX_PAYLOAD_BYTES) return [400, { error: 'bad-payload' }];
      // Never trust the client's fingerprint: recompute it from the payload.
      const fp = fingerprintOf(payload);
      if (fingerprint !== fp) return [400, { error: 'fingerprint-mismatch' }];
      if (blocked.has(fp)) return [423, { error: 'blocked', reason: blocked.get(fp) }];
      const existing = entries.get(fp);
      if (existing) {
        // Idempotent: the same publisher retrying the same payload gets the same answer, without extending the TTL.
        if (existing.publisher !== account) return [409, { error: 'taken' }];
        if (stateOf(fp) !== 'live') return [410, { error: stateOf(fp) }];
        return [200, { ok: true, fingerprint: fp, expiresAt: existing.expiresAt, repeated: true }];
      }
      const e = { payload, publisher: account, createdAt: now(), expiresAt: now() + ttlMs, state: 'live', receivedBy: null };
      entries.set(fp, e);
      record('publish', account, fp);
      return [200, { ok: true, fingerprint: fp, expiresAt: e.expiresAt }];
    }

    let m = path.match(/^\/resolve\/([0-9a-f]+)$/);
    if (method === 'GET' && m) {
      const fp = m[1];
      if (!HEX24.test(fp)) return [400, { error: 'bad-fingerprint' }];
      const state = stateOf(fp);
      record('resolve', account, fp, { state });
      if (state === 'unknown') return [404, { error: 'not-found' }];
      if (state === 'blocked') return [423, { error: 'blocked', reason: blocked.get(fp) }];
      if (state !== 'live') return [410, { error: state }];
      const e = entries.get(fp);
      // Read-only: resolving does not consume. The receiver may still reject this reception (a clash in its final listen).
      return [200, { payload: e.payload, publisher: e.publisher, publishedAt: e.createdAt, expiresAt: e.expiresAt }];
    }

    if (method === 'POST' && path === '/received') {
      const fp = body?.fingerprint;
      if (typeof fp !== 'string' || !HEX24.test(fp)) return [400, { error: 'bad-fingerprint' }];
      const state = stateOf(fp);
      if (state === 'unknown') return [404, { error: 'not-found' }];
      if (state === 'blocked') return [423, { error: 'blocked', reason: blocked.get(fp) }];
      if (state !== 'live') return [410, { error: state }];
      const e = entries.get(fp);
      if (e.publisher === account) return [403, { error: 'own-request' }];
      // Single use: check and set in one synchronous step (a database would use a conditional update).
      e.state = 'consumed';
      e.receivedBy = account;
      record('received', account, fp, { publisher: e.publisher, band: typeof body.band === 'string' ? body.band.slice(0, 16) : null });
      return [200, { ok: true, publisher: e.publisher }];
    }

    if (method === 'POST' && path === '/report-risk') {
      const reason = typeof body?.reason === 'string' ? body.reason.slice(0, 40) : 'unspecified';
      const fps = (Array.isArray(body?.fingerprints) ? body.fingerprints : []).filter((x) => typeof x === 'string' && HEX24.test(x));
      const shorts = (Array.isArray(body?.shortIds) ? body.shortIds : []).filter((x) => typeof x === 'string' && HEX6.test(x) && x !== '000000');
      const hit = new Set(fps);
      // A risk frame carries only 3-byte short ids: block the live entries they point to.
      for (const s of shorts) for (const [fp] of entries) if (fp.startsWith(s) && stateOf(fp) === 'live') hit.add(fp);
      // Consumed entries stay consumed; everything else involved is refused from now on.
      const done = [...hit].filter((fp) => stateOf(fp) !== 'consumed');
      for (const fp of done) blocked.set(fp, reason);
      record('report-risk', account, null, { reason, blocked: done });
      return [200, { blocked: done }];
    }

    m = path.match(/^\/status\/([0-9a-f]{24})$/);
    if (method === 'GET' && m) {
      const e = entries.get(m[1]);
      if (!e || e.publisher !== account) return [404, { error: 'not-found' }];
      return [200, { state: stateOf(m[1]), receivedBy: e.receivedBy, blockedReason: blocked.get(m[1]) ?? null, expiresAt: e.expiresAt }];
    }

    if (method === 'GET' && path === '/audit') return [200, { audit }];
    return [404, { error: 'no-such-endpoint' }];
  }

  const server = httpServer((req, res) => {
    const account = String(req.headers['x-demo-user'] ?? '').trim().slice(0, 40) || null;
    const path = (req.url ?? '/').split('?')[0];
    const chunks = [];
    let size = 0;
    req.on('data', (c) => { size += c.length; if (size <= 16_384) chunks.push(c); });
    req.on('end', () => {
      let body = null;
      if (chunks.length) {
        try { body = JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch { body = null; }
      }
      const [status, out] = size > 16_384 ? [413, { error: 'too-large' }] : handle(req.method ?? 'GET', path, account, body);
      res.writeHead(status, { 'content-type': 'application/json', 'cache-control': 'no-store' });
      res.end(JSON.stringify(out));
    });
  });
  server.on('close', () => clearInterval(sweep));
  return server;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const port = Number(process.env.PORT ?? 8787);
  createBackend({ log: (l) => console.log(l) }).listen(port, '127.0.0.1', () => {
    console.log(`bysound reference backend on http://localhost:${port} (TTL 60 s, single use, in memory)`);
  });
}
