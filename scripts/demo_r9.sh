#!/usr/bin/env bash
# demo_r9.sh safe|unsafe — R9 `max-clients`: 200 connections as fast as possible.
#   unsafe: one fork() per connection without limit until fork() fails (EAGAIN).
#   safe:   at --max-clients (32) new connections get ERROR SERVER_BUSY.
# To protect the machine the script caps processes with `ulimit -u` (current count + 150).
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" max-clients
# RLIMIT_NPROC counts threads (tasks) of this user, not processes
cap=$(( $(ps -L -u "$(id -u)" --no-headers | wc -l) + 150 ))
ulimit -u "$cap" 2>/dev/null && c_note "ulimit -u $cap (safety net)"
start_sim --loop
start_server $UNSAFE_OPT
c_head "bench-evil flood --count 200"
"$BIN/bench-evil" flood "127.0.0.1:$PORT" --count 200 --sec 6 &
BG_PIDS+=($!)
sleep 4
c_head "benchd children: $(children_of | wc -l)   (max-clients 32 + reader)"
c_head "fork failures logged: $(grep -c 'fork for' "$LOG_D")   busy rejections logged: $(grep -c 'server full' "$LOG_D")"
wait "${BG_PIDS[-1]}" 2>/dev/null
