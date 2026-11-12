#!/usr/bin/env bash
# demo_r2.sh safe|unsafe — R2 `sigpipe`: the server is full and clients reset their connections.
#   When full, the master peeks at the client's HELLO (for the log) and answers SERVER_BUSY.
#   A client that already sent RST makes that write fail:
#   unsafe: default SIGPIPE -> the master is killed -> the whole server is gone.
#   safe:   SIGPIPE ignored, EPIPE/ECONNRESET logged, server keeps going.
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" sigpipe
start_sim --loop
start_server --max-clients 3 $UNSAFE_OPT

c_head "fill the server: 3 long-lived clients"
"$BIN/bench-evil" flood "127.0.0.1:$PORT" --count 3 --sec 20 &
BG_PIDS+=($!)
sleep 1
c_head "40 more clients connect and immediately reset (SO_LINGER 0 -> RST)"
"$BIN/bench-evil" rst "127.0.0.1:$PORT" --count 40
sleep 1
c_head "is benchd still running?"
if server_alive; then c_ok "yes — master pid $D_PID still listening on :$PORT"
else
    wait "$D_PID" 2>/dev/null; st=$?
    c_bad "NO — master exited with status $st ($( [ $st -gt 128 ] && kill -l $((st-128)) ))"
fi
show_server_log 6
