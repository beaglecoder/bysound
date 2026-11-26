# backend-reference

bysound's **reference mode with a backend**: a shop asks for a payment, and only a 12-byte fingerprint travels by sound. The amount, the merchant and **who published the request** come from a tiny local server. The design, and every attack shown here, is explained in [docs/SECURITY.md, "Using a backend"](../../docs/SECURITY.md#using-a-backend).

Everything runs on your computer: no API keys, no cloud, no dependencies beyond `../../web`'s toolchain (run `npm ci` in `../../web` once).

```sh
cd examples/backend-reference
npm run server     # terminal 1: the backend on http://localhost:8787 (node:http, in memory, TTL 60 s, single use)
npm run dev        # terminal 2: the pages on http://localhost:5173 (Vite proxies /api to the backend)
```

Open http://localhost:5173 and, from there, the **Sender**, **Receiver** and **Rogue** pages, each in its own browser **window** (not a background tab: browsers slow down timers in hidden tabs). One computer is enough: its speaker reaches its own microphone. Speaker volume high. To use phones, serve the pages over HTTPS (browsers allow the microphone only on HTTPS or localhost) and point them at the same backend.

`npm test` runs the backend round trip offline (publish → resolve → single use → expiry → blocked, plus rate limit and audit log); `npm run typecheck` and `npm run build` check the pages.

## What to watch

1. **Normal payment.** Receiver: *Listen*. Sender: *Request payment*. The sender publishes first and plays only if the backend stored it. The receiver resolves the fingerprint and shows *12.50 EUR to account shop-7 (verified by the backend)*. Press *Confirm*: the app calls `confirmed()` and only then `POST /received`. The sender's log shows the ack by sound (someone read it) and then the backend's answer (*paid by alice*). The backend terminal prints the audit log.
2. **Replay.** Record case 1 with a phone's voice recorder, press *Listen* again and play the recording back: the fingerprint decodes fine, the backend answers that it was already used (or expired after 60 s), and bysound fails with `NOT_FOUND`.
3. **Rogue at the same time.** Receiver: *Listen*. Sender: *Request payment*. About 5 s later (the sender listens 3–4 s before playing), Rogue: *Emit now*. The receiver gets `MULTIPLE_TRANSMISSIONS` and delivers nothing; the sender hears its risk frame and stops (`RISK_SIGNAL`). Both report to the backend, which **blocks** the fingerprints involved; the sender's log says *blocked, issue a new one*. If the two frames overlapped exactly, the receiver could not read either fingerprint; the sender's report blocks its own.
4. **Rogue after delivery.** Let the receiver show the request, then press *Emit now* on the rogue and only then *Confirm*: `confirmed()` fails, nothing is paid, both fingerprints are blocked.
5. **Rogue alone.** Only the rogue plays. Its payload says *Shop 7*, but the receiver shows **account mallory**: the backend knows who published it. This is the case bysound cannot detect by sound; the screen and the person are the defence.

If the rogue starts during the sender's pre-listen, the sender refuses to play (`OTHER_TRANSMISSION`) and only the rogue is heard: case 5.

## Files

| File | What |
|---|---|
| `server.mjs` | The backend: `POST /publish`, `GET /resolve/:fp`, `POST /received`, `POST /report-risk`, `GET /status/:fp`, `GET /audit` |
| `test-server.mjs` | Offline test of the backend (`npm test`) |
| `api.ts` | The pages' calls: `publish` (own timeout, idempotent), `fetchPayload` (read-only), `markReceived`, `reportRisk`, `status` |
| `sender.ts`, `receiver.ts`, `rogue.ts` | The three pages: `sendSession(payload, { mode: 'reference' })`, `receiveSession({ resolve })` with the confirm gate, and an attacker with every protection off |
| `vite.config.mjs`, `tsconfig.json`, `package.json` | Standalone setup that borrows `../../web`'s Vite and TypeScript |

## Honest limits

- **A demo, not a backend.** "Signing in" is a header (`x-demo-user`) anyone can set, there is no TLS, and the store is a `Map` that forgets everything on restart. A real backend authenticates with a session token over TLS, stores entries in a database with an atomic consume, and pushes the result to the sender instead of being polled.
- **The backend adds** confidentiality, the publisher's identity, expiry, single use, blocking and an audit log. **bysound adds** integrity and detection of a second emitter when a microphone hears it. Neither stops a **relay attack** (sound captured and replayed elsewhere in real time) or **jamming**; see SECURITY.md for the mitigations.
- Anyone signed in can call `/report-risk` and block live requests: a real backend rate limits it and reviews reports. Blocking never pays anyone; the sender issues a new request.
- Timing in the browser varies: whether the rogue's frame overlaps, falls in the final listen or arrives after delivery depends on when you press the button. The four outcomes were also checked on bysound's simulated air against this backend, not only by hand.
