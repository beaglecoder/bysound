# music-variants

Renders the "music + hidden message" demo (melody from `web/src/music.ts`, message looped under it with `mix`) with several channels and message levels into WAVs, and reports which ones decode. Level is the peak of the message alone (the web demo uses `top` at 0.5).

```sh
cd examples && npm run -s music-variants                       # default set: top 0.5/0.3/0.15/0.08, mid 0.5/0.3/0.15, high, upper at 0.3
npm run -s music-variants -- top:0.05 mid:0.1 --text "my text" --rate 44100
```

WAVs go to `out/music-variants/` (play them on a phone and decode what the room hears: that is the real test; the offline decode here is the lossless case). Table columns: variant, decoded yes/no, band it was read on, corrected bytes.
