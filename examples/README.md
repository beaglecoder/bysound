# bysound examples

Richer, complete examples than the minimal demos. All local: no API keys, no backend, no network.

| Example | Platform | Status | What it shows |
|---|---|---|---|
| [web-chat](web-chat/) | browser (TS) | runnable | Two devices chatting by sound with `sendSession`/`receiveSession`: "delivered on <band>" per message, "two senders" handling, the confirm gate |
| [web-file](web-file/) | browser (TS) | runnable | A small file as base64 text, per-band size limit (`maxPayloadBytes`), progress, SHA-256 check on the receiver, loopback test |
| [web-wifi-setup](web-wifi-setup/) | browser (TS) | runnable | Wi-Fi credentials in the `WIFI:` QR syntax, a clear "sound is not secret" warning and a one-time-code protection (AES-GCM) with its limits; simulated device |
| [web-channel-ladder](web-channel-ladder/) | browser (TS) | runnable | The automatic ladder top, high, upper, mid: per-repetition channel, where it was read, and an offline "broken tweeter" simulation converging to mid |
| [c-wav-tools](c-wav-tools/) | host (C) | runnable | `batch-decode` (table for a folder of recordings) and `noise-test` (decode under added noise, SNR sweep) on `../c` |
| [node-cli](node-cli/) | Node (TS) | runnable | `encode` text to WAV and `decode` WAV with the TS library, no hardware |
| [music-variants](music-variants/) | Node (TS) | runnable | The music + hidden message demo at several channels and message levels, WAVs and a decode report |
| [ios-share-link](ios-share-link/) | iOS (SwiftUI) | runnable (builds; try on two iPhones) | Send a URL with the session (band auto); on receive, `confirmed()` then *Open* only for validated http(s) URLs, host shown |
| [android-share-contact](android-share-contact/) | Android (Kotlin) | runnable (builds; try on two phones) | Send a contact as a vCard 3.0, strict parse on receive, `confirmed()`, then *Add to contacts* via `ACTION_INSERT` (no contacts permission) |
| [esp32-wifi-setup](esp32-wifi-setup/) | ESP32 (C) | TODO (needs C session layer) | Device side of web-wifi-setup: code on screen, decrypt, join Wi-Fi, ack by sound |

## Running

The TypeScript examples import `../web/src` directly (nothing is published to npm) and use web's toolchain, so install it once:

```sh
cd web && npm ci
cd ../examples
npm run dev:web-chat            # also dev:web-file, dev:web-wifi-setup, dev:web-channel-ladder
npm run build:web               # production build of all four (to <example>/dist)
npm run typecheck               # tsc over all the examples
npm run -s node:decode -- some.wav        # see node-cli/
cd c-wav-tools && make demo     # needs a C compiler; see c-wav-tools/
```

The microphone needs `localhost` or HTTPS; to try from a second device, serve over HTTPS (e.g. a tunnel) or use the loopback buttons. Audio can only start from a click, and the speaker should be loud and not Bluetooth/headphones.

Some examples import modules that the public `web/src/index.ts` does not re-export yet (`maxPayloadBytes`, `mix`, `renderMelody`); each says so in a comment.
