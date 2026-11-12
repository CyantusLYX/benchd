# scripts/lib.sh — shared helpers for the demo / test scripts (source it, do not run it).
#
# Environment knobs:
#   BIN   directory with the executables   (default: build/bin)
#   PORT  TCP port for benchd              (default: 9000)
#   DEV   benchsim pty symlink             (default: /tmp/benchsim0)
#   WORK  where logs and outputs go        (default: out/<script name>)

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${BIN:-$ROOT/build/bin}
PORT=${PORT:-9000}
DEV=${DEV:-/tmp/benchsim0}
SCRIPT_NAME=$(basename "$0" .sh)
WORK=${WORK:-$ROOT/out/$SCRIPT_NAME}
LOG_SIM=$WORK/benchsim.log
LOG_D=$WORK/benchd.log
mkdir -p "$WORK"
: >"$LOG_SIM"
: >"$LOG_D"

BG_PIDS=()
SIM_PID=
D_PID=

c_head() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
c_ok()   { printf '\033[1;32m%s\033[0m\n' "$*"; }
c_bad()  { printf '\033[1;31m%s\033[0m\n' "$*"; }
c_note() { printf '\033[2m%s\033[0m\n' "$*"; }

cleanup() {
    for p in "${BG_PIDS[@]}"; do kill "$p" 2>/dev/null; done
    # give benchd its 1 s shutdown window, then make sure nothing is left behind
    sleep 1.3
    for p in "${BG_PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
}
trap cleanup EXIT
trap 'exit 130' INT TERM

need_bins() {
    for b in benchd bench benchsim bench-evil; do
        [ -x "$BIN/$b" ] || { echo "missing $BIN/$b — run 'make' first" >&2; exit 1; }
    done
}

# start_sim [benchsim options...]
start_sim() {
    rm -f "$DEV"
    "$BIN/benchsim" -L "$DEV" "$@" 2>>"$LOG_SIM" &
    SIM_PID=$!
    BG_PIDS+=("$SIM_PID")
    for _ in $(seq 50); do [ -e "$DEV" ] && return 0; sleep 0.05; done
    echo "benchsim did not come up" >&2
    return 1
}

stop_sim() { [ -n "$SIM_PID" ] && kill "$SIM_PID" 2>/dev/null; wait "$SIM_PID" 2>/dev/null; SIM_PID=; }

wait_port() {
    for _ in $(seq 100); do
        ss -ltn "sport = :$PORT" 2>/dev/null | grep -q LISTEN && return 0
        sleep 0.05
    done
    echo "benchd is not listening on $PORT" >&2
    return 1
}

# start_server [benchd options...]
start_server() {
    "$BIN/benchd" -d "$DEV" -p "$PORT" "$@" 2>>"$LOG_D" &
    D_PID=$!
    BG_PIDS+=("$D_PID")
    wait_port
}

server_alive() { kill -0 "$D_PID" 2>/dev/null && ss -ltn "sport = :$PORT" | grep -q LISTEN; }
reader_pid()   { pgrep -P "$D_PID" -x benchd-reader | head -1; }
children_of()  { pgrep -P "$D_PID" 2>/dev/null; }
info()         { "$BIN/bench" info --raw "127.0.0.1:$PORT" 2>&1; }
info_key()     { info | sed -n "s/^$1=//p"; }

# CPU usage (percent of one core) of a pid over ~1 s, from /proc/<pid>/stat.
cpu_pct() {
    local pid=$1 a b hz
    hz=$(getconf CLK_TCK)
    a=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null) || { echo "?"; return; }
    sleep 1
    b=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null) || { echo "?"; return; }
    echo $(( (b - a) * 100 / hz ))
}

# mode argument handling for demo_rN.sh: sets MODE and UNSAFE_OPT
parse_mode() {
    MODE=${1:-safe}
    case "$MODE" in
        safe)   UNSAFE_OPT= ;;
        unsafe) UNSAFE_OPT="--unsafe=$2" ;;
        *) echo "usage: $0 safe|unsafe" >&2; exit 1 ;;
    esac
    if [ "$MODE" = unsafe ]; then
        c_bad "### $SCRIPT_NAME: UNSAFE — mechanism '$2' disabled"
    else
        c_ok "### $SCRIPT_NAME: SAFE — mechanism '$2' enabled"
    fi
}

show_server_log() {
    c_head "benchd log (last ${1:-15} lines, $LOG_D)"
    tail -n "${1:-15}" "$LOG_D"
}
