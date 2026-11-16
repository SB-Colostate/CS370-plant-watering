#!/bin/sh
# Integration: the watering-engine event records (kinds 5-10) survive the store and plantctl
# names them. Fixture values, not measurements.
set -eu
BIN=${BIN:?BIN not set}
RUNNER=${RUNNER:-}

d=$(mktemp -d /tmp/plant-it-XXXXXX)
trap 'rm -rf "$d"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

"$BIN/mkevents" "$d"    # START + 6 engine events
out=$($RUNNER "$BIN/plantctl" -d "$d" tail 6)
echo "$out"
rows=$(echo "$out" | tail -n +2 | awk '{ $2 = $3 = $4 = ""; print }' | tr -s ' ' | sed 's/^ *//')
want="1 system WATER_INTENT dose=20000ms
2 system DIAG OK
3 system LOCKOUT UNFINISHED_WATERING
4 system ACK cleared=PUMP_NO_FLOW
5 system PHASE WATERING
6 system BLOCKS 0x41"
[ "$rows" = "$want" ] || { echo "got:"; echo "$rows"; fail "event rows differ"; }

# status still works on a log full of events (no soil/level samples in it)
out=$($RUNNER "$BIN/plantctl" -d "$d" status)
echo "$out" | grep -q '^records:    7 (last seq 6)$' || fail "status record count"
echo "$out" | grep -q '^soil:       no samples$'    || fail "status soil"

echo "it_plantctl_events: PASS"
