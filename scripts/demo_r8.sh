#!/usr/bin/env bash
# demo_r8.sh safe|unsafe — R8 `dev-reopen`: the serial device is unplugged and plugged back.
#
#   unsafe: read() keeps returning EIO and the reader just loops -> 100% CPU, and the device
#           is never reopened (no more data even after it comes back).
#   safe:   DEVICE_LOST event, reopen every 500 ms, DEVICE_BACK, data flows again.
. "$(dirname "$0")/lib.sh"
need_bins
parse_mode "$1" dev-reopen

start_sim --loop
start_server $UNSAFE_OPT
sleep 1
R=$(reader_pid)
c_head "before: reader pid $R uses $(cpu_pct "$R")% CPU; $(info | grep -E '^dev\.(state|rx_bytes)=' | tr '\n' ' ')"

c_head "unplug: kill benchsim"
stop_sim
sleep 1
c_note "$(info | grep -E '^dev\.state=')"
c_head "reader CPU while the device is gone: $(cpu_pct "$R")%"
top -b -n 1 -p "$R" | tail -2

c_head "plug back in: restart benchsim"
start_sim --loop
sleep 2
rx1=$(info_key dev.rx_bytes); sleep 2; rx2=$(info_key dev.rx_bytes)
echo "$(info | grep -E '^dev\.(state|reconnects)=' | tr '\n' ' ') rx_bytes $rx1 -> $rx2"
if [ "$rx2" -gt "$rx1" ] 2>/dev/null; then c_ok "data is flowing again"; else c_bad "no new data from the device"; fi
c_head "reader CPU now: $(cpu_pct "$R")%"

c_head "device events in the shared history"
"$BIN/bench" tail "127.0.0.1:$PORT" -n 64K | grep -a '──── device'
show_server_log 6
