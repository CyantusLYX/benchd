#!/usr/bin/env bash
# demo_r3.sh safe|unsafe — R3 `validate`: one malicious packet vs. header validation.
#
#   unsafe: the child trusts the header's len (1 MiB) and reads it into a 4 KiB stack buffer
#           -> stack overflow, the child crashes ("stack smashing detected" / SIGSEGV).
#   safe:   magic / version / len <= limit / CRC are checked; ERROR BAD_LENGTH, clean disconnect.
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" validate

start_sim --loop
start_server $UNSAFE_OPT

c_head "a normal viewer is watching in the background"
"$BIN/bench" watch "127.0.0.1:$PORT" --name viewer --duration 8 --out "$WORK/viewer.bin" >/dev/null 2>"$WORK/viewer.err" &
BG_PIDS+=($!)
sleep 1

for m in bad-magic bad-crc bad-len; do
    c_head "bench-evil $m"
    "$BIN/bench-evil" "$m" "127.0.0.1:$PORT"
    sleep 0.5
done

c_head "is the server still serving?"
if server_alive && info_key server.clients >/dev/null; then
    c_ok "benchd alive: $(info | grep -E '^server\.(clients|unsafe)=' | tr '\n' ' ')"
else
    c_bad "benchd is not answering"
fi
show_server_log 12
c_head "children that died abnormally"
grep -E 'killed by signal|stack smashing' "$LOG_D" || c_ok "(none)"
