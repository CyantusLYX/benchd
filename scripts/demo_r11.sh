#!/usr/bin/env bash
# demo_r11.sh safe|unsafe — R11 `term-restore` (client): the interactive client crashes.
#   unsafe: the terminal stays in raw mode (no echo, no line editing) after the crash.
#   safe:   SIGSEGV handler restores termios + scroll region, then dies with the same signal.
# Runs the client inside a detached GNU screen and checks `stty -a` afterwards.
. "$(dirname "$0")/lib.sh"
need_bins
command -v screen >/dev/null || { echo "needs GNU screen"; exit 1; }
parse_mode "$1" term-restore
start_sim --loop
start_server
SESSION=r11-$$
LC_ALL=C.UTF-8 screen -dmS "$SESSION" bash -c "'$BIN/bench' $UNSAFE_OPT --name crashy 127.0.0.1:$PORT; echo \"exit=\$?\" >'$WORK/exit.txt'; stty -a >'$WORK/stty.txt'; sleep 2"
sleep 1.5
P=$(pgrep -f "^$BIN/bench .*--name crashy" | head -1)
c_head "kill -SEGV the client (pid $P)"
kill -SEGV "$P"
sleep 1
cat "$WORK/exit.txt"
c_head "terminal flags after the crash"
grep -oE -- '-?(icanon|echo|isig|icrnl)( |$)' "$WORK/stty.txt" | tr '\n' ' '; echo
if grep -q -- '-icanon' "$WORK/stty.txt"; then c_bad "terminal left in RAW mode (no echo / no line editing)"
else c_ok "terminal restored (icanon echo)"; fi
screen -S "$SESSION" -X quit 2>/dev/null
