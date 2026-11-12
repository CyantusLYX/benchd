#!/usr/bin/env bash
# demo_r4.sh safe|unsafe — R4 `readn`: a client sends its packets one byte every 50 ms.
#   unsafe: one recv() is assumed to return a whole header -> a 1-byte "header" is parsed,
#           magic is wrong, the client is kicked out for a protocol error it never made.
#   safe:   readn() loops until the full header / payload arrived.
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" readn
start_sim --loop
start_server $UNSAFE_OPT
c_head "bench-evil slow"
"$BIN/bench-evil" slow "127.0.0.1:$PORT" | head -8
show_server_log 4
