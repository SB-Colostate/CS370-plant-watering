#!/bin/sh
# End to end through the REAL processes: fake Uno -> FIFO -> sampler -> Unix socket -> hub (store,
# watering engine, pump control with the SIMULATED relay file) -> plant.plog -> plantctl.
# Short engine timings (hub -t). Everything here is simulated; no hardware is involved.
#  1. a full watering cycle ends with DIAG OK, and the intent was saved before the relay went on;
#  2. a clean hub restart replays the history and does NOT water a second time;
#  3. kill -9 mid-dose: the relay file is still ON after the crash; the restarted hub switches it
#     OFF before anything else, and locks out with UNFINISHED_WATERING.
set -eu
BIN=${BIN:?BIN not set}
RUNNER=${RUNNER:-}
root=$(mktemp -d /tmp/plant-it-XXXXXX)
pids=""
cleanup() { for p in $pids; do kill -9 "$p" 2>/dev/null || true; done; rm -rf "$root"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; for f in "$d"/*.err; do echo "--- $f"; tail -n 20 "$f"; done; exit 1; }

log()     { "$BIN/plantctl" -d "$d" tail 1000000 2>/dev/null; }
count()   { log | grep -c "$1" || true; }
relay()   { cat "$d/pump.sim" 2>/dev/null || echo "?"; }
# wait_until <seconds> <shell condition>
wait_until() {
    n=$(( $1 * 10 )); shift
    while ! eval "$1"; do n=$((n - 1)); [ $n -gt 0 ] || return 1; sleep 0.1; done
}

start_hub() {
    before=$(grep -c 'water: replayed' "$d/hub.err" 2>/dev/null || true)
    $RUNNER "$BIN/hub" -d "$d" -t 2>>"$d/hub.err" &
    hub_pid=$!; pids="$pids $hub_pid"
    wait_until 20 '[ "$(grep -c "water: replayed" "$d/hub.err")" -gt "$before" ]' || fail "hub did not start"
}

# setup <name>: a fresh data dir with a hub, the real sampler and a fake Uno.
setup() {
    d="$root/$1"; mkdir -p "$d"; : > "$d/hub.err"
    start_hub
    mkfifo "$d/uno"
    exec 3<>"$d/uno"                     # hold the FIFO open so the sampler never sees EOF
    $RUNNER "$BIN/sampler" -D "$d/uno" -d "$d" 2>"$d/sampler.err" 3>&- &
    pids="$pids $!"
    "$BIN/fakeuno" "$d/uno" "$d/pump.sim" 8 2>"$d/fake.err" 3>&- &
    pids="$pids $!"
}

# ---------------------------------------------------------------- 1. full cycle
setup cycle
wait_until 40 '[ "$(count WATER_INTENT)" -ge 1 ]' || fail "no watering requested"
wait_until 10 '[ "$(relay)" = 1 ]'                || fail "relay never switched on"
wait_until 40 '[ "$(count "DIAG ")" -ge 1 ]'       || fail "no diagnosis"
log | grep -q 'DIAG OK'                            || { log | grep 'DIAG '; fail "diagnosis was not OK"; }
[ "$(relay)" = 0 ]                                 || fail "relay still on after the watering"
# The intent reached the log before the relay switched on.
intent_s=$(log | awk '/WATER_INTENT/ { print $4; exit }')
on_ns=$(awk '$3 == "ON" { print $1; exit }' "$d/pump.sim.log")
awk -v i="$intent_s" -v o="$on_ns" 'BEGIN { exit !(i * 1e9 <= o) }' || fail "relay on before the intent was saved"
echo "1. full cycle: PASS ($(count WATER_INTENT) intent, DIAG OK, intent at ${intent_s}s before relay ON)"

# ---------------------------------------------------------------- 2. clean restart
kill -TERM "$hub_pid"
set +e; wait "$hub_pid"; hrc=$?; set -e
[ $hrc -eq 0 ] || fail "hub exit $hrc on SIGTERM"
start_hub
wait_until 30 '[ "$(count "PHASE DRY_DWELL")" -ge 2 ]' \
    || fail "after the restart the soil never read dry again (test would prove nothing)"
sleep 10                                           # long past warm-up + dwell + confirm
[ "$(count WATER_INTENT)" -eq 1 ] || fail "watered again after a restart: the history was not restored"
[ "$(count LOCKOUT)" -eq 0 ]      || fail "unexpected lockout after a clean restart"
blocks=$(log | awk '/BLOCKS/ { b = $7 } END { print b }')
[ $(( blocks & 0x40 )) -ne 0 ]    || fail "last BLOCKS $blocks lacks RATE_LIMIT (0x40)"
echo "2. clean restart: PASS (dry again, but blocked by RATE_LIMIT; still $(count WATER_INTENT) intent)"

# ---------------------------------------------------------------- 3. crash mid-dose
exec 3>&-
setup crash
wait_until 40 '[ "$(relay)" = 1 ]' || fail "relay never switched on"
kill -9 "$hub_pid"
set +e; { wait "$hub_pid"; } 2>/dev/null; set -e
sleep 0.5
[ "$(relay)" = 1 ] || fail "the simulated relay went off by itself after the crash (it must not)"
lines_before=$(wc -l < "$d/pump.sim.log")
start_hub
first_new=$(tail -n +"$((lines_before + 1))" "$d/pump.sim.log" | head -n 1)
[ "$(echo "$first_new" | awk '{ print $2 " " $3 }')" = "$hub_pid OFF" ] \
    || fail "first relay command after the restart was '$first_new', want '$hub_pid OFF'"
[ "$(relay)" = 0 ] || fail "relay not OFF after the restart"
# The OFF came before the store was even opened.
awk '/relay OFF at startup/ { off = NR } /hub: log/ { if (NR > off && off) ok = 1 } END { exit !ok }' "$d/hub.err" \
    || fail "relay OFF was not the first thing the restarted hub did"
wait_until 10 '[ "$(count "LOCKOUT UNFINISHED_WATERING")" -eq 1 ]' || fail "no UNFINISHED_WATERING lockout"
sleep 8
[ "$(count WATER_INTENT)" -eq 1 ] || fail "watered again after the crash"
[ "$(relay)" = 0 ]                || fail "relay came back on while locked out"
echo "3. crash mid-dose: PASS (relay ON after kill -9, OFF first on restart by pid $hub_pid, LOCKOUT UNFINISHED_WATERING)"

"$BIN/plantctl" -d "$d" status | sed -n '1,4p'
echo "it_engine: PASS"
