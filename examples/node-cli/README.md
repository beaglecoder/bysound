# node-cli

Text to WAV and WAV to text with the TypeScript library, no audio hardware. The script imports `../../web/src` and is bundled to plain `.mjs` with web's Vite (`ssr` build), so there is nothing to install beyond `npm ci` in `../../web`.

```sh
cd examples
npm run -s node:encode -- "hello" -o out/hello.wav --band mid --profile robust   # --rate 44100, --level 0.5, --lead 0.2
npm run -s node:decode -- out/hello.wav
# out/hello.wav: band=mid profile=robust corrected=0 text=hello
```

Or build once (`npm run build:node`) and call `node dist-node/cli.mjs encode|decode ...` directly. WAVs written here decode with `c/build/bysound decode` and the C `batch-decode`, and the other way round. Text too long for 30 s exits 1 with a message (`TOO_LONG`).
