# store-presence

Presence / check-in at a shop by sound. A beacon near the till (or the door) broadcasts a tiny rotating token on the `top` channel (18–19.5 kHz, inaudible to most adults); a customer's phone listens, shows "📍 You are at Café Demo (verified, 12 s ago)" and offers a check-in.

**Use cases:** loyalty check-in without a QR code to print or a Bluetooth permission; a coupon that only unlocks when you are in the shop; joining a queue ("I am here") without scanning anything.

| File | What it is |
|---|---|
| [`beacon.html`](beacon.html) / `beacon.ts` | Web beacon: from a click, plays a fresh token every few seconds on `top` |
| [`customer.html`](customer.html) / `customer.ts` | Customer page: listens, verifies, shows the store, check-in button, rejected tokens with the reason; a no-audio self-test |
| [`token.ts`](token.ts) | Token format, HMAC (WebCrypto) and the verifier |
| [`esp32-beacon/`](esp32-beacon/) | The same beacon on an ESP32-S3 (ESP-IDF, mbedTLS HMAC, SNTP or a fixed epoch) |

## The token

```
P1|<storeId>|<window>|<mac>        P1|cafe42|58123456|54ba1d6aad99     (31 characters, about 2.1 s on `top`)
window = floor(unix_time / 30)
mac    = first 6 bytes (12 hex characters) of HMAC-SHA256(storeKey, "<storeId>|<window>")
```

Test vector (both implementations must give it): key `demo-store-key-do-not-use-in-production`, store `cafe42`, window `58123456` gives mac `54ba1d6aad99`.

It is sent with the plain modem (`encode`/`send`, no session): it is a broadcast, nobody acknowledges it, and the beacon never listens.

### Why rotating

A static "I am at store X" sound could be recorded once and replayed from home, any day. With a window and a MAC, a recording is only accepted while its window is current: the verifier accepts the window of its own clock **±1** (so a token is good for 30 to 90 s). A recording replayed later is useless.

What it does **not** give you, said plainly:

- **Replay inside the 30-90 s window is possible.** Someone who records the sound and plays it to a friend within a minute, near enough for the friend's phone, passes. A relay over the internet (record here, stream there, play within seconds) also passes. This is presence evidence, not proof; for a coupon worth money add something per customer (a login, a rate limit, a one-per-day rule).
- **"Already used" is per verifier memory.** A verifier rejects a `(storeId, window, mac)` it already accepted for a check-in, but only if it saw it. Two verifiers that do not share memory accept the same token once each. Use one backend with one store of used tokens.
- **The clock matters.** A beacon whose clock is more than about a minute off is rejected as "too old" or "from the future".
- **Anyone can hear it and read it**, as with any bysound message. The token is not secret; it is only unforgeable without the key.

### Where verification really happens

In a real deployment the phone **does not hold `storeKey`**. It forwards the token it heard to the store's backend (`POST /checkin { token }`), which holds the key, checks the MAC and the window, remembers used tokens, and answers with the coupon or the queue ticket. The key lives in the backend and in the beacon's flash, never in an app.

In this demo the customer page verifies by itself with a **shared demo secret** that is written in the page, only so the example runs offline with no server. Treat that field as what it is: a demo.

## Run the web pages

```sh
cd ../../web && npm ci            # once
cd ../examples
npm run dev:store-presence        # http://localhost:5173/  (pages: beacon.html, customer.html)
npm run build:web                 # also builds store-presence to store-presence/dist
```

Open the beacon on one device and the customer page on another (the microphone needs `localhost` or HTTPS; a tunnel works). On the beacon: Start, volume up, speaker (not headphones). On the customer: Listen. To see every rejection without audio press "Try the rejections (no audio)". Both pages must use the same store key; the customer only accepts store ids listed in its "Known stores" box.

## ESP32 beacon

See [`esp32-beacon/README.md`](esp32-beacon/README.md).

## Limits

- Not tested in a real shop. The web pages were built and the token/verifier logic was checked in Node, and the ESP32 firmware builds (ESP-IDF 5.4, ESP32-S3, both time sources); it was not flashed or heard on a board.
- Range is a few meters at best and depends entirely on the speaker (many cannot reproduce `top` loudly), the phone's microphone and the room; a counter with a person and a till in between helps nobody. Choose another channel (`high`, `upper`, `mid`) if `top` does not reach.
- Sound reaches the whole room, including people outside a glass wall near the speaker: it is "near this beacon", not "inside the shop".
