#!/usr/bin/env bash
# demo_r6.sh safe|unsafe — R6 `ctrl-release`: the controller crashes while holding control.
#
#   unsafe: ctrl_pid keeps the dead pid -> nobody can ever take control (or type) again.
#   safe:   released on disconnect / by the SIGCHLD reaper / by TAKE's kill(pid,0) check,
#           so the next user gets control immediately.
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" ctrl-release

start_sim
start_server $UNSAFE_OPT

c_head "evil client takes control, then dies without saying BYE"
"$BIN/bench-evil" take-and-die "127.0.0.1:$PORT" --name crashy
sleep 0.5

c_head "control state according to the server"
info | grep -E '^ctrl\.'

c_head "alice tries to take control and type 'version'"
"$BIN/bench" send "127.0.0.1:$PORT" --name alice --wait 800 'version\r'
rc=$?
if [ $rc -eq 0 ]; then c_ok "alice got control (exit $rc)"; else c_bad "alice was refused (exit $rc)"; fi

c_head "control events in the shared history"
"$BIN/bench" tail "127.0.0.1:$PORT" -n 4K | grep -a 'control'
show_server_log 8
