# ios-share-link (TODO)

**Status: planned.** Waits for the iOS session layer (`sendSession`/`receiveSession` equivalents in Swift, docs/SESSION.md) to land.

Planned example: a small SwiftUI app. **Send:** paste or share-sheet a URL, send it by sound as a session (ack, `delivered on <band>`). **Receive:** listen, show the URL with its band, run it through the confirm gate (`confirmed()`), then offer *Open* (`UIApplication.open`) only for `https`/`http` URLs and show the host prominently before opening, since sound is not authenticated. Use the Share Extension to send from Safari. No network, no keys.
