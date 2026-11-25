#!/bin/sh
# Fetches ggwave (MIT, Georgi Gerganov) at the exact commit the published results were produced with.
set -eu
COMMIT=060aec73dd7123ccac200442f75bdc7369795ffe   # v0.4.3-2-g060aec7, 2026-04-16 "emscripten: Emscripten fixup (#177)"
DIR="$(cd "$(dirname "$0")" && pwd)/third_party/ggwave"
if [ ! -d "$DIR/.git" ]; then
  mkdir -p "$(dirname "$DIR")"
  git clone --quiet https://github.com/ggerganov/ggwave "$DIR"
fi
git -C "$DIR" fetch --quiet origin "$COMMIT" 2>/dev/null || true
git -C "$DIR" -c advice.detachedHead=false checkout --quiet "$COMMIT"
echo "ggwave at $(git -C "$DIR" rev-parse HEAD)"
