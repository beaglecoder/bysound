# bysound examples

Richer, complete examples than the minimal demos. All local: no API keys and no cloud services (backend-reference runs its own tiny local server).

| Example | Platform | Status | Why it is useful |
|---|---|---|---|
| [web-wifi-setup](web-wifi-setup/) | web (TS) | runnable | Give a screenless device Wi-Fi credentials by sound, with a one-time-code protection and an honest "sound is not secret" warning |
| [esp32-wifi-setup](esp32-wifi-setup/) | ESP32 (C) | builds (ESP-IDF 5.4, ESP32-S3); not run on a board | The device half of web-wifi-setup: decrypts, joins the network, stores it, acknowledges by sound |
| [store-presence](store-presence/) | web (TS) + ESP32 (C) | web runnable; ESP32 builds, not run on a board | Check-in at a shop without QR or Bluetooth: a broadcast beacon with rotating HMAC tokens, and the customer page that verifies and rejects replays |
| [backend-reference](backend-reference/) | web (TS) + Node | runnable (local server, no keys) | Only a 12-byte fingerprint travels by sound; the payment request, the sender's identity and the blocking of suspicious messages live on a backend — with a rogue-sender page to see the attack detected and blocked |
| [ios-share-link](ios-share-link/) | iOS (SwiftUI) | runnable (builds; try on two iPhones) | Send a link between nearby phones with the session layer; *Open* only after `confirmed()` and only for validated http(s) URLs |
| [android-share-contact](android-share-contact/) | Android (Kotlin) | runnable (builds; try on two phones) | Send a contact as a vCard, strictly parsed, added through `ACTION_INSERT` with no contacts permission |
| [web-channel-ladder](web-channel-ladder/) | web (TS) | runnable | See the automatic channel ladder (top, high, upper, mid) pick and fall back, including an offline "broken tweeter" simulation |
| [c-wav-tools](c-wav-tools/) | host (C) | runnable | Decode a folder of recordings into a table and sweep noise levels with the C library: the quickest way to check a field recording |

## Running

The TypeScript examples import `../web/src` directly (nothing is published to npm) and use web's toolchain, so install it once:

```sh
cd web && npm ci
cd ../examples
npm run dev:web-wifi-setup      # also dev:web-channel-ladder, dev:store-presence
npm run build:web               # production build of the three (to <example>/dist)
npm run typecheck               # tsc over all the examples
cd c-wav-tools && make demo     # needs a C compiler; see c-wav-tools/
cd ../backend-reference && npm test && npm run server   # then npm run dev in another terminal
```

The microphone needs `localhost` or HTTPS; to try from a second device, serve over HTTPS (e.g. a tunnel) or use the loopback buttons. Audio can only start from a click, and the speaker should be loud and not Bluetooth/headphones.

CI (`.github/workflows/ci.yml`, job `examples` and the Swift and Android jobs) typechecks and builds the TypeScript examples, runs `make demo` in `c-wav-tools` and builds `ios-share-link` and `android-share-contact`. `esp32-wifi-setup` and `store-presence/esp32-beacon` are built with ESP-IDF by hand (see their READMEs).
