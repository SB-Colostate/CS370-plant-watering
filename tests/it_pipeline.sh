#!/bin/sh
# Integration: fixture lines -> FIFO -> sampler -> Unix datagram -> hub -> log -> plantctl.
# Fixture values are made up for the test; they are not sensor readings.
set -eu
BIN=${BIN:?BIN not set}
RUNNER=${RUNNER:-}

d=$(mktemp -d /tmp/plant-it-XXXXXX)
hub_pid=
trap '[ -n "$hub_pid" ] && kill "$hub_pid" 2>/dev/null; rm -rf "$d"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

$RUNNER "$BIN/hub" -d "$d" 2>"$d/hub.err" &
hub_pid=$!
i=0
until [ -S "$d/hub.sock" ]; do
    i=$((i + 1)); [ $i -le 200 ] || { cat "$d/hub.err"; fail "hub socket never appeared"; }
    sleep 0.1
done

mkfifo "$d/uno"
exec 3<>"$d/uno"        # hold a writer open so the sampler doesn't see EOF yet
$RUNNER "$BIN/sampler" -D "$d/uno" -d "$d" 2>"$d/sampler.err" &
sampler_pid=$!
sleep 1

printf 'IST,7.0\r\n'                 >&3   # cut first line after a reset: discarded
printf 'SOIL,412,DIST,23.4\r\n'      >&3
printf 'SOIL,1023,DIST,-1.0\r\n'     >&3
printf 'BAD,LINE\r\n'                >&3   # counted as a bad frame, not stored
printf 'SOIL,0,DIST,5.0\n'           >&3
sleep 2
exec 3>&-                                  # writer gone: sampler sees EOF and must exit 3

set +e
wait "$sampler_pid"; src=$?
kill -TERM "$hub_pid"; wait "$hub_pid"; hrc=$?
set -e
hub_pid=
cat "$d/sampler.err" "$d/hub.err"
[ $src -eq 3 ] || fail "sampler exit $src on EOF, want 3"
[ $hrc -eq 0 ] || fail "hub exit $hrc on SIGTERM, want 0"
[ ! -e "$d/hub.sock" ] || fail "hub left its socket behind"
grep -q 'bad_frames=1' "$d/sampler.err" || fail "bad frame not counted"

out=$($RUNNER "$BIN/plantctl" -d "$d" tail 100)
echo "$out"
# START, SAMPLER_RESTART, then 3 lines x 2 readings = 8 records.
[ "$(echo "$out" | wc -l)" -eq 9 ] || fail "want header + 8 records"
rows=$(echo "$out" | tail -n +2 | awk '{ $2 = $3 = $4 = ""; print }' | tr -s ' ')
want=" 0 system START
 1 system SAMPLER_RESTART pid=$sampler_pid
 2 soil 412 raw
 3 level 23.4 cm
 4 soil 1023 raw
 5 level NO_ECHO
 6 soil 0 raw
 7 level 5.0 cm"
[ "$(echo "$rows" | sed 's/^ *//; s/^/ /')" = "$want" ] || { echo "got:"; echo "$rows"; fail "records differ"; }

echo "it_pipeline: PASS"
