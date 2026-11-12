#!/usr/bin/env bash
# demo_r7.sh safe|unsafe — R7 `robust-mutex`: a process dies while holding the shared lock.
#
# Needs the debug build (fault injection is compiled only there): BENCH_FAULT=die_in_lock:3
# makes the 3rd connection's child call abort() while it holds the lock.
#
#   unsafe: plain mutex -> every process blocks on the lock forever: viewers freeze, the reader
#           cannot append, new clients hang.
#   safe:   PTHREAD_MUTEX_ROBUST -> next locker gets EOWNERDEAD, checks/repairs the ring,
#           pthread_mutex_consistent(), logs "EOWNERDEAD recovered", everything keeps running.
ROOT_GUESS=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-$ROOT_GUESS/build/debug/bin}
[ -x "$BIN/benchd" ] || make -C "$ROOT_GUESS" debug >/dev/null
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" robust-mutex

start_sim --loop
BENCH_FAULT=die_in_lock:3 start_server $UNSAFE_OPT

c_head "two viewers watching (connections #1 and #2)"
for v in 1 2; do
    "$BIN/bench" watch "127.0.0.1:$PORT" --name "viewer$v" --duration 12 --out "$WORK/viewer$v.bin" \
        >/dev/null 2>"$WORK/viewer$v.err" &
    BG_PIDS+=($!)
done
sleep 3
b1=$(stat -c %s "$WORK/viewer1.bin")

c_head "connection #3 aborts inside the critical section"
timeout 3 "$BIN/bench" watch "127.0.0.1:$PORT" --name victim --duration 1 >/dev/null 2>&1
sleep 1
b2=$(stat -c %s "$WORK/viewer1.bin")
sleep 4
b3=$(stat -c %s "$WORK/viewer1.bin")
echo "viewer1 bytes received: before=$b1  right after=$b2  4 s later=$b3"
if [ "$b3" -gt "$b2" ]; then c_ok "viewers keep receiving data"; else c_bad "viewers are FROZEN (no new data)"; fi

c_head "can a new client still get system info? (3 s timeout)"
if out=$(timeout 3 "$BIN/bench" info --raw "127.0.0.1:$PORT" 2>&1); then
    c_ok "yes: $(echo "$out" | grep -E '^server\.(clients|lock_recoveries)=' | tr '\n' ' ')"
else
    c_bad "no — the server hangs"
fi
show_server_log 8
