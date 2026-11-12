#!/usr/bin/env bash
# demo_r12.sh safe|unsafe — R12 `reconnect` (client): benchd restarts under a connected client.
#   unsafe: the client simply exits when the connection drops.
#   safe:   back-off reconnect (1,2,4,8,16,30 s), "server restarted" separator, replay.
. "$(dirname "$0")/lib.sh"
need_bins
command -v screen >/dev/null || { echo "needs GNU screen"; exit 1; }
parse_mode "$1" reconnect
start_sim --loop
start_server
SESSION=r12-$$
LC_ALL=C.UTF-8 screen -dmS "$SESSION" bash -c "LANG=C.UTF-8 '$BIN/bench' $UNSAFE_OPT --name survivor 127.0.0.1:$PORT; echo \"client exited: \$?\"; sleep 30"
sleep 1.5
c_head "restart benchd"
kill "$D_PID"; wait "$D_PID" 2>/dev/null
sleep 1
start_server
sleep 3
screen -S "$SESSION" -X hardcopy "$WORK/screen.txt"
grep -a -E 'server shutdown|server restarted|replay end|client exited|RECONNECT|VIEW' "$WORK/screen.txt" | cut -c1-80
if pgrep -f "^$BIN/bench .*--name survivor" >/dev/null; then c_ok "client still running and reconnected"
else c_bad "client is gone"; fi
screen -S "$SESSION" -X quit 2>/dev/null
