#!/bin/sh
# Round trip through the CLI: encode -> decode for every band (and robust, and other rates), plus a real recording.
# usage: cli_test.sh path/to/bysound path/to/falso-v4.wav workdir
set -e
BYSOUND=$1; REC=$2; DIR=$3/cli-test
mkdir -p "$DIR"
fail=0
check() { # name, wav, expected substring
  if "$BYSOUND" decode "$2" | grep -qF -- "$3"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi
}
for b in top high upper mid audible xhigh wide; do
  t="hello $b v4 sound modem"
  "$BYSOUND" encode "$t" -o "$DIR/$b.wav" --band "$b" >/dev/null
  check "band $b" "$DIR/$b.wav" "band=$b profile=normal"
  check "band $b text" "$DIR/$b.wav" "text=$t"
done
for b in top mid; do
  "$BYSOUND" encode "robust $b" -o "$DIR/r-$b.wav" --band "$b" --profile robust >/dev/null
  check "robust $b" "$DIR/r-$b.wav" "band=$b profile=robust corrected=0 text=robust $b"
done
"$BYSOUND" encode "44k top" -o "$DIR/44.wav" --band top --rate 44100 >/dev/null; check "44100 Hz" "$DIR/44.wav" "text=44k top"
"$BYSOUND" encode "16k audible" -o "$DIR/16.wav" --band audible --rate 16000 >/dev/null; check "16000 Hz" "$DIR/16.wav" "text=16k audible"
"$BYSOUND" encode "twice" -o "$DIR/rep.wav" --repeat 2 >/dev/null
[ "$("$BYSOUND" decode "$DIR/rep.wav" | grep -c 'text=twice')" = 2 ] && echo "ok   repeat 2" || { echo "FAIL repeat 2"; fail=1; }
check "recording falso-v4.wav" "$REC" "text=000201010212264700"
"$BYSOUND" bands >/dev/null && echo "ok   bands"
"$BYSOUND" bands | grep -q "max text" && echo "ok   bands max" || { echo "FAIL bands max"; fail=1; }
long=$(awk 'BEGIN{for(i=0;i<5000;i++)printf "a"}')
if "$BYSOUND" encode "$long" -o "$DIR/long.wav" --band top 2>"$DIR/long.err"; then echo "FAIL too long accepted"; fail=1; else grep -q "text too long" "$DIR/long.err" && echo "ok   too long rejected" || { echo "FAIL too long message"; fail=1; }; fi
exit $fail
