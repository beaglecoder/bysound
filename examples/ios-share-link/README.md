# ios-share-link

A minimal SwiftUI app: send a URL by sound, receive one, and open it only after the confirm gate and a check.

- **Send:** type a link, tap *Send by sound*. It runs a session with the band on automatic (the ladder picks it) and shows `Delivered on <band>` when the receiver acknowledges.
- **Receive:** tap *Listen*. When the data is in, the app calls `confirmed()` (throws if another emitter or a risk frame appeared meanwhile), then shows the text, the band it was read on and, if it is an `http`/`https` URL with a host, its host in bold and an *Open* button.

It uses the `BySound` package by relative path (`../..`, the root `Package.swift`), so the repo is all you need. No network, no keys, no signing team.

## Run

```sh
open examples/ios-share-link/ShareLink.xcodeproj      # pick a device, set your team once, Run (two iPhones to try both sides)
# or just check that it builds:
xcodebuild -project examples/ios-share-link/ShareLink.xcodeproj -scheme ShareLink -destination 'generic/platform=iOS Simulator' build CODE_SIGNING_ALLOWED=NO
```

The microphone and speaker only work on a real device; use a loud speaker, not headphones or Bluetooth.

## Honesty note

Sound is not secret and not authenticated: anyone in earshot can send you any text, and the confirm gate only says that no second emitter or risk frame was heard, not who sent it. So the app never opens what it heard blindly:

- only `http` and `https` URLs with a host are offered (`openableURL` in `ContentView.swift`); `tel:`, `sms:`, custom app schemes, `file:` and the like are shown as text but never opened;
- the host is shown before the button, because lookalike hosts are the usual trick;
- the sender checks the same rule before sending, and a real app would also let the person review the link on the receiving side, as this one does, before every open.

The link itself travels in the clear: do not send URLs with tokens or private paths.
