#!/usr/bin/env bash
# demo_r5.sh safe|unsafe — R5 `timeout`: 32 clients connect and never say anything.
#   unsafe: each one holds a child and a slot forever -> the server (max 32) refuses everybody.
#   safe:   HELLO must arrive within 5 s (then 60 s idle limit + TCP keepalive) -> slots free up.
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" timeout
start_sim --loop
start_server --max-clients 32 $UNSAFE_OPT
c_head "32 silent connections"
"$BIN/bench-evil" silent "127.0.0.1:$PORT" --count 32 --sec 15 &
BG_PIDS+=($!)
sleep 1
c_head "t=1s: a normal user tries to connect"
info | head -3
sleep 6
c_head "t=7s: the same user tries again"
if out=$(info); then c_ok "connected: $(echo "$out" | grep -E '^server\.clients=')"
else c_bad "refused: $out"; fi
c_head "server children now: $(children_of | wc -l) (including the reader)"
show_server_log 4
