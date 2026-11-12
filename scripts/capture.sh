#!/usr/bin/env bash
# capture.sh — packet captures for the report (§10.2).
#
#   captures/session.pcap  connect -> replay -> live data -> info -> take -> "version\n"
#                          -> release -> quit
#   captures/rst.pcap      bench-evil take-and-die: the client vanishes -> compare FIN vs RST
#
# Capture tool: `sudo tcpdump -i lo` as in the spec; if dumpcap (Wireshark) has capture
# capabilities it is used instead so no root is needed. Override with CAPTURE=tcpdump|dumpcap.
# If tshark is installed, a decoded summary is printed with tools/benchd.lua.
. "$(dirname "$0")/lib.sh"
need_bins
CAPDIR=$ROOT/captures
mkdir -p "$CAPDIR"
LUA=$ROOT/tools/benchd.lua

pick_capture() {
    if [ -n "$CAPTURE" ]; then echo "$CAPTURE"; return; fi
    local d
    d=$(command -v dumpcap)
    if [ -n "$d" ] && [ -x "$d" ] && getcap "$d" 2>/dev/null | grep -q cap_net_raw; then
        echo dumpcap
    else
        echo tcpdump
    fi
}
CAP=$(pick_capture)

# cap_start <file> : start capturing loopback traffic on $PORT
cap_start() {
    rm -f "$1"
    if [ "$CAP" = dumpcap ]; then
        dumpcap -q -i lo -f "tcp port $PORT" -w "$1" 2>"$WORK/capture.err" &
    else
        sudo tcpdump -i lo -U -Z "$(id -un)" -w "$1" "tcp port $PORT" 2>"$WORK/capture.err" &
    fi
    CAP_PID=$!
    sleep 1.5
}
cap_stop() {
    sleep 0.5
    if [ "$CAP" = dumpcap ]; then kill -INT "$CAP_PID"; else sudo kill -INT "$CAP_PID" 2>/dev/null || kill -INT "$CAP_PID"; fi
    wait "$CAP_PID" 2>/dev/null
}

summary() {   # summary <pcap>
    command -v tshark >/dev/null || { c_note "(install tshark for a decoded summary)"; return; }
    tshark -X lua_script:"$LUA" -o "tcp.desegment_tcp_streams:TRUE" -r "$1" \
        -d "tcp.port==$PORT,benchd" -Y benchd -T fields -e frame.number -e tcp.srcport -e tcp.dstport \
        -e _ws.col.Info 2>/dev/null | cut -c1-140
}

c_head "capture tool: $CAP (port $PORT)"
start_sim
start_server
sleep 2.5      # some boot output in the history first

# ---------- session.pcap ----------
c_head "captures/session.pcap: connect, replay, live, info, take, 'version\\n', release, quit"
cap_start "$CAPDIR/session.pcap"
if command -v screen >/dev/null; then
    S=cap-$$
    LC_ALL=C.UTF-8 screen -dmS "$S" bash -c "'$BIN/bench' --name capture -n 2K 127.0.0.1:$PORT; sleep 1"
    sleep 2                                   # replay + live data
    screen -S "$S" -X stuff $'\x1di'; sleep 1  # info
    screen -S "$S" -X stuff $'\x1dt'; sleep 1  # take
    screen -S "$S" -X stuff $'version\n'; sleep 1.5
    screen -S "$S" -X stuff $'\x1dr'; sleep 1  # release
    screen -S "$S" -X stuff $'\x1dq'; sleep 0.5
    screen -S "$S" -X quit 2>/dev/null

else
    c_note "(GNU screen not found: using the non-interactive subcommands instead)"
    "$BIN/bench" tail "127.0.0.1:$PORT" -n 2K >/dev/null
    "$BIN/bench" info "127.0.0.1:$PORT" >/dev/null
    "$BIN/bench" send "127.0.0.1:$PORT" 'version\n' --wait 1500 >/dev/null
fi
cap_stop
ls -l "$CAPDIR/session.pcap"
summary "$CAPDIR/session.pcap" | grep -v '^[0-9]*\s[0-9]*\s[0-9]*\sDATA' | head -40
c_note "(DATA-only frames omitted above: $(summary "$CAPDIR/session.pcap" | grep -c DATA) frames carry DATA)"

# ---------- rst.pcap ----------
c_head "captures/rst.pcap: normal quit (FIN) vs take-and-die"
cap_start "$CAPDIR/rst.pcap"
"$BIN/bench" info "127.0.0.1:$PORT" >/dev/null           # orderly close: FIN
"$BIN/bench-evil" take-and-die "127.0.0.1:$PORT" --name crashy
sleep 0.5
cap_stop
ls -l "$CAPDIR/rst.pcap"
if command -v tshark >/dev/null; then
    tshark -r "$CAPDIR/rst.pcap" -Y 'tcp.flags.fin==1 || tcp.flags.reset==1' \
        -T fields -e frame.number -e tcp.srcport -e tcp.dstport -e tcp.flags.str 2>/dev/null
fi
c_note "Open in Wireshark with: wireshark -X lua_script:tools/benchd.lua captures/session.pcap"
