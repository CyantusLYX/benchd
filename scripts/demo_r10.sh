#!/usr/bin/env bash
# demo_r10.sh safe|unsafe — R10 `overrun`: a viewer stops reading and falls further behind
# than the 4 KiB history ring holds.
#   unsafe: its child keeps reading at its stale offset -> bytes that were already overwritten
#           are sent: scrambled / out-of-order text.
#   safe:   my_offset < tail is detected -> EVENT DROPPED (n bytes) and a jump to tail.
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" overrun
# a log without pauses, numbered lines, played fast (921600 baud ~ 92 KB/s): gaps are easy to see
LOGF=$WORK/numbered.log
for i in $(seq -w 1 3000); do echo "line $i: the quick brown fox jumps over the lazy dog"; done >"$LOGF"
start_sim -l "$LOGF" -b 921600 --loop
start_server --history 4K $UNSAFE_OPT
c_head "bench-evil lag: join, stop reading for 15 s, then read for 2 s"
"$BIN/bench-evil" lag "127.0.0.1:$PORT" --sec 15 --after 2 >"$WORK/lag.txt"
c_head "what the lagging client received after it resumed (first 25 lines)"
sed -n '/^-----/,$p' "$WORK/lag.txt" | head -25 | cut -c1-100
c_head "integrity check of everything received after resuming"
# A line cut off right before / right after a DROPPED event is an honest gap, not corruption.
tr -d '\r' <"$WORK/lag.txt" | sed -n '/^-----/,$p' | LC_ALL=C awk '
    function flush_pending() { if (pending != "") { bad++; if (bad <= 4) print "  corrupted: " substr(pending, 1, 72) } pending = "" }
    /^-----/                 { frag = 1; next }
    /EVENT DROPPED/          { drops++; pending = ""; frag = 1; next }
    /^$/                     { next }
    /^line [0-9]+: the quick brown fox jumps over the lazy dog$/ {
        flush_pending()
        n = substr($2, 1, length($2) - 1) + 0
        if (prev && n != prev + 1 && n != 1 && !gap) jumps++   # n == 1: benchsim looped
        prev = n; gap = 0; frag = 0; next }
    { if (frag) { frag = 0; gap = 1; next }   # tail of a line cut by a DROPPED event
      flush_pending(); pending = $0; gap = 1 }
    END { flush_pending()
          printf "%d DROPPED events, %d silent jumps in line numbers, %d corrupted lines\n", drops, jumps, bad
          exit (bad + jumps > 0) }'
if [ $? -eq 0 ]; then c_ok "every gap was announced by a DROPPED event; no corrupted data"
else c_bad "data was silently lost or corrupted"; fi
