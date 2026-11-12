#!/usr/bin/env bash
# test.sh — `make test` (§10.3): automated checks against a live benchd + benchsim.
#
#   build facts     no threads, shared .so, TRACE strings absent from the release build
#   protocol        info round trip, send/receive through the UART, replay amount
#   control         take/release mutual exclusion, idle auto-release, crash release
#   device          unplug -> DEV LOST -> replug -> UP
#   robustness      every bench-evil mode in safe mode leaves the server alive
#   hygiene         no zombie children afterwards
PORT=${PORT:-19400}
DEV=${DEV:-/tmp/benchsim-test-$$}
. "$(dirname "$0")/lib.sh"
need_bins
PASS=0
FAIL=0
check() {   # check "description" command...
    local desc=$1; shift
    if "$@" >>"$WORK/test.log" 2>&1; then
        PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %s\n' "$desc"
    else
        FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$desc"
    fi
}
: >"$WORK/test.log"
A=127.0.0.1:$PORT

c_head "build"
check "no pthread_create() call in the sources" bash -c "! grep -rnE 'pthread_create[[:space:]]*\(' '$ROOT'/server '$ROOT'/client '$ROOT'/lib '$ROOT'/tools --include='*.c' --include='*.h'"
check "no pthread_create symbol in binaries"    bash -c "! nm -D '$BIN'/* '$ROOT'/build/lib/libbench.so.1 2>/dev/null | grep -q pthread_create"
check "benchd and bench link libbench.so.1"     bash -c "ldd '$BIN/benchd' | grep -q 'libbench.so.1 => .*build' && ldd '$BIN/bench' | grep -q 'libbench.so.1 => .*build'"
check "release build has no TRACE strings"      bash -c "! strings '$BIN/benchd' '$ROOT/build/lib/libbench.so.1' | grep -q 'tx pkt type='"

start_sim
start_server --idle-release 3
sleep 2.5      # let some boot output into the history

c_head "protocol"
check "INFO round trip has all sections"        bash -c "'$BIN/bench' info --raw $A | grep -q '^host.kernel=' && '$BIN/bench' info --raw $A | grep -q '^ctrl.holder='"
check "tail -n 0 replays nothing"               bash -c "[ \$('$BIN/bench' tail $A -n 0 | wc -c) -eq 0 ]"
check "tail -n 1K replays at most 1K"           bash -c "n=\$('$BIN/bench' tail $A -n 1K | wc -c); [ \$n -gt 0 ] && [ \$n -le 1024 ]"
check "tail -n 64K replays the whole history"   bash -c "'$BIN/bench' tail $A -n 64K | grep -aq 'U-Boot SPL'"
check "send 'version' gets the U-Boot answer"   bash -c "'$BIN/bench' send $A 'version\r' --wait 1500 | grep -aq 'U-Boot 2014.07 (benchsim'"
check "unknown host port fails cleanly"         bash -c "! '$BIN/bench' info 127.0.0.1:1 2>/dev/null"

c_head "control"
"$BIN/bench-evil" take-hold "$A" --name alice --sec 8 >"$WORK/hold.txt" 2>&1 &
HOLD=$!
sleep 0.7
check "second client is refused (exit 2)"       bash -c "'$BIN/bench' send $A 'x' --name bob; [ \$? -eq 2 ]"
check "refusal names the holder"                bash -c "'$BIN/bench' send $A 'x' --name bob 2>&1 | grep -q 'held by alice'"
sleep 3.5
check "idle holder auto-released after 3 s"     bash -c "'$BIN/bench' info --raw $A | grep -q '^ctrl.holder=(none)'"
check "now bob gets control"                    "$BIN/bench" send "$A" '\r' --name bob
kill "$HOLD" 2>/dev/null; wait "$HOLD" 2>/dev/null
"$BIN/bench-evil" take-and-die "$A" --name crashy >/dev/null 2>&1
sleep 0.3
check "crashed controller released"             "$BIN/bench" send "$A" '\r' --name bob
check "history records take/release events"     bash -c "'$BIN/bench' tail $A -n 64K | grep -aq 'control released by crashy (disconnect)'"

c_head "device"
stop_sim
sleep 0.8
check "unplugged device reported LOST"          bash -c "'$BIN/bench' info --raw $A | grep -q '^dev.state=LOST'"
check "input while LOST gets ERROR DEVICE_LOST" bash -c "'$BIN/bench' send $A 'x' --wait 300 2>&1 | grep -q DEVICE_LOST"
start_sim
sleep 1.2
check "replugged device back UP"                bash -c "'$BIN/bench' info --raw $A | grep -q '^dev.state=UP' && '$BIN/bench' info --raw $A | grep -q '^dev.reconnects=1'"

c_head "robustness (safe mode): server survives every bench-evil mode"
for m in slow bad-len bad-magic bad-crc take-and-die; do
    "$BIN/bench-evil" "$m" "$A" >"$WORK/evil-$m.txt" 2>&1
    check "survives $m" server_alive
done
check "slow client still gets INFO"             grep -q 'got INFO' "$WORK/evil-slow.txt"
check "bad-len answered with BAD_LENGTH"        grep -q 'BAD_LENGTH' "$WORK/evil-bad-len.txt"
"$BIN/bench-evil" silent "$A" --count 5 --sec 7 >"$WORK/evil-silent.txt" 2>&1
check "silent clients dropped after ~5 s"       grep -q '0 of 5 silent connections still open' "$WORK/evil-silent.txt"
"$BIN/bench-evil" rst "$A" --count 20 >/dev/null 2>&1
check "survives rst x20"                        server_alive
"$BIN/bench-evil" flood "$A" --count 40 --sec 2 >"$WORK/evil-flood.txt" 2>&1
check "flood capped at max-clients (32)"        grep -q '32 welcomed, 8 refused' "$WORK/evil-flood.txt"
check "survives flood"                          server_alive
"$BIN/bench-evil" lag "$A" --sec 3 --after 1 >/dev/null 2>&1
check "survives lag"                            server_alive

c_head "hygiene"
sleep 1
check "no zombie children"                      bash -c "! ps -o stat= --ppid $D_PID | grep -q Z"
check "only the reader is left as a child"      bash -c "[ \$(pgrep -P $D_PID | wc -l) -eq 1 ]"

echo
if [ "$FAIL" -eq 0 ]; then c_ok "all $PASS checks passed"; else c_bad "$FAIL of $((PASS + FAIL)) checks FAILED (details: $WORK/test.log)"; exit 1; fi
