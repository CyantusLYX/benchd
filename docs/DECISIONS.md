# DECISIONS — implementation choices not fixed by the spec

One line per decision, grouped by area. "Spec" = the project specification (§ numbers refer to it).

## Build and layout
- The working directory is the project root (the spec draws it as `benchd/`); `make dist` puts everything under `benchd/` inside `112590023_benchd.tar.gz`.
- Release builds go to `build/{bin,lib}`, debug builds go to `build/debug/{bin,lib}`, and objects live under each tree's own `obj/`, so switching modes never mixes objects.
- `libbench.so.1` is the real file (soname) and `libbench.so` is a symlink used only at link time; executables find the library through `-rpath '$ORIGIN/../lib'`.
- Extra warning flags `-Wshadow -Wformat=2` and `-fstack-protector-strong` are on in both modes (the latter also makes the R3 overflow show up as a crash).

## Threads and synchronisation
- **No threads anywhere.** `pthread_create` is never called (`make test` checks the sources and the dynamic symbols). The only pthread object is a `pthread_mutex_t` with `PTHREAD_PROCESS_SHARED | PTHREAD_MUTEX_ROBUST` inside the `MAP_SHARED` mapping: an inter-*process* lock, see `server/robust.c`.
- Logging and other I/O never happen inside the lock: messages about a recovery or a control release are collected under the lock and printed after unlocking.

## Protocol
- DATA/EVENT `offset` is the logical ring offset at the **end** of that record (where the next record starts). A client resumes with "the last offset I saw"; REPLAY_END/DROPPED carry the same kind of offset.
- Added error code `BAD_REQUEST = 12` (e.g. unsupported baud). It is not a protocol violation, so the connection stays open.
- Payload layouts are packed structs with big-endian fields (`include/bench_proto.h`). `bn_send` writes header and payload in a single `write()`.
- `server.clients` in INFO counts every client that finished the handshake, including the one that asked. The stress test therefore shows 21 for 20 bots plus the `bench info` connection.
- INFO has one extra key, `server.lock_recoveries` (the number of EOWNERDEAD recoveries), which is useful in the R7 demo.

## Server
- The shared memory also holds a slot table (`BENCH_MAX_SLOTS = 1024`): pid, joined flag, name, peer, stream offset. The master uses it to count children for `--max-clients`, children use it for the viewer count, and shutdown uses it to wait until every child has streamed the SERVER_SHUTDOWN event.
- An explicit RELEASE, and the release on a normal disconnect, both travel the input pipe as a message, and **the reader** performs them. That keeps them ordered after INPUT the same child already forwarded: `bench send "x"` = TAKE, INPUT, RELEASE never loses the input. The master's SIGCHLD path (crashed child) still releases directly in shm.
- Shutdown sends SIGTERM to each known child pid and to the reader instead of `kill(0, …)`. When benchd is started from a script it shares the script's process group, so a group kill would hit the script too. Children ignore SIGINT so a terminal Ctrl-C reaches only the master, which coordinates the shutdown.
- Children and the reader use `prctl(PR_SET_PDEATHSIG, SIGTERM)`, so if the master dies (e.g. R2 unsafe) the whole server goes with it instead of orphans serving forever.
- The reader's SIGTERM handler also arms `alarm(2)`: if it is stuck on a lock whose owner died (R7 unsafe), it still ends.
- If the reader dies, the master logs ERROR and forks a new one.
- Processes are named `benchd-reader` / `benchd-child` (`PR_SET_NAME`) so `pstree`/`ps` output reads well in the report.
- The listener is dual-stack (IPv6 socket with `IPV6_V6ONLY=0`) and falls back to IPv4.
- Records evicted from a corrupt chain (impossible length) reset `tail = head` rather than walking garbage.

## Robustness switches (§9)
- R2: when full, the master waits up to 100 ms for the client's HELLO so the log can name who was turned away, then answers SERVER_BUSY. On Linux the first `write()` after an RST returns ECONNRESET without a signal. The HELLO read consumes that error, so the following write raises SIGPIPE. That is exactly the realistic path that kills an unprotected master.
- R2 unsafe explicitly sets `SIGPIPE` back to `SIG_DFL`. An ignored disposition is inherited across `exec`, and launchers such as Python ignore SIGPIPE, so without this the unsafe demo could silently behave like the safe one.
- R3: in unsafe mode the 1 MiB `len` overflows a 4 KiB stack buffer. Depending on what gets overwritten, the child dies with `stack smashing detected` or SIGSEGV. It often dies while holding the shared lock, and then R7's recovery message appears too.
- R4 unsafe uses one `recv()` for the header and one for the payload, with no loop.
- R5 safe uses more than the HELLO deadline: `SO_RCVTIMEO` (5 s during the handshake, 60 s afterwards), `SO_SNDTIMEO` 60 s (a peer that never reads cannot pin a child), and TCP keepalive (30/10/3).
- R6 unsafe disables all four release paths (disconnect, SIGCHLD, `kill(pid,0)` on TAKE, idle). CTRL_RESULT then reports `auto_release_in = UINT32_MAX`, shown as "no auto-release".
- R7 fault injection (debug build only): `BENCH_FAULT=die_in_lock[:N]` makes the child of the N-th connection (default 1) `abort()` while holding the lock, with core dumps disabled for that process.
- R10 unsafe: the naive reader does not compare its offset with `tail`. If it reads a nonsense record length it clamps it to 1024 and carries on, so overwritten bytes (including record headers) reach the screen.
- The client honours `--unsafe` for `term-restore`, `reconnect`, `readn`, `validate` and `sigpipe`. The server ignores client-only names and vice versa.
- `SO_REUSEADDR` and retrying `accept()` on EINTR are always on (no switch), as the spec says.

## Logging (§7)
- Disabled log macros expand to `do { (void)sizeof(printf(...)); } while (0)` instead of an empty `do {} while (0)`. Nothing is evaluated and the format string is **not** emitted (verified with `strings`), but the arguments are still type-checked and no "unused variable" warnings appear.
- The `#if LOG_COMPILE_LEVEL >= N` tests use numeric literals, because the preprocessor cannot see enum values.
- SIGUSR1 cycles INFO → DEBUG → TRACE → ERROR → WARN → INFO. WARN is added so the cycle visits every level. Above the compile-time cap the change is accepted and logged with a note that this build lacks those messages.
- TRACE logs every packet header in `bn_send`/`bn_recv` (`tx/rx pkt type= len= crc=`) for lining the log up with tcpdump.

## Client
- Raw mode keeps `OPOST|ONLCR` so bare `\n` from a UART still returns to column 1.
- In text mode, device output goes through a small ANSI filter (`client/ui.c` `esc_byte`). Its state lives in `struct app`, so a sequence split across two DATA records still parses. Colours (`ESC[…m`), erase-in-line (`K`), relative cursor moves (`A B C D G`) and `@ P X` pass through. `ESC[J` becomes `ESC[K`, so the status bar is never cleared. Absolute positioning, scroll regions, private modes (`ESC[?2004h`), queries, OSC titles and every other ESC sequence are dropped. Other control bytes (NUL, BEL, …) are dropped too. Use `:hex` to see every byte. This came from testing on a real PYNQ board: its zsh prompt sends all of these.
- `:ts` stamps each line with the server timestamp of the record that holds the line's first character.
- The status bar never writes the last column (avoids auto-wrap on the bottom row). Terminals that draw East-Asian "ambiguous width" characters (`─`, `│`) as two columns, e.g. GNU screen under a zh_TW locale, wrap separators. Use a UTF-8 non-CJK locale or a terminal with narrow ambiguous width.
- `Ctrl-] l` without a name records to `bench-YYYYmmdd-HHMMSS.log`. `:log <file>` starts a named recording and `:log` alone stops it.
- Quitting (and every subcommand) does BYE, `shutdown(SHUT_WR)`, then drains until EOF, so the TCP close is a clean FIN exchange rather than an RST from unread data. `captures/rst.pcap` contrasts the two.
- `bench info` prints a box on a terminal and raw `key=value` otherwise (or with `--raw`). `bench watch` takes `-n` (default 0) and prints a one-line summary (records, bytes, first/last offset) to stderr, which the stress test uses. `bench send` takes `--wait MS` to print the device's answer.
- Before the first successful connection, the reconnect back-off also applies (status shows CONNECTING). `--no-reconnect` exits instead.

## benchsim
- It holds its own slave fd open, which keeps the raw termios settings and avoids EIO on the master while benchd is away. Output that nobody reads is dropped, as on a real UART.
- Any key during playback stops "autoboot" and drops to the prompt. Unlike real U-Boot the key is not swallowed, so `bench send "version\r"` works at any time.
- A `#delay` starts after the text queued before it has gone out on the simulated wire.
- `sim/pynq_boot.log` is a real capture: an EBAZ4205 (Zynq 7z010, PYNQ Linux) `sudo reboot` from the U-Boot banner to `pynq login:`, recorded through benchd on `/dev/ttyUSB2`. A recorder client timestamped every DATA packet, and gaps of 100 ms or more (after subtracting the bytes' own wire time at 115200) became `#delay` lines at the next line start. NUL padding was stripped, and lines are stored with LF because benchsim appends CRLF. The raw bytes are in `captures/pynq/` (`pynq_boot_full.log` ends at the logged-in prompt, `reboot2_timed.jsonl` keeps the timing, `session_raw.log` and `benchd.log` cover the whole session including a power cycle).
- A power cycle loses U-Boot and the kernel messages: the board's CH340 USB-serial adapter is powered by the board, so it re-enumerates about 2.5 s after power-on (benchd logs device lost / is back). A warm `sudo reboot` keeps the adapter powered, so that is what was recorded.
- `sim/onkyo_boot.log` is a placeholder (lines starting `#placeholder`) until the author adds the real capture. While a placeholder line exists, benchsim warns and plays a built-in, explicitly generic U-Boot/Linux-style text.

## bench-evil
- Extra mode `take-hold` (take control and sit idle for `--sec`), used by `make test` and the idle-release check.
- `lag` sets `SO_RCVBUF` to 4 KiB **before** `connect()`. Otherwise loopback autotuning would buffer megabytes and hide the lag. `demo_r10.sh` also plays a pause-free log at 921600 baud.

## Scripts
- `capture.sh` uses `sudo tcpdump -i lo -Z $USER` as in the spec. If `dumpcap` is executable with capture capabilities it is used instead (no root needed). Override with `CAPTURE=tcpdump|dumpcap`.
- The interactive demos (`capture.sh`, `demo_r11.sh`, `demo_r12.sh`) drive the TUI in a detached GNU screen. `capture.sh` falls back to the subcommands without screen.
- `demo_r9.sh` sets `ulimit -u` to the user's current **task** count + 150 (RLIMIT_NPROC counts threads, not processes).
