// Offline round-trip test of the reference backend: publish → resolve → single use → expiry → blocked.
// Run: node test-server.mjs   (no network beyond 127.0.0.1, no dependencies)
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { createBackend, fingerprintOf } from './server.mjs';

let clock = 1_700_000_000_000;
const server = createBackend({ ttlMs: 60_000, now: () => clock });
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}`;

async function call(user, method, path, body) {
  const res = await fetch(base + path, {
    method,
    headers: { ...(user ? { 'x-demo-user': user } : {}), ...(body ? { 'content-type': 'application/json' } : {}) },
    body: body ? JSON.stringify(body) : undefined,
  });
  return { status: res.status, body: await res.json() };
}
const request = (merchant, amount) => JSON.stringify({ type: 'payment-request', merchant, amount, currency: 'EUR', nonce: randomBytes(12).toString('hex') });
const publish = (user, payload, fingerprint = fingerprintOf(payload)) => call(user, 'POST', '/publish', { fingerprint, payload });
let passed = 0;
const step = (name) => { passed++; console.log(`ok ${passed} - ${name}`); };

try {
  // Authentication and validation.
  const p1 = request('Shop 7', '12.50');
  const fp1 = fingerprintOf(p1);
  assert.equal((await publish(null, p1)).status, 401);
  assert.equal((await publish('shop-7', p1, '0'.repeat(24))).status, 400, 'the server recomputes the fingerprint');
  step('unauthenticated and forged fingerprints are refused');

  // Publish, idempotent.
  const a = await publish('shop-7', p1);
  assert.equal(a.status, 200);
  assert.equal(a.body.fingerprint, fp1);
  const again = await publish('shop-7', p1);
  assert.equal(again.status, 200);
  assert.equal(again.body.repeated, true);
  assert.equal(again.body.expiresAt, a.body.expiresAt, 'a retry does not extend the TTL');
  assert.equal((await publish('mallory', p1)).status, 409, 'another account cannot take over a fingerprint');
  step('publish is idempotent for its publisher');

  // Resolve: does not consume, returns the verified publisher.
  const r1 = await call('alice', 'GET', `/resolve/${fp1}`);
  assert.equal(r1.status, 200);
  assert.equal(r1.body.payload, p1);
  assert.equal(r1.body.publisher, 'shop-7');
  assert.equal((await call('alice', 'GET', `/resolve/${fp1}`)).status, 200, 'resolving twice does not consume');
  assert.equal((await call('alice', 'GET', `/resolve/${'a'.repeat(24)}`)).status, 404);
  step('resolve returns payload and verified publisher, does not consume');

  // Received: single use, and the publisher sees it.
  assert.equal((await call('shop-7', 'POST', '/received', { fingerprint: fp1 })).status, 403, 'the publisher cannot mark its own request');
  const rec = await call('alice', 'POST', '/received', { fingerprint: fp1 });
  assert.equal(rec.status, 200);
  assert.equal(rec.body.publisher, 'shop-7');
  assert.equal((await call('bob', 'POST', '/received', { fingerprint: fp1 })).status, 410, 'second use refused');
  assert.equal((await call('alice', 'GET', `/resolve/${fp1}`)).status, 410, 'a replayed fingerprint no longer resolves');
  const st = await call('shop-7', 'GET', `/status/${fp1}`);
  assert.deepEqual([st.body.state, st.body.receivedBy], ['consumed', 'alice']);
  assert.equal((await call('mallory', 'GET', `/status/${fp1}`)).status, 404, 'only the publisher sees the status');
  assert.equal((await publish('shop-7', p1)).status, 410, 'a consumed request cannot be published again');
  step('received is single use and reaches the publisher');

  // Expiry.
  const p2 = request('Shop 7', '3.00');
  const fp2 = fingerprintOf(p2);
  assert.equal((await publish('shop-7', p2)).status, 200);
  clock += 59_999;
  assert.equal((await call('alice', 'GET', `/resolve/${fp2}`)).status, 200);
  clock += 1;
  assert.equal((await call('alice', 'GET', `/resolve/${fp2}`)).status, 410);
  assert.equal((await call('alice', 'POST', '/received', { fingerprint: fp2 })).status, 410);
  assert.equal((await call('shop-7', 'GET', `/status/${fp2}`)).body.state, 'expired');
  step('expires after the TTL');

  // Blocking after a clash: the real request (full fingerprint) and the rogue one (only its short id, from the risk alert).
  const real = request('Shop 7', '12.50');
  const rogue = request('Shop 7', '99.00'); // the rogue claims the same merchant name, but publishes under its own account
  const fpReal = fingerprintOf(real), fpRogue = fingerprintOf(rogue);
  await publish('shop-7', real);
  await publish('mallory', rogue);
  assert.equal((await call('alice', 'GET', `/resolve/${fpRogue}`)).body.publisher, 'mallory', 'the verified publisher shows who really published it');
  const unpublished = 'b'.repeat(24);
  const rep = await call('alice', 'POST', '/report-risk', { reason: 'MULTIPLE_TRANSMISSIONS', fingerprints: [fpReal, unpublished], shortIds: [fpRogue.slice(0, 6), '000000'] });
  assert.equal(rep.status, 200);
  assert.deepEqual(new Set(rep.body.blocked), new Set([fpReal, fpRogue, unpublished]));
  assert.equal((await call('alice', 'GET', `/resolve/${fpReal}`)).status, 423);
  assert.equal((await call('alice', 'GET', `/resolve/${fpRogue}`)).status, 423);
  assert.equal((await call('alice', 'POST', '/received', { fingerprint: fpReal })).status, 423);
  assert.equal((await call('shop-7', 'GET', `/status/${fpReal}`)).body.state, 'blocked', 'the real sender learns it must issue a new request');
  assert.equal((await publish('mallory', rogue)).status, 423);
  const fresh = request('Shop 7', '12.50');
  assert.equal((await publish('shop-7', fresh)).status, 200, 'a new request (new nonce) works again');
  step('report-risk blocks the fingerprints involved, also by short id and unpublished ones');

  // Audit log.
  const log = (await call('alice', 'GET', '/audit')).body.audit;
  assert.ok(log.some((l) => l.event === 'report-risk' && l.account === 'alice'));
  assert.ok(log.some((l) => l.event === 'received' && l.fingerprint === fp1 && l.publisher === 'shop-7'));
  step('every step is in the audit log');

  // Rate limit.
  const limitedServer = createBackend({ rateLimit: { perMinute: 3 }, now: () => clock });
  await new Promise((r) => limitedServer.listen(0, '127.0.0.1', r));
  const lb = `http://127.0.0.1:${limitedServer.address().port}`;
  const codes = [];
  for (let i = 0; i < 4; i++) codes.push((await fetch(`${lb}/resolve/${fp1}`, { headers: { 'x-demo-user': 'scanner' } })).status);
  limitedServer.close();
  assert.deepEqual(codes, [404, 404, 404, 429]);
  step('resolve is rate limited per account');

  console.log(`\nall ${passed} checks passed`);
} finally {
  server.close();
}
