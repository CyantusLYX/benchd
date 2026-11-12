#!/usr/bin/env bash
# demo_r1.sh safe|unsafe — R1 `zombie`: 20 clients connect and leave.
#   unsafe: SIGCHLD is not handled -> every finished child stays <defunct>.
#   safe:   SIGCHLD sets a flag, the main loop reaps with waitpid(-1, WNOHANG).
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" zombie
start_sim --loop
start_server $UNSAFE_OPT

c_head "20 clients connect and disconnect"
for i in $(seq 20); do "$BIN/bench" info --raw "127.0.0.1:$PORT" >/dev/null; done
sleep 0.5
c_head "children of benchd ($D_PID)"
ps -o pid,stat,comm --ppid "$D_PID"
z=$(ps -o stat= --ppid "$D_PID" | grep -c Z)
if [ "$z" -gt 0 ]; then c_bad "$z zombie (<defunct>) children"; else c_ok "no zombies"; fi
