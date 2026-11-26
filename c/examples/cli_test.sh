#!/bin/sh
# Round trip through the CLI: encode -> decode for every band (and robust, and other rates), plus a real recording.
# usage: cli_test.sh path/to/bysound path/to/payload-top-v4.wav workdir
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
check "recording payload-top-v4.wav" "$REC" "text=000201010212264700"
# acks and receipts: the id is the first 4 bytes of SHA-256 of the text ("abc" -> ba7816bf), the heard nibble says where it was read
"$BYSOUND" ack "abc" -o "$DIR/ack.wav" >/dev/null; check "ack" "$DIR/ack.wav" "band=mid profile=normal corrected=0 ack id=ba7816bf"
"$BYSOUND" ack "abc" -o "$DIR/ack-h.wav" --heard upper/robust >/dev/null; check "ack heard" "$DIR/ack-h.wav" "ack id=ba7816bf heard=upper/robust"
"$BYSOUND" ack "abc" -o "$DIR/receipt.wav" --receipt --heard top >/dev/null; check "receipt" "$DIR/receipt.wav" "band=high profile=normal corrected=0 receipt id=ba7816bf heard=top/normal"
"$BYSOUND" ack "abc" -o "$DIR/ack44.wav" --rate 44100 --band top >/dev/null; check "ack 44100 Hz on top" "$DIR/ack44.wav" "band=top profile=normal corrected=0 ack id=ba7816bf"
"$BYSOUND" bands >/dev/null && echo "ok   bands"
"$BYSOUND" bands | grep -q "max text" && echo "ok   bands max" || { echo "FAIL bands max"; fail=1; }
long=$(awk 'BEGIN{for(i=0;i<5000;i++)printf "a"}')
if "$BYSOUND" encode "$long" -o "$DIR/long.wav" --band top 2>"$DIR/long.err"; then echo "FAIL too long accepted"; fail=1; else grep -q "text too long" "$DIR/long.err" && echo "ok   too long rejected" || { echo "FAIL too long message"; fail=1; }; fi
exit $fail
