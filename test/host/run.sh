#!/bin/sh
# test/host/run.sh -- compile firmware/fauxcloud/src/*.cpp + the host test with g++ and run it. No network.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
src="$root/firmware/fauxcloud/src"
mkdir -p "$here/build/c"
echo "*" > "$here/build/.gitignore"  # self-ignoring build dir
# The C side -- vendored BearSSL (device + host-only lists, warnings not promoted) and our generated anchors (ours:
# -Werror). Every OS entropy / time source and the 128-bit paths off, as on the xtensa target.
brdefs="-DBR_64=0 -DBR_LE_UNALIGNED=0 -DBR_INT128=0 -DBR_RDRAND=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0"
cobjs=""
for c in "$src"/bearssl/*.c "$here"/bearssl/*.c; do
  o="$here/build/c/$(basename "$c" .c).o"
  gcc -std=c99 -O1 -c $brdefs -I "$src" -I "$src/bearssl" "$c" -o "$o"
  cobjs="$cobjs $o"
done
for c in "$src"/*.c; do
  o="$here/build/c/$(basename "$c" .c).o"
  gcc -std=c99 -O1 -Wall -Wextra -Werror -c $brdefs -I "$src" -I "$src/bearssl" "$c" -o "$o"
  cobjs="$cobjs $o"
done
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -ffp-contract=off -I "$src" "$src"/*.cpp "$here/test_fauxcloud.cpp" $cobjs -o "$here/build/test_fauxcloud"
"$here/build/test_fauxcloud"
# The variants, the same suite each (FC_MAX_STATIONS and FWD_TLS are independent profile constants; three runs by
# convention): the default run above is the release (FWD_TLS=1, one station); FWD_TLS=0 the HTTP-only fallback;
# the PSRAM placeholder, two stations + TLS + the bench diagnostics (the host compiles bench code under the flag)
for v in "http:-DFWD_TLS=0" "st2:-DFWD_TLS=1 -DFC_MAX_STATIONS=2 -DFC_DIAG_BENCH=1"; do
  name=${v%%:*}
  defs=${v#*:}
  g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -ffp-contract=off $defs -I "$src" "$src"/*.cpp "$here/test_fauxcloud.cpp" $cobjs -o "$here/build/test_fauxcloud_$name"
  "$here/build/test_fauxcloud_$name" > "$here/build/test_fauxcloud_$name.out" || { cat "$here/build/test_fauxcloud_$name.out"; exit 1; }
  echo "$defs: $(tail -1 "$here/build/test_fauxcloud_$name.out")"
done
# The outlook replay against a saved board block -- MATCH exits 0, one code changed exits 1
g++ -std=c++17 -O1 -Wall -Wextra -Werror -ffp-contract=off -I "$src" "$src"/*.cpp "$here/forecast_replay.cpp" $cobjs -o "$here/build/forecast_replay"
fx="$here/fixtures"
"$here/build/forecast_replay" "$fx/replay-hourly.json" "$fx/replay-live.json" "$fx/replay-bridge.json" >/dev/null
sed 's/"p_falling"/"p_steady"/' "$fx/replay-live.json" > "$here/build/replay-live-diff.json"
rc=0
"$here/build/forecast_replay" "$fx/replay-hourly.json" "$here/build/replay-live-diff.json" "$fx/replay-bridge.json" >/dev/null || rc=$?
if [ "$rc" -ne 1 ]; then  # DIFF is exit 1; 2 is a parse error
  echo "FAIL forecast_replay: a changed board code gave exit $rc, not 1 (DIFF)"; exit 1
fi
# T3 through --metar -- MATCH with the airport's drizzle + mist, DIFF without the file, nothing from 61 km
"$here/build/forecast_replay" "$fx/replay-hourly.json" "$fx/replay-t3-live.json" "$fx/replay-bridge.json" --metar "$fx/replay-t3-metar.json" >/dev/null \
  || { echo "FAIL forecast_replay: the T3 fixture did not MATCH with --metar"; exit 1; }
rc=0
"$here/build/forecast_replay" "$fx/replay-hourly.json" "$fx/replay-t3-live.json" "$fx/replay-bridge.json" >/dev/null || rc=$?
[ "$rc" -eq 1 ] || { echo "FAIL forecast_replay: the T3 fixture without --metar gave exit $rc, not 1"; exit 1; }
"$here/build/forecast_replay" "$fx/replay-hourly.json" "$fx/replay-live.json" "$fx/replay-bridge.json" --metar "$fx/replay-t3-metar-far.json" >/dev/null \
  || { echo "FAIL forecast_replay: an airport 61 km away changed the outlook"; exit 1; }
echo "OK: forecast_replay MATCH / DIFF on the fixture"
# --what-if (fixed rules vs the learners in use): a calm evening at 0.5 °C/h with L5 learned at 0.35 changes 6 of
# 8 hours to night_clear; the same file with every learner "default" changes none; the saved board block runs
wi="$here/build/forecast_replay --what-if"
out=$($wi "$fx/replay-whatif-hourly.json" "$fx/replay-bridge.json" "$fx/replay-learn.json") || { echo "FAIL what-if exit"; exit 1; }
echo "$out" | grep -q "summary: 6 of 8 hours changed" && echo "$out" | grep -q "learned night_clear" || { echo "FAIL what-if: $out"; exit 1; }
sed 's/"in_use"/"default"/g' "$fx/replay-learn.json" > "$here/build/replay-learn-default.json"
$wi "$fx/replay-whatif-hourly.json" "$fx/replay-bridge.json" "$here/build/replay-learn-default.json" | grep -q "summary: 0 of 8 hours changed" || { echo "FAIL what-if default"; exit 1; }
$wi "$fx/replay-hourly.json" "$fx/replay-bridge.json" "$fx/replay-learn.json" >/dev/null || { echo "FAIL what-if on the board block"; exit 1; }
echo "OK: forecast_replay --what-if"
