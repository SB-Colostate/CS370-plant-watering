#!/bin/sh
# Integration: plantctl against a log written through the store API.
# BIN (binary dir) and RUNNER (e.g. valgrind) come from make.
set -eu
BIN=${BIN:?BIN not set}
RUNNER=${RUNNER:-}

d=$(mktemp -d /tmp/plant-it-XXXXXX)
trap 'rm -rf "$d"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

"$BIN/mklog" "$d" 5    # START + 5 soil + 5 level = 11 records

out=$($RUNNER "$BIN/plantctl" -d "$d" status)
echo "$out"
echo "$out" | grep -q '^records:    11 (last seq 10)$' || fail "status record count"
echo "$out" | grep -q 'none (no hub holds LOCK)'       || fail "status lock state"
echo "$out" | grep -q '^soil:       404 raw'            || fail "status last soil"
echo "$out" | grep -q '^level:      NO_ECHO'            || fail "status no-echo level"
echo "$out" | grep -q '^last RECOVERED: none$'          || fail "status recovered"

out=$($RUNNER "$BIN/plantctl" -d "$d" tail 3)
echo "$out"
[ "$(echo "$out" | wc -l)" -eq 4 ]                           || fail "tail 3 should print header + 3 rows"
echo "$out" | sed -n 2p | grep -q ' 8  .* level   123.3 cm$'|| fail "tail row seq 8"
echo "$out" | sed -n 3p | grep -q ' 9  .* soil    404 raw$'  || fail "tail row seq 9"

# A torn tail is reported, and plantctl must not touch the file.
printf 'torn!' >> "$d/plant.plog"
size=$(stat -c %s "$d/plant.plog")
out=$($RUNNER "$BIN/plantctl" -d "$d" status)
echo "$out" | grep -q '^torn tail:  5 bytes' || fail "torn tail not reported"
echo "$out" | grep -q '^records:    11 '     || fail "torn tail changed the valid count"
[ "$(stat -c %s "$d/plant.plog")" -eq "$size" ] || fail "plantctl modified the log"

# Error paths: missing log -> 2, bad command -> 1.
set +e
$RUNNER "$BIN/plantctl" -d "$d/nope" status 2>/dev/null; rc=$?
[ $rc -eq 2 ] || fail "missing log exit $rc, want 2"
$RUNNER "$BIN/plantctl" -d "$d" bogus 2>/dev/null; rc=$?
[ $rc -eq 1 ] || fail "bad command exit $rc, want 1"
set -e

echo "it_plantctl: PASS"
