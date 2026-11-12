#!/usr/bin/env bash
# stress.sh — proof that benchd serves >= 10 clients at once (§10.1).
#
#  1. benchsim (--loop) + benchd
#  2. 20 x `bench watch --name botNN --duration 20 --out out/stress/botNN.bin` started together
#  3. at t=5 s: pstree (master + reader + 20 children), established TCP connections, bench info
#  4. afterwards: every bot replayed the full history from the same offset, so over the range
#     all files cover (the shortest file) the bytes must be identical -> one sha256 for all 20.
. "$(dirname "$0")/lib.sh"
need_bins
N=${N:-20}
DUR=${DUR:-20}
rm -f "$WORK"/bot*.bin "$WORK"/bot*.err

c_head "start benchsim + benchd (history 1M, max-clients 32)"
start_sim --loop
start_server --history 1M --max-clients 32
sleep 2

c_head "start $N watchers at once"
T0=$(date +%s.%N)
BOTS=()
for i in $(seq -w 1 "$N"); do
    "$BIN/bench" watch "127.0.0.1:$PORT" --name "bot$i" -n 1M --duration "$DUR" \
        --out "$WORK/bot$i.bin" >/dev/null 2>"$WORK/bot$i.err" &
    BOTS+=($!)
done
sleep 5

c_head "t=5s snapshot"
pstree -p "$D_PID" | tee "$WORK/pstree.txt"
nchild=$(pgrep -P "$D_PID" -x benchd-child | wc -l)
est=$(ss -tn state established "( sport = :$PORT )" | tail -n +2 | wc -l)
echo "ss -tn state established '( sport = :$PORT )': $est connections" | tee "$WORK/ss.txt"
info | tee "$WORK/info.txt" | grep -E '^server\.(clients|max_clients)=|^hist\.'
c_note "(server.clients includes the 'bench info' connection that asked)"

c_head "waiting for the watchers to finish (${DUR}s)"
fail=0
for p in "${BOTS[@]}"; do wait "$p" || fail=$((fail + 1)); done

c_head "verify: identical broadcast to every client"
min=
first_offsets=$(cat "$WORK"/bot*.err | sed -n 's/.*first_offset=\([0-9]*\).*/\1/p' | sort -u)
for f in "$WORK"/bot*.bin; do
    s=$(stat -c %s "$f")
    if [ -z "$min" ] || [ "$s" -lt "$min" ]; then min=$s; fi
done
printf '%-8s %10s %10s  %s\n' bot bytes compared "sha256 of common range"
for f in "$WORK"/bot*.bin; do
    h=$(head -c "$min" "$f" | sha256sum | cut -c1-16)
    printf '%-8s %10s %10s  %s\n' "$(basename "$f" .bin)" "$(stat -c %s "$f")" "$min" "$h"
done | tee "$WORK/hashes.txt"
uniq_hashes=$(awk '{print $4}' "$WORK/hashes.txt" | sort -u | wc -l)

c_head "summary"
{
    printf '| item | result |\n|---|---|\n'
    printf '| concurrent watchers started | %s |\n' "$N"
    printf '| benchd children at t=5s (pstree) | %s client children + 1 reader |\n' "$nchild"
    printf '| established TCP connections at t=5s | %s |\n' "$est"
    printf '| server.clients at t=5s | %s |\n' "$(sed -n 's/^server.clients=//p' "$WORK/info.txt")"
    printf '| watchers that exited with an error | %s |\n' "$fail"
    printf '| first offset of every stream | %s |\n' "$(echo $first_offsets)"
    printf '| bytes compared per client | %s |\n' "$min"
    printf '| distinct sha256 over the common range | %s |\n' "$uniq_hashes"
} | tee "$WORK/summary.md"
if [ "$uniq_hashes" -eq 1 ] && [ "$fail" -eq 0 ] && [ "$nchild" -ge "$N" ] && [ "$(echo "$first_offsets" | wc -l)" -eq 1 ]; then
    c_ok "PASS: $N clients served concurrently and all received identical data"
else
    c_bad "FAIL"
    exit 1
fi
