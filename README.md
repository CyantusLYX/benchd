# benchd — multi-user remote serial/UART console

`benchd` shares one serial port over TCP. Many clients watch the UART output live; at most one
of them at a time holds **control** and can type into it. Late joiners get the recent history
replayed. `benchsim` replays a U-Boot boot log at real baud-rate pace, so no hardware is needed.

Network Programming midterm — C (gnu11), Linux, libc only, **fork() per client, no threads**.

## Build and run

```bash
make            # release: build/bin, build/lib   (make debug -> build/debug/...)
```

```bash
build/bin/benchsim -l sim/pynq_boot.log --loop &           # fake device on /tmp/benchsim0
build/bin/benchd -d /tmp/benchsim0 &                       # server on :9000
build/bin/bench localhost                                  # interactive console
```

Keys in the console: `Ctrl-]` then `t` take, `r` release, `i` info, `s` timestamps, `h` hex,
`l` record, `:` command line (`:baud 9600`, `:log file`), `?` help, `q` quit.

Non-interactive: `bench info|tail|watch|send <host[:port]> ...` (see `bench -h`).

## Architecture

```
            ┌──────────────────────────── benchd ─────────────────────────────┐
            │                                                                 │
 TCP :9000  │  master ── accept() ── fork() ──► child (one per client) ──┐    │
 clients ◄──┼─────────────────────────────────── handshake / replay      │    │
            │      │ SIGCHLD: waitpid, release control of dead child      │    │
            │      │                                                     │    │
            │      ▼        shared memory (mmap MAP_SHARED, before fork)  │    │
            │  ┌───────────────────────────────────────────────────┐     │    │
            │  │ robust process-shared mutex                       │◄────┘    │
            │  │ state: device, control (ctrl_pid), slots, stats   │ copy     │
            │  │ ring buffer: DATA + EVENT records, offsets only   │ [off,head)│
            │  │   grow; each child keeps its own read offset      │          │
            │  └───────────────────────────────────────────────────┘          │
            │      ▲ append DATA/EVENT                                         │
            │  reader ◄── input pipe ◄── children (INPUT/BAUD/RELEASE,        │
            │    │                       atomic <= PIPE_BUF messages)         │
            └────┼────────────────────────────────────────────────────────────┘
                 ▼
          serial device (/dev/ttyUSB0, or benchsim's pty /tmp/benchsim0)
```

- Only the **reader** holds the serial fd, so no two processes steal bytes from each other.
- Broadcast needs no child-to-child IPC: everything (UART data and events) goes into one ring
  and every child reads it at its own offset, so all viewers see data and events in the same
  order, replay included.
- fork() isolates slow clients: a stuck viewer only blocks its own child.
- The whole IPC is one shared memory block, one robust mutex and one pipe.

## Requirement → where to look

| Requirement | Implementation | Proof |
|---|---|---|
| fork() per connection, no threads | `server/main.c` `handle_accept`, `server/session.c` | `pstree` in `scripts/stress.sh`; `make test` checks no `pthread_create` |
| ≥ 10 concurrent clients | 20 × `bench watch` | `make stress` → `out/stress/summary.md` (identical sha256 for all 20) |
| packet capture & analysis | binary protocol `include/bench_proto.h`, Wireshark dissector `tools/benchd.lua` | `make capture` → `captures/session.pcap`, `captures/rst.pcap` |
| two-level debug log | `include/bench_log.h` (compile time), `lib/log.c` + shm level + SIGUSR1 (run time) | `strings build/bin/benchd \| grep 'ring append'` finds nothing; `build/debug/bin/benchd` has it |
| shared `.so` | `lib/*.c` → `libbench.so.1` | `ldd build/bin/benchd build/bin/bench` |
| system info request | `INFO_REQ` → `server/sysinfo.c` | `bench info localhost`, `Ctrl-] i` |
| robustness (12 mechanisms) | `--unsafe=<name>` switches, `tools/bench-evil.c` | `scripts/demo_rN.sh safe\|unsafe` |
| Makefile | `Makefile` | `make release debug test stress capture clean dist` |

## Robustness demos

Each script starts its own benchsim + benchd and prints a verdict. Run it with `safe`, then
with `unsafe`:

| script | mechanism | unsafe result | safe result |
|---|---|---|---|
| `demo_r1.sh` | zombie | 20 `<defunct>` children | reaped |
| `demo_r2.sh` | sigpipe | master killed by SIGPIPE (status 141) | keeps serving |
| `demo_r3.sh` | validate | child crashes (SIGSEGV / stack smashing) | `ERROR BAD_LENGTH`, clean close |
| `demo_r4.sh` | readn | 1-byte "header" → bogus BAD_MAGIC | dribbled packets reassembled |
| `demo_r5.sh` | timeout | 32 silent sockets lock everyone out | dropped after 5 s |
| `demo_r6.sh` | ctrl-release | nobody can take control after a crash | released immediately |
| `demo_r7.sh` | robust-mutex | whole server frozen | `EOWNERDEAD recovered`, keeps running |
| `demo_r8.sh` | dev-reopen | reader at 100 % CPU, never recovers | DEVICE LOST → reconnected |
| `demo_r9.sh` | max-clients | forks until EAGAIN | 32 served, rest `SERVER_BUSY` |
| `demo_r10.sh` | overrun | ring headers / stale bytes on screen | `DROPPED` event, clean data |
| `demo_r11.sh` | term-restore | shell left in raw mode | terminal restored |
| `demo_r12.sh` | reconnect | client exits | reconnects, "server restarted" |

`demo_r7.sh` needs the debug build (it runs `make debug` if needed). `demo_r11/12` need GNU
`screen`. Scripts honour `PORT`, `DEV`, `BIN`.

## Files

```
include/   bench_proto.h bench_log.h bench_net.h bench_util.h
lib/       proto.c crc32.c net.c log.c util.c                 -> libbench.so.1
server/    main.c reader.c session.c ring.c control.c sysinfo.c robust.c server.h
client/    main.c term.c ui.c cmd.c session.c subcmd.c client.h
tools/     benchsim.c bench-evil.c benchd.lua
sim/       pynq_boot.log    (real PYNQ/EBAZ4205 boot, U-Boot -> login, recorded through benchd)
           onkyo_boot.log   (placeholder: no Onkyo capture available)
captures/pynq/             raw recordings from the real board (see docs/DECISIONS.md)
scripts/   lib.sh test.sh stress.sh capture.sh demo_r1..r12.sh
docs/      DECISIONS.md     (implementation choices)
```
