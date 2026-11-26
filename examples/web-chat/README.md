# web-chat

Two browsers chatting by sound with the **session layer**. Every message is a `sendSession` / `receiveSession` exchange: the other side acknowledges by sound, and you see `✓ delivered on <band>` (where it was actually read, from the ack) per message. `⚠ two senders` is shown when the session reports `OTHER_TRANSMISSION`, `OTHER_EMITTER`, `CLONED_TRANSMISSION`, `MULTIPLE_TRANSMISSIONS` or `RISK_SIGNAL` (docs/SESSION.md section 8). The receiving side also calls the confirm gate (`confirmed()`) before trusting a message.

Run (needs `npm ci` once in `../../web`):

```sh
cd examples && npm run dev:web-chat      # http://localhost:5173, open it on two devices
```

Press **Start** on both (it opens the microphone), type, Send. Half duplex: while a device sends it does not listen; listening resumes right after. Speaker at full volume, not headphones.

Shows: `sendSession` with the default automatic ladder and `onRepetition`, `receiveSession` with `onHeard` / `onLevel`, `SessionError` codes, `confirmed()`.
Browser limits: audio can only start from a click; Safari/iOS may ask for another tap after a send before listening resumes.
