# web-file

Send a small file by sound: `BSF1|name|size|sha256|base64` as one text, SHA-256 checked on the receiver ("✓ SHA-256 verified" or "✗ HASH MISMATCH"), with a download link.

```sh
cd examples && npm run dev:web-file      # open on two devices: Listen on one, pick a file and Send on the other
```

- The page shows how many bytes fit on the selected band and profile (`maxPayloadBytes`; e.g. `mid` normal = 1052 characters, roughly 700 bytes of file, `top` robust = 335). One transmission is capped at 30 s; `encode` throws `TOO_LONG` otherwise, which the page reports.
- Progress: the sender's bar follows the playing time of one repetition; the receiver's bar is `LevelEvent.progress` from `listen()` (and `blocksTotal` once the header is read). Tick *session mode* to use `sendSession` / `receiveSession` instead: you then get the acknowledgement, but the receiver has no progress bar (`receiveSession` only exposes `onLevel(rms)`).
- **Loopback test (no audio)** encodes, decodes and verifies inside the page: try it first.

Not secret and not private: see `../web-wifi-setup` for the honest version of that.
