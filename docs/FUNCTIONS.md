# FUNCTIONS — benchd 每個檔案、每個函式的設計與用途

本文件逐檔說明專案裡每一個函式（以及標頭檔中的型別、巨集、常數）為什麼存在、怎麼設計、與誰互動。程式碼識別字、檔名與常數一律保留英文。

## 如何閱讀這份文件

**行號基準。** 所有 `檔案:行號` 都對應 commit `65dc371` 的工作目錄（working tree）。工作目錄在該 commit 之上只有 `docs/` 底下的改動，`include/`、`lib/`、`server/`、`client/`、`tools/`、`scripts/` 沒有未提交的修改，所以行號與該 commit 一致。之後若修改原始碼，行號會位移，以函式名稱為準。

**每個函式一個區塊。** 標題固定為 `### `name``，標題下一行是斜體的「位置」與「簽章」，接著是固定的欄位：

| 欄位 | 內容 |
|---|---|
| 位置 | 定義所在的 `檔案:行號`（函式第一行，即回傳型別與名稱所在行） |
| 簽章 | 原始碼中的完整宣告 |
| 用途 | 一句話說明這個函式解決什麼問題 |
| 設計 | 為什麼這樣寫：選了什麼做法、為了擋哪一類錯誤（若與 R1-R12 有關會標出） |
| 細節 | 實作上不顯而易見的地方：邊界條件、鎖的持有狀態、錯誤路徑、回傳值約定 |
| 關係 | 被誰呼叫、呼叫誰；引用的呼叫關係皆已用 grep 對照原始碼確認 |

**範圍。** 每個 `.c` 檔的所有函式（含 `static`）都有一個區塊。標頭檔（`include/*.h`、`server/server.h`、`client/client.h`）列的是型別、列舉、巨集與宣告，沒有函式本體的項目也用 `###` 區塊說明；`include/bench_net.h` 只有宣告，整理成一張表。`tools/benchd.lua` 是 Lua，區塊對應其中的函式或區域定義。`scripts/*.sh` 的區塊對應 shell 函式或腳本主流程。

**慣例。** 名稱後綴 `_locked` 代表呼叫時必須已持有 `shm_lock()`（`server/server.h` 的 `ctrl_*_locked`）。`LOGE/LOGW/LOGI/LOGD/LOGT` 是五級 log 巨集。`UNSAFE(R_xxx)` 為真代表使用者用 `--unsafe=` 關掉了對應的防護，程式碼走「天真版」路徑，用來示範 R1-R12 失效時的後果。

## 行程與檔案總覽

benchd 以 `fork()` 分出三種行程（沒有 thread），彼此只靠三樣東西溝通：一塊 `MAP_SHARED` 共享記憶體、其中的一把 robust process-shared mutex、一條 pipe（`server/server.h` 開頭的說明）。client 與工具則是各自獨立的行程，只透過 TCP 與 benchd 連接。

```
                                  使用者終端機 (raw mode)
                                         ▲ │ 按鍵
                                 stdout  │ ▼ stdin
 ┌────────────────────────────────────────────────────────┐
 │ bench  (client/*.c)   單一行程，一個 poll() 迴圈        │
 │   app_run: stdin -> cmd_keys -> app_send(BN_INPUT)     │
 │            socket -> handle_packet -> ui_data -> stdout│
 └───────────────────────────┬────────────────────────────┘     bench-evil (tools/bench-evil.c)
                             │ TCP :9000                         benchsim    (tools/benchsim.c)
                             │ 封包: magic|ver|type|len|crc|payload   benchd.lua  (Wireshark 解析器，
                             │ (lib/proto.c 編碼/解碼)                        不是行程，離線讀 pcap)
 ┌───────────────────────────┴──────────────────── benchd ───────────────────────────────────┐
 │                                                                                           │
 │  master  (server/main.c)                                                                  │
 │    listen_fd, poll(200ms) -> handle_accept -> fork() ──┐                                  │
 │    SIGCHLD 旗標 -> reap_children   SIGUSR1 -> cycle_log_level                             │
 │    spawn_reader() -> fork()                              │                                │
 │          │                                               ▼                                │
 │          │                         child x N  (server/session.c, 每個 client 一個)        │
 │          │                           handshake / pump(replay) / handle_packet / pump(live)│
 │          │                                  │ 讀寫           │ 寫入 PIPE_INPUT/BAUD/RELEASE│
 │          ▼                                  ▼                ▼ (<= PIPE_BUF，寫入是原子的) │
 │  reader  (server/reader.c)      ┌────────────────────────┐     ┌──────────┐               │
 │    唯一持有序列埠 fd             │ 共享記憶體 struct       │     │ input    │               │
 │    tty read -> ring_append ───► │ bench_shm:              │     │ pipe     │               │
 │    pipe read -> tty write ◄──── │  robust mutex, state,   │     └────┬─────┘               │
 │    (裝置遺失重開 / idle 釋放)   │  slots[], ring (head/   │          │                     │
 │          ▲                      │  tail), ctrl_pid, stats │          │                     │
 │          └──────────────────────┴────────────────────────┘◄─────────┘                     │
 └───────────────┬───────────────────────────────────────────────────────────────────────────┘
                 │ read()/write()，termios 8N1
                 ▼
        序列裝置 (/dev/ttyUSB0)  或  benchsim 建立的 pty (/tmp/benchsim0)
```

各原始檔所在行程：

| 檔案 | 在哪個行程執行 |
|---|---|
| `server/main.c` | master（`main`、`handle_accept`、`reap_children`、`shutdown_server` 等）。`spawn_reader()` 與 `handle_accept()` 內 `fork()` 之後，子行程在 `reader_main()` 或 `session_main()` 裡永遠不回到 master 的迴圈 |
| `server/reader.c` | reader（`reader_main()` 為入口）。`baud_supported()` 是例外：它也被 child 呼叫（`server/session.c:401`）來檢查 `BN_BAUD` 的值 |
| `server/session.c` | 每個 client 一個 child（`session_main()` 為入口） |
| `server/ring.c`、`server/control.c`、`server/robust.c` | 被 master、reader、child 共同呼叫，在共享記憶體上操作；呼叫端要持有 `shm_lock()`（`shm_lock`/`shm_unlock` 本身除外） |
| `server/sysinfo.c` | child（回應 `BN_INFO_REQ`，`server/session.c:382`） |
| `lib/*.c` | 編成 `libbench.so.1`，benchd、bench、benchsim、bench-evil 都動態連結它，所以每個行程都有 |
| `client/*.c` | `bench`（互動模式 `app_run`，或子命令 `sub_info/sub_tail/sub_watch/sub_send`） |
| `tools/benchsim.c` | 模擬裝置，獨立行程，建立 pty 並依真實 baud 速率重播開機 log |
| `tools/bench-evil.c` | 故障注入 client，獨立行程，用來觸發 R1-R12 的情境 |
| `tools/benchd.lua` | 不是行程，由 Wireshark/tshark 載入，解析 `benchd` 封包 |
| `scripts/*.sh` | 把上述行程組合起來跑測試、壓力測試、封包擷取與 R1-R12 示範 |

## 三條端到端呼叫鏈

以下行號皆已對照原始碼。箭頭 `->` 代表「呼叫」或「資料流向」。

### (a) benchd 啟動、accept()、fork()，到 child 的 live loop

```
main()                                        server/main.c:270
  getopt_long 解析參數                        server/main.c:288
  bench_unsafe_parse(optarg, &unsafe)         server/main.c:303   (--unsafe 清單 -> bitmask，lib/util.c:259)
  bench_log_init_level(verbose)               server/main.c:331
  mmap(MAP_SHARED|MAP_ANONYMOUS)              server/main.c:345   (struct bench_shm + history 大小的 ring，fork 之前建立)
  shm_mutex_init(&g_shm->lock)                server/main.c:350   (server/robust.c:28)
  ring_init(g_shm, g_cfg.history)             server/main.c:351   (server/ring.c:42)
  bench_log_set_level_ptr(&g_shm->log_level)  server/main.c:360   (所有行程共用 log 層級)
  pipe(g_cfg.input_pipe)                      server/main.c:362
  tcp_listen(g_cfg.port, 128)                 server/main.c:378   (lib/net.c:54)
  sigaction: SIGINT/SIGTERM on_term, SIGUSR1 on_usr1, SIGCHLD on_chld (R1 時才裝)
                                              server/main.c:384-396
  spawn_reader()                              server/main.c:399   (定義 :64)
    fork() -> 子行程 reader_main()            server/main.c:65, :72   (reader 的一生見鏈 (b)(c))
  while (!got_term):                          server/main.c:406
    poll(listen_fd, 200 ms)                   server/main.c:408
    got_chld -> reap_children()               server/main.c:409-412  (定義 :80)
    got_usr1 -> cycle_log_level()             server/main.c:413-416  (定義 :199)
    poll 回報可讀 -> handle_accept(&conn_seq) server/main.c:418  (定義 :134)
      accept()（EINTR 重試）                  server/main.c:140
      sock_peer_str(fd, peer, ...)            server/main.c:150
      shm_lock; slots_in_use; 找空 slot 並暫記 pid = -1; shm_unlock
                                              server/main.c:152-162
      已滿且未 --unsafe=max-clients -> reject_busy(fd, peer, in_use)，return
                                              server/main.c:164-173  (定義 :114)
      fork()                                  server/main.c:175
        pid == 0 -> session_main(fd, slot, seq, peer)   server/main.c:187-188
        pid  > 0 -> close(fd)，把 slot 的 pid 從 -1 改成真正的 pid   server/main.c:189-195

session_main()                                server/session.c:423   (child，永不 return，結束靠 finish() 內的 _exit)
  bench_log_set_role / prctl(PR_SET_NAME "benchd-child") / PR_SET_PDEATHSIG
                                              server/session.c:429-431
  signal(SIGINT/SIGUSR1, SIG_IGN)；close(listen_fd)；close(input_pipe[0])；pipe_wr = input_pipe[1]
                                              server/session.c:434-440
  set_keepalive + set_io_timeouts（R5 時才設） server/session.c:442-445   (lib/net.c:144, :155)
  handshake(&s, conn_seq)                     server/session.c:448   (定義 :189)
    poll 等 HELLO（5 s），bn_recv              server/session.c:197-210
    登記 slot，slots_recount，填 WELCOME   server/session.c:243-263
    bn_send(BN_WELCOME)                       server/session.c:287
    非續傳：bn_send(BN_REPLAY_BEGIN) -> pump(s, snap_head, 0) -> bn_send(BN_REPLAY_END)
                                              server/session.c:308-313
  for (;;) live loop                          server/session.c:451
    poll(fd, 20 ms)                           server/session.c:453
    最多 32 次 handle_packet(&s)（定義 :321）  server/session.c:457-461
    idle 超過 IDLE_TIMEOUT_MS -> finish        server/session.c:462-466
    pump(&s, UINT64_MAX, 1)（定義 :132）       server/session.c:467
```

### (b) 使用者敲一個鍵：client -> TCP -> child -> input pipe -> reader -> 序列埠

```
bench 的 app_run()                            client/session.c:332
  poll(stdin, socket, timeout)                client/session.c:372
  read(STDIN_FILENO, k, 256)                  client/session.c:382
  cmd_keys(a, k, n)                           client/session.c:385   (定義 client/cmd.c:156)
    非跳脫字元、已連線 (ST_LIVE)、持有控制權 (is_ctrl):
      a->ibuf[a->ilen++] = k[i]；ilen 為 0 時設 iflush_at = 現在 + 5 ms
                                              client/cmd.c:180-182
      ibuf 滿（64 bytes）-> app_flush_input   client/cmd.c:183-184
  迴圈尾端 now >= iflush_at -> app_flush_input(a)   client/session.c:406-407   (定義 :159)
    app_send(a, BN_INPUT, a->ibuf, a->ilen)   client/session.c:164   (定義 :148)
      conn_send -> bn_send                    client/session.c:61 -> lib/proto.c:46
        bench_crc32 -> bn_hdr_encode -> writen   lib/proto.c:52-57 -> lib/net.c:38
  ====================== TCP ======================
child 的 session_main 迴圈:                    server/session.c:451
  poll 可讀 -> handle_packet(&s)              server/session.c:458
    bn_recv(s->fd, &h, buf, 4096, &perr)      server/session.c:326   (lib/proto.c:77；readn 讀 header、驗 magic/len/crc，再讀 payload)
    case BN_INPUT                             server/session.c:341
      len > BN_MAX_INPUT -> violation()（R3） server/session.c:342-346
      shm_lock 看 ctrl_pid == me、dev_state == DEV_UP，並更新 ctrl_last_input_ns
                                              server/session.c:347-353
      非控制者 -> bn_send_error(NOT_CONTROLLER)；裝置遺失 -> bn_send_error(DEVICE_LOST)
                                              server/session.c:354-357
      pipe_send(s, PIPE_INPUT, buf, h.len)    server/session.c:357   (定義 :54)
        writen(pipe_wr, &m, PIPE_MSG_HDR + len)   server/session.c:59   (m 整體 <= PIPE_BUF，寫入原子)
  ====================== input pipe ======================
reader_main()                                 server/reader.c:203, 迴圈 :234
  poll(pipe_rd, tty_fd, 100 ms)               server/reader.c:236
  p[0].revents & POLLIN                       server/reader.c:261
    readn(pipe_rd, &m, PIPE_MSG_HDR)，再 readn payload   server/reader.c:263-265
    handle_pipe_msg(&m)                       server/reader.c:266   (定義 :139)
      shm_lock 再確認 m->pid == ctrl_pid（控制權可能已轉手）  server/reader.c:141-143
      case PIPE_INPUT: tty_write_all(m->data, m->len)   server/reader.c:159   (定義 :117)
        write(tty_fd, ...)                    server/reader.c:122
      成功後 tx_bytes += w                    server/reader.c:160-163
  ====================== 序列埠 ======================
```

### (c) 序列埠來了一個位元組：reader -> ring -> pump() -> TCP -> client 螢幕

```
reader_main() 迴圈                             server/reader.c:234
  poll 回報 tty_fd 可讀                        server/reader.c:242
  read(tty_fd, buf, REC_MAX_DATA)             server/reader.c:243
  shm_lock(g_shm)                             server/reader.c:245
    ring_append(g_shm, REC_DATA, 0, buf, r)   server/reader.c:246   (定義 server/ring.c:49)
      空間不足就逐筆推進 tail 淘汰最舊的記錄；寫入 ring_rec 與 payload；最後才 head += size（發佈）
                                              server/ring.c:49-70
    rx_bytes += r；last_rx_ns = now_ns()
  shm_unlock(g_shm)
  ====================== 共享記憶體 ring ======================
每個 child 的 session_main 迴圈                server/session.c:451
  pump(&s, UINT64_MAX, 1)                     server/session.c:467   (定義 :132)
    shm_lock；若 s->off < tail（被超車，R10）-> 送 DROPPED，s->off = tail   server/session.c:138-141
    ring_copy(g_shm, &s->off, end, copybuf, sizeof copybuf)   server/session.c:146   (定義 server/ring.c:109)
    更新 slots[s->slot].offset；shm_unlock
    對每筆記錄 send_record(s, r, data, o)     server/session.c:161   (定義 :81)
      REC_DATA: 填 bn_data_hdr（offset = 該記錄結尾的邏輯 offset）後
        bn_send(s->fd, BN_DATA, ...)          server/session.c:97
          bench_crc32 -> bn_hdr_encode -> writen   lib/proto.c:46
  ====================== TCP ======================
bench 的 app_run()                            client/session.c:332
  poll 回報 socket 可讀                        client/session.c:389
  conn_recv(&a->c, &h, buf, sizeof buf, &perr)   client/session.c:391   (定義 :53；內部 bn_recv，成功時更新 last_rx_ms)
  handle_packet(a, &h, buf)                   client/session.c:393   (定義 :211)
    case BN_DATA: 記下 a->c.pos = offset（重連續傳用），
      ui_data(a, data, n, ts_ns)              client/session.c:245   (定義 client/ui.c:180)
        可見字元累積成一段 run -> ob_put(...)   client/ui.c:204, :213, :226   (定義 client/term.c:44)
        控制字元 \n \r 與 ESC 序列另行處理（esc_byte，client/ui.c:124）
      若有開 log 檔（Ctrl-] l），fwrite 原始位元組   client/session.c:246-247
  迴圈尾端 ob_flush()                          client/session.c:422   (定義 client/term.c:32；write(STDOUT_FILENO, ...))
```

說明：

- (b) 之所以要經過 pipe 再由 reader 寫入序列埠，是因為只有 reader 持有序列埠 fd，避免多個行程同時寫入或搶讀（`docs/DECISIONS.md` 與 README「Architecture」說明）。pipe 訊息整體不超過 `PIPE_BUF`，多個 child 同時寫入也不會互相穿插。
- (c) 的廣播不需要 child 之間的 IPC：UART 資料與事件都進同一個 ring，每個 child 用自己的讀取位置（`struct sess` 的 `off`，同步寫入 `slots[].offset`）從 ring 取走，所以所有 viewer 看到的順序一致，history replay 也是同一條路徑（鏈 (a) 的 `pump(s, snap_head, 0)`）。
- 事件（控制權變更、裝置遺失、baud 變更、關機）也由 `ring_event` 寫進 ring，成為 `REC_EVENT` 記錄，經同一個 `pump()` 以 `BN_EVENT` 送出。

## 檔案索引

「函式／項目數」是各檔案區塊中 `###` 標題的數量：`.c`、`.lua`、`.sh` 為函式（或腳本）數；`.h` 為型別、巨集與宣告項目數（`include/bench_net.h` 沒有 `###` 區塊，7 個宣告整理在一張表裡）。連結為 GitHub 風格的錨點。

| 檔案 | 一句話角色 | 函式／項目數 | 連結 |
|---|---|---|---|
| `include/bench_proto.h` | 線上封包格式：常數、型別／錯誤／事件列舉、packed 結構、編解碼宣告 | 25 | [include/bench_proto.h](#includebench_protoh) |
| `include/bench_net.h` | socket 輔助函式宣告（readn/writen、listen、connect、選項） | 0（7 個宣告在表中） | [include/bench_net.h](#includebench_neth) |
| `include/bench_util.h` | 工具函式宣告與 R1-R12 的 `enum bench_robust`、`UNSAFE()` | 3 | [include/bench_util.h](#includebench_utilh) |
| `include/bench_log.h` | 兩階段 log 的巨集與介面 | 9 | [include/bench_log.h](#includebench_logh) |
| `server/server.h` | benchd 內部共用：共享記憶體結構、ring 記錄、pipe 訊息、跨檔宣告 | 42 | [server/server.h](#serverserverh) |
| `client/client.h` | `bench` 內部共用：連線、應用狀態、各模組宣告 | 44 | [client/client.h](#clientclienth) |
| `lib/crc32.c` | CRC-32 表與計算 | 2 | [lib/crc32.c](#libcrc32c) |
| `lib/log.c` | log 層級、輸出格式、檔案輸出 | 12 | [lib/log.c](#liblogc) |
| `lib/net.c` | readn/writen、TCP listen/connect、socket 選項 | 8 | [lib/net.c](#libnetc) |
| `lib/proto.c` | 封包標頭編解碼、`bn_send`/`bn_recv`、錯誤封包 | 8 | [lib/proto.c](#libprotoc) |
| `lib/util.c` | 時間／大小格式、字串清理、hexdump、`--unsafe` 解析 | 18 | [lib/util.c](#libutilc) |
| `server/main.c` | master：參數、共享記憶體、accept/fork、收屍、關機 | 11 | [server/main.c](#servermainc) |
| `server/reader.c` | reader：序列埠開啟／設定、tty 與 pipe 的搬運、裝置遺失重連 | 10 | [server/reader.c](#serverreaderc) |
| `server/session.c` | child：握手、replay、封包處理、串流 ring | 10 | [server/session.c](#serversessionc) |
| `server/ring.c` | 共享記憶體上的歷史 ring（附加、搜尋、複製、修復） | 8 | [server/ring.c](#serverringc) |
| `server/control.c` | 單一控制權的取得、釋放、閒置自動釋放 | 6 | [server/control.c](#servercontrolc) |
| `server/robust.c` | robust mutex 的初始化／加鎖／解鎖與 slot 計數、`--unsafe` 橫幅 | 6 | [server/robust.c](#serverrobustc) |
| `server/sysinfo.c` | 組出 `INFO` 回應的 key=value 文字 | 5 | [server/sysinfo.c](#serversysinfoc) |
| `client/main.c` | `bench` 入口：參數解析與子命令分派 | 2 | [client/main.c](#clientmainc) |
| `client/session.c` | 連線層與互動主迴圈、重連、封包分派 | 14 | [client/session.c](#clientsessionc) |
| `client/term.c` | 終端機 raw mode、輸出緩衝、還原 | 10 | [client/term.c](#clienttermc) |
| `client/ui.c` | 畫面：資料輸出、分隔線、狀態列、資訊框、說明 | 21 | [client/ui.c](#clientuic) |
| `client/cmd.c` | 按鍵處理、`Ctrl-]` 跳脫、`:` 命令列 | 6 | [client/cmd.c](#clientcmdc) |
| `client/subcmd.c` | 非互動子命令 `info`/`tail`/`watch`/`send` | 9 | [client/subcmd.c](#clientsubcmdc) |
| `tools/benchsim.c` | 以 pty 模擬序列裝置並重播開機 log | 14 | [tools/benchsim.c](#toolsbenchsimc) |
| `tools/bench-evil.c` | 故障注入 client，觸發 R1-R12 情境 | 19 | [tools/bench-evil.c](#toolsbench-evilc) |
| `tools/benchd.lua` | Wireshark/tshark 的 benchd 封包解析器 | 12 | [tools/benchd.lua](#toolsbenchdlua) |
| `scripts/lib.sh` | 腳本共用函式（啟動／停止行程、斷言、擷取） | 18 | [scripts/lib.sh](#scriptslibsh) |
| `scripts/test.sh` | `make test` 的整合測試 | 1 | [scripts/test.sh](#scriptstestsh) |
| `scripts/stress.sh` | 20 個並行 client 的壓力測試 | 1 | [scripts/stress.sh](#scriptsstresssh) |
| `scripts/capture.sh` | tcpdump 擷取 session.pcap 與 rst.pcap | 4 | [scripts/capture.sh](#scriptscapturesh) |
| `scripts/demo_r1.sh` | R1 zombie 示範 | 1 | [scripts/demo_r1.sh](#scriptsdemo_r1sh) |
| `scripts/demo_r2.sh` | R2 sigpipe 示範 | 1 | [scripts/demo_r2.sh](#scriptsdemo_r2sh) |
| `scripts/demo_r3.sh` | R3 validate 示範 | 1 | [scripts/demo_r3.sh](#scriptsdemo_r3sh) |
| `scripts/demo_r4.sh` | R4 readn 示範 | 1 | [scripts/demo_r4.sh](#scriptsdemo_r4sh) |
| `scripts/demo_r5.sh` | R5 timeout 示範 | 1 | [scripts/demo_r5.sh](#scriptsdemo_r5sh) |
| `scripts/demo_r6.sh` | R6 ctrl-release 示範 | 1 | [scripts/demo_r6.sh](#scriptsdemo_r6sh) |
| `scripts/demo_r7.sh` | R7 robust-mutex 示範 | 1 | [scripts/demo_r7.sh](#scriptsdemo_r7sh) |
| `scripts/demo_r8.sh` | R8 dev-reopen 示範 | 1 | [scripts/demo_r8.sh](#scriptsdemo_r8sh) |
| `scripts/demo_r9.sh` | R9 max-clients 示範 | 1 | [scripts/demo_r9.sh](#scriptsdemo_r9sh) |
| `scripts/demo_r10.sh` | R10 overrun 示範 | 2 | [scripts/demo_r10.sh](#scriptsdemo_r10sh) |
| `scripts/demo_r11.sh` | R11 term-restore 示範 | 1 | [scripts/demo_r11.sh](#scriptsdemo_r11sh) |
| `scripts/demo_r12.sh` | R12 reconnect 示範 | 1 | [scripts/demo_r12.sh](#scriptsdemo_r12sh) |

## 名詞對照

| 名詞 | 意義 | 主要出處 |
|---|---|---|
| master | benchd 最初的行程：持有 listening socket，`accept()` 後 `fork()` child，用 SIGCHLD 收屍，處理 SIGUSR1 與關機 | `server/main.c` |
| reader | 唯一打開序列埠的行程，process 名稱 `benchd-reader`；把 tty 讀到的資料附加進 ring，並把輸入 pipe 的訊息寫入 tty | `server/reader.c` `reader_main` |
| child | 每個 TCP client 對應一個 fork 出來的行程，process 名稱 `benchd-child`；負責握手、replay、串流與命令處理。慢 client 只會卡住自己的 child | `server/session.c` `session_main` |
| bench | 使用者用的 client，單一行程單一 `poll()` 迴圈；與 child 一一對應 | `client/` |
| 共享記憶體（shm） | 在 fork 之前以 `mmap(MAP_SHARED \| MAP_ANONYMOUS)` 建立的 `struct bench_shm`（全域指標 `g_shm`），含 mutex、狀態、slot 表與 ring | `server/server.h`、`server/main.c:345` |
| slot | `bench_shm.slots[]` 的一格（`struct bench_slot`，共 `BENCH_MAX_SLOTS` 格）：child 的 pid、是否已完成握手、名稱、peer、已送到的 offset。master 用它計算 `--max-clients`，child 用它計 viewer 數，關機時用它等所有 child 送完 `SERVER_SHUTDOWN`。`pid == 0` 代表空閒，`pid == -1` 代表 master 已保留、`fork()` 尚未回傳 | `server/server.h`、`server/main.c:134` `handle_accept` |
| ring | 共享記憶體中的歷史緩衝區，容量 `cap`（`--history`）。記錄為 `struct ring_rec`（`len/type/code/ts_ns`）加 payload，按 8 bytes 對齊（`REC_SIZE`），型別有 `REC_DATA` 與 `REC_EVENT` | `server/ring.c`、`server/server.h` |
| offset | 邏輯 offset：只增不減的 64 位元計數，實體位置是 `offset % cap`。封包 `DATA`/`EVENT` 的 `offset` 是該記錄「結尾」的邏輯 offset（下一筆記錄的開頭），client 重連時回報最後看到的 offset 即可續傳 | `docs/DECISIONS.md` Protocol 一節、`server/session.c:81` `send_record` |
| head | `bench_shm.head`：下一筆記錄要寫入的邏輯 offset。reader 寫完記錄之後才推進它（發佈） | `server/ring.c:49` `ring_append` |
| tail | `bench_shm.tail`：最舊且仍有效的記錄的邏輯 offset。空間不足時 `ring_append` 把它往前推；child 發現自己的 offset 小於 tail 就是被超車（R10），送 `DROPPED` 事件 | `server/ring.c`、`server/session.c:138` `pump` |
| 讀取位置 | 每個 child 自己的 `s->off`，也同步寫入 `slots[].offset`；各 child 獨立前進，彼此不影響 | `server/session.c:132` `pump` |
| control（控制權） | 同一時間最多一個 client 能輸入。以 `bench_shm.ctrl_pid` 表示持有者（0 為無人），`BN_TAKE` 取得、`BN_RELEASE`／斷線／閒置逾時釋放 | `server/control.c` |
| input pipe | children 到 reader 的單向 pipe，訊息為 `struct pipe_msg`（`pid/type/len/data`），型別 `PIPE_INPUT`、`PIPE_BAUD`、`PIPE_RELEASE`；整體不超過 `PIPE_BUF` 所以多寫者也不會穿插。明確的 RELEASE 也走這條 pipe 交給 reader 處理，確保排在同一個 child 先前送出的 INPUT 之後 | `server/server.h`、`server/session.c:54` `pipe_send` |
| viewer | 完成握手的 client 數（`bench_shm.viewers`），包含正在查詢的那一條連線 | `server/robust.c:70` `slots_recount` |
| replay | 新 client 握手後，child 先把歷史（從 `max(tail, head - replay_bytes)` 起，對齊到記錄邊界）送出，以 `REPLAY_BEGIN`／`REPLAY_END` 包住，之後進入 live | `server/session.c:308-313` |
| robust mutex | `PTHREAD_PROCESS_SHARED \| PTHREAD_MUTEX_ROBUST` 的 mutex；持有者死亡時下一個加鎖者收到 `EOWNERDEAD`，`shm_lock` 修復 ring 後繼續。benchd 不建立任何 thread，這只是行程間的鎖 | `server/robust.c` |
| R1-R12 | 十二個可用 `--unsafe=<name>` 關掉的防護，用來示範失效後果；`enum bench_robust` 定義於 `include/bench_util.h:42`，旗標檢查是 `UNSAFE(R_xxx)` | 見下表 |

R1-R12 對照（名稱依 `lib/util.c:249` 的 `robust_names`，示範腳本為 `scripts/demo_rN.sh`）：

| 編號 | `--unsafe=` 名稱 | 防護內容 | 主要實作位置 |
|---|---|---|---|
| R1 | `zombie` | SIGCHLD handler 只設旗標，主迴圈 `waitpid` 收屍 | `server/main.c` `on_chld`、`reap_children`、`main` |
| R2 | `sigpipe` | 忽略 SIGPIPE，寫入已關閉連線時得到 `EPIPE` 而不被殺死 | `server/main.c` `main`（約 :340）、`reject_busy` |
| R3 | `validate` | 驗證 magic、長度、CRC、型別，不合法回 `ERROR` 並乾淨關閉 | `lib/proto.c` `bn_recv`、`server/session.c` `violation`、`handle_packet` |
| R4 | `readn` | 讀滿所需位元組，封包被拆成小片段也能重組 | `lib/net.c` `readn`、`lib/proto.c` `bn_recv` |
| R5 | `timeout` | 握手逾時、閒置逾時、keepalive、送出逾時，擋住只連線不講話的 socket | `server/session.c` `handshake`、`session_main`、`lib/net.c` `set_keepalive`、`set_io_timeouts` |
| R6 | `ctrl-release` | child 斷線或崩潰時立即釋放控制權 | `server/control.c`、`server/main.c` `reap_children`、`server/session.c` `finish` |
| R7 | `robust-mutex` | 持鎖行程死亡時以 `EOWNERDEAD` 復原，不凍結整台伺服器 | `server/robust.c` `shm_lock`、`shm_mutex_init` |
| R8 | `dev-reopen` | 裝置消失時標記 `DEVICE_LOST`，之後週期性重開並送出 `DEVICE_BACK` | `server/reader.c` `dev_lost`、`dev_back`、`reader_main` |
| R9 | `max-clients` | 超過 `--max-clients` 時回 `SERVER_BUSY`，不再 fork | `server/main.c` `handle_accept`、`reject_busy` |
| R10 | `overrun` | child 落後被 ring 超車時送 `DROPPED`，並驗證記錄鏈，不把殘留位元組當標頭 | `server/session.c` `pump`、`server/ring.c` `ring_copy`、`ring_seek` |
| R11 | `term-restore` | `bench` 結束或被訊號打斷時還原終端機 | `client/term.c` `term_restore`、`on_fatal` |
| R12 | `reconnect` | `bench` 連線中斷後自動重連並從最後 offset 續傳 | `client/session.c` `app_disconnect`、`try_connect` |

---

## `include/bench_proto.h`

這個標頭檔定義 benchd 的線上協定（wire protocol，規格 §6）：封包標頭的常數、封包型別／錯誤碼／事件碼三組列舉、解碼後的標頭結構 `struct bn_hdr`，以及所有 payload 的 packed 結構。它被每一個可執行檔共用：master（`server/main.c` 的 `reject_busy` 直接讀 HELLO 的位元組並送 `BN_ERROR`）、每個 client 的 child（`server/session.c` 幾乎全部的封包處理）、reader（只用到 `BN_NAME_LEN`、`BN_MAX_INPUT`，不收發封包）、互動式 `bench` 與子命令（`client/session.c`、`client/subcmd.c`）、攻擊測試工具 `tools/bench-evil.c`。Wireshark dissector `tools/benchd.lua` 沒有 include 這個檔，而是把相同的數字（型別、錯誤碼、事件碼、欄位位移 0/32/36/40/48 等）手抄一份，所以改動這裡的欄位順序或編號時 Lua 檔必須同步修改。

標頭依賴 `<stddef.h>`、`<stdint.h>` 與 `<endian.h>`；include `<endian.h>` 的實際作用是讓所有 include 本檔的程式都能直接使用 `htobe16/32/64`、`be16/32/64toh`（`server/session.c`、`client/session.c` 都沒有自己 include `<endian.h>`）。實作在 `lib/proto.c` 與 `lib/crc32.c`，編進 `libbench.so.1`。

封包格式（所有多位元組欄位都是 network byte order，即 big-endian）：

```
  0       2     3      4               8              12
  +-------+-----+------+---------------+---------------+------------
  | magic | ver | type |      len      |     crc32     | payload ...
  | 'BN'  |  1  |      | payload bytes | of payload    |
  +-------+-----+------+---------------+---------------+------------
```

DATA / EVENT 帶的 `offset` 是「這筆 ring record 結束處的邏輯 ring offset」（下一筆 record 的起點，見 docs/DECISIONS.md「Protocol」）。這個 offset 的單位是 ring 內的位元組，包含 `struct ring_rec` 標頭（16 bytes）與對齊到 8 的 padding（`REC_SIZE`，`server/server.h:38`），不是「UART 收到幾個 byte」。client 只要記住最後看到的 offset，斷線重連時就能用 `bn_hello.resume_offset` 接續。

本檔宣告了下列函式，定義都在 `lib/`，各函式的設計請見該檔的說明；這裡只列出標頭中寫明的契約：

| 宣告（`include/bench_proto.h`） | 實作 | 標頭註解中的契約 |
|---|---|---|
| `bench_crc32`（129） | `lib/crc32.c:21` | CRC32，IEEE 802.3、reflected 多項式 0xEDB88320（與 zlib `crc32()` 相同） |
| `bn_hdr_encode`（132） | `lib/proto.c:18` | 把 magic/ver/type/len/crc 以 big-endian 寫成 12 bytes |
| `bn_hdr_decode`（133） | `lib/proto.c:29` | 反向，結果放進 host order 的 `struct bn_hdr` |
| `bn_send`（136） | `lib/proto.c:46` | 標頭加 payload 用單一 `write()` 送出；成功 0，失敗 -1（errno） |
| `bn_send_error`（137） | `lib/proto.c:60` | printf 風格送 `BN_ERROR`（帶 `__attribute__((format(printf, 3, 4)))` 讓編譯器檢查格式） |
| `bn_recv`（145） | `lib/proto.c:77` | 回傳 1 成功、0 對端正常結束、-1 I/O 錯誤（errno）、-2 協定錯誤（`*perr` 為 `BN_ERR_BAD_MAGIC/VERSION/LENGTH/CRC`）；標頭或 payload 讀到一半就 EOF 也回 0；受 `--unsafe=readn`、`--unsafe=validate` 影響 |
| `bn_type_name`（147）、`bn_err_name`（148）、`bn_evt_name`（149） | `lib/proto.c:139`、`:164`、`:173` | 數值轉名稱字串，超出範圍回 `"?"` |

### `BN_MAGIC`
*位置：`include/bench_proto.h:22`　簽章：`#define BN_MAGIC 0x424E`*

- **用途**：每個封包前兩個 byte 的識別碼，`0x424E` 即 ASCII 的 `'B' 'N'`。
- **設計**：讓接收端能立刻分辨「這條 TCP 連線上講的不是 benchd 協定」（例如有人用瀏覽器或 `nc` 亂連），也讓封包邊界錯位時能被偵測。16 位元的 magic 以 `htons()` 寫出，所以線上第一個 byte 是 `0x42`（`'B'`）。
- **細節**：
  - `bn_hdr_encode` 寫入 `htons(BN_MAGIC)`（`lib/proto.c:20`）；`bn_recv` 比對解碼後的 `h->magic`，不符就設 `*perr = BN_ERR_BAD_MAGIC` 並回傳 -2（`lib/proto.c:102`）。
  - `--unsafe=validate`（R3）時這個檢查被整段跳過。
- **關係**：被誰使用 → `lib/proto.c`（`bn_hdr_encode`、`bn_recv`）；`tools/benchd.lua` 以 `0x424E` 判斷是否為 benchd 封包。

### `BN_VERSION`
*位置：`include/bench_proto.h:23`　簽章：`#define BN_VERSION 1`*

- **用途**：協定版本，寫在標頭第 3 個 byte，也放在 `bn_welcome.proto_ver`。
- **設計**：雙方的 `bn_recv` 都要求版本完全相等，未來若更動封包格式只需遞增這個數字，舊版對端會得到 `BN_ERR_BAD_VERSION` 而不是把新格式當舊格式亂解。
- **細節**：
  - `bn_recv` 檢查 `h->ver != BN_VERSION` → `BN_ERR_BAD_VERSION`（`lib/proto.c:106`），受 R3 開關影響。
  - 注意它與 `BENCH_VERSION`（軟體版本字串）是兩回事。server 填 `w.proto_ver = BN_VERSION`（`server/session.c:253`）；client 沒有讀取 `proto_ver`（`client/session.c` 的 `handle_welcome` 不使用），真正擋版本不符的是 `bn_recv` 的標頭檢查。
- **關係**：被誰使用 → `lib/proto.c`、`server/session.c`。

### `BN_HDR_LEN`
*位置：`include/bench_proto.h:24`　簽章：`#define BN_HDR_LEN 12`*

- **用途**：封包標頭固定長度：2（magic）+ 1（ver）+ 1（type）+ 4（len）+ 4（crc32）。
- **設計**：標頭長度固定，接收端先 `readn` 精確 12 bytes 就能得知後面還有多少 payload，不需要分隔符或掃描；length-prefix 的做法讓 TCP 分段、黏包都可以用「讀滿 n bytes」解決（R4）。
- **細節**：
  - 用來宣告 `bn_hdr_encode`／`bn_hdr_decode` 的陣列參數、`bn_recv` 內的 `raw[BN_HDR_LEN]`、`bn_send` 用的靜態 `txbuf[BN_HDR_LEN + BN_MAX_PAYLOAD]`。
  - `server/main.c` 的 `reject_busy` 在伺服器已滿時，用 `BN_HDR_LEN` 當偏移直接從 `recv()` 到的位元組取出 HELLO 的 `name`（`memcpy(name, tmp + BN_HDR_LEN, BN_NAME_LEN)`，`server/main.c:122`）。這依賴「`name` 是 `bn_hello` 的第一個欄位」，改動 `bn_hello` 欄位順序前要先看這裡。
- **關係**：被誰使用 → `lib/proto.c`、`server/main.c`、`tools/bench-evil.c`（`:53`、`:166`）。

### `BN_MAX_PAYLOAD`
*位置：`include/bench_proto.h:25`　簽章：`#define BN_MAX_PAYLOAD 65536`*

- **用途**：單一封包 payload 的上限（64 KiB）。
- **設計**：有上限，接收端才能在讀 payload 之前就拒絕一個謊報超大 `len` 的標頭，不會被誘導去等或讀一個無限大的封包；發送端用一個固定大小的靜態緩衝區就能把標頭與 payload 併成一次 `write()`（見 DECISIONS：`bn_send` 單次 `write()`）。
- **細節**：
  - `bn_send` 在 `len > BN_MAX_PAYLOAD` 時回 -1、`errno = EMSGSIZE`（`lib/proto.c:48`）。
  - `bn_recv` 在 `h->len > BN_MAX_PAYLOAD || h->len > cap` 時回 `BN_ERR_BAD_LENGTH`（`lib/proto.c:110`）。所以實際上限是兩者的較小值：server 的 child 傳入的 `cap` 只有 4096（`server/session.c` 的 `char buf[4096]`），client 傳入的是 `BN_MAX_PAYLOAD` 大小的緩衝區（`client/session.c:app_run`、`conn_wait_welcome`）。
  - `--unsafe=validate`（R3）會同時略過這兩個長度檢查，1 MiB 的 `len` 就會寫爆 server 的 4 KiB 堆疊緩衝區（DECISIONS「R3」）。
  - 實際的 DATA/EVENT 遠小於此：ring 的單筆 record 資料最多 `REC_MAX_DATA` = 1024 bytes（`server/server.h:27`、`server/ring.c:51`）。
- **關係**：被誰使用 → `lib/proto.c`（`bn_send`、`bn_recv`）、`client/session.c:95`、`:334`、`client/subcmd.c:20`、`tools/bench-evil.c:39` 的接收緩衝區。

### `BN_DEFAULT_PORT`
*位置：`include/bench_proto.h:26`　簽章：`#define BN_DEFAULT_PORT 9000`*

- **用途**：benchd 預設監聽、`bench` 預設連線的 TCP 埠。
- **設計**：server 與 client 共用同一個常數，避免兩邊預設值不一致。
- **細節**：server 以它初始化 `g_cfg.port`（`server/main.c:283`，可用 `-p` 覆寫）；client 與 `bench-evil` 在 `parse_hostport` 時當作 `defport`（`client/main.c:88`、`tools/bench-evil.c:472`），所以 `bench localhost` 等於 `localhost:9000`。
- **關係**：被誰使用 → `server/main.c`、`client/main.c`、`tools/bench-evil.c`。

### `BN_MAX_INPUT`
*位置：`include/bench_proto.h:27`　簽章：`#define BN_MAX_INPUT 256`*

- **用途**：一個 `BN_INPUT` 封包最多可帶幾個要打進 UART 的位元組。
- **設計**：這個數字同時決定「child → reader」輸入 pipe 訊息的大小（`struct pipe_msg` 的 `data[BN_MAX_INPUT]`，`server/server.h:88`；整個訊息 8 + 256 = 264 bytes，遠小於 `PIPE_BUF`），pipe 對不超過 `PIPE_BUF` 的 `write()` 保證原子，多個 child 同時寫同一條 pipe 也不會互相交錯。限制輸入大小也避免單一 client 一次塞滿 UART。
- **細節**：
  - child 收到 `len > BN_MAX_INPUT` 的 INPUT：預設視為 `BN_ERR_BAD_LENGTH` 的協定違規並斷線（`server/session.c:342`）；`--unsafe=validate` 時改為悄悄截成 256 bytes。
  - `reader` 讀 pipe 訊息時也再檢查 `m.len <= BN_MAX_INPUT`（`server/reader.c:264`）。
  - `bench send` 把長字串切成每塊 256 bytes 分封包送（`client/subcmd.c:282`）。
- **關係**：被誰使用 → `server/session.c`、`server/server.h`、`server/reader.c`、`client/subcmd.c`。

### `BN_NAME_LEN`
*位置：`include/bench_proto.h:28`　簽章：`#define BN_NAME_LEN 32`*

- **用途**：client 暱稱緩衝區大小，也是 `bn_hello.name` 的線上長度。
- **設計**：固定長度欄位讓 `struct bn_hello` 大小固定、能直接 `memcpy`；同一個常數也用在 server 的共享記憶體（`bench_slot.name`、`bench_shm.ctrl_name`）與 client 的 `struct app`，所有暱稱緩衝區等長，`bench_strlcpy` 不會截到不一致的長度。
- **細節**：
  - 線上的 `name` 不保證有 NUL 結尾（對端可以填滿 32 個非 NUL 字元）。接收端必須先做 `sanitize_name(name, BN_NAME_LEN)`，它會強制 `name[31] = 0`、把不可印字元換成 `?`、空字串改成 `"anon"`（`server/session.c:229`、`server/main.c:123`）。因此實際可用暱稱最長 31 字元。
  - `bn_welcome.ctrl_name`、`bn_ctrl_result.holder`、`bn_status.ctrl_name` 在結構中寫的是字面值 `32`，數值與此相同。
- **關係**：被誰使用 → `server/session.c`、`server/main.c`、`server/server.h`、`server/reader.c`、`client/client.h`、`tools/bench-evil.c`。

### `BENCH_VERSION`
*位置：`include/bench_proto.h:29`　簽章：`#define BENCH_VERSION "1.0.0"`*

- **用途**：軟體發行版本字串（給人看），與線上協定版本 `BN_VERSION` 分開。
- **設計**：協定版本只在封包格式不相容時才改，發行版本則可隨功能更新，所以拆成兩個常數。
- **細節**：出現在 WELCOME 的 `server_version[16]`（`server/session.c:263`，以 `bench_strlcpy` 複製，所以超過 15 字元會被截斷）、INFO 的 `server.version=`（`server/sysinfo.c:151`）、server 啟動訊息（`server/main.c:402`）與 client 啟動 log（`client/main.c:121`）。
- **關係**：被誰使用 → `server/session.c`、`server/sysinfo.c`、`server/main.c`、`client/main.c`。

### `enum bn_type`
*位置：`include/bench_proto.h:31`　簽章：`enum bn_type { BN_HELLO = 0x01, ..., BN_PONG = 0x8A }`*

- **用途**：標頭 `type` 欄位（1 byte）的所有封包型別。
- **設計**：client → server 用 `0x01`–`0x08`，server → client 用 `0x81`–`0x8A`，高位元表示方向。這只是編號慣例：程式不測試這個位元（server 的 `handle_packet` 用 `switch` 比對確切值，收到 `0x81` 以上的型別會落入 `default` 而被當作 `BN_ERR_BAD_TYPE`），好處是 hex dump 與 Wireshark 裡一眼能看出封包方向。
- **細節**：各型別的 payload 與處理方式（行號為 `server/session.c`）：

| 型別 | 值 | payload | 處理／來源 |
|---|---|---|---|
| `BN_HELLO` | 0x01 | `struct bn_hello`（至少 56 bytes） | 連線後第一個封包，`handshake()` 處理；不是 HELLO 或太短 → `BAD_TYPE`／`BAD_LENGTH` |
| `BN_INPUT` | 0x02 | 原始位元組，≤ `BN_MAX_INPUT` | 只有控制者能送；經輸入 pipe 轉給 reader 寫進 UART（`:341`） |
| `BN_TAKE` | 0x03 | 無（payload 被忽略） | 搶控制權，回 `BN_CTRL_RESULT`（`:361`） |
| `BN_RELEASE` | 0x04 | 無 | 若是控制者就經 pipe 通知 reader 釋放；沒有直接回應（結果以 `EV_CTRL_RELEASED` 事件與 STATUS 呈現）（`:372`） |
| `BN_INFO_REQ` | 0x05 | 無 | 回 `BN_INFO`（`sysinfo_build` 產生的 `key=value` 文字）（`:380`） |
| `BN_BAUD` | 0x06 | 4 bytes big-endian 的 baud | 只有控制者能改；不支援的值 → `BAD_REQUEST`（`:385`） |
| `BN_PING` | 0x07 | `struct bn_ping` | 回 `BN_PONG`（`:406`） |
| `BN_BYE` | 0x08 | 無 | child 正常結束（`finish(s, "bye")`，`:413`） |
| `BN_WELCOME` | 0x81 | `struct bn_welcome` | 握手完成後回覆 HELLO（`:287`） |
| `BN_DATA` | 0x82 | `struct bn_data_hdr` + 原始 UART 位元組 | 由 ring record 轉出（`send_record`） |
| `BN_EVENT` | 0x83 | `struct bn_event_hdr` + UTF-8 文字 | 由 ring record 或 `send_dropped` 產生 |
| `BN_REPLAY_BEGIN` | 0x84 | `struct bn_replay_begin` | 非續傳的新加入者，在歷史重播前送出 |
| `BN_REPLAY_END` | 0x85 | `struct bn_replay_end` | 歷史重播結束，之後是即時串流 |
| `BN_CTRL_RESULT` | 0x86 | `struct bn_ctrl_result` | `BN_TAKE` 的回應 |
| `BN_INFO` | 0x87 | 純文字 | `BN_INFO_REQ` 的回應 |
| `BN_STATUS` | 0x88 | `struct bn_status` | child 在串流迴圈中主動推送 |
| `BN_ERROR` | 0x89 | `struct bn_error_hdr` + 文字 | 錯誤通知，見 `enum bn_err` |
| `BN_PONG` | 0x8A | `struct bn_pong` | `BN_PING` 的回應 |

  - 收到未知型別時，server 預設送 `BAD_TYPE` 後斷線，`--unsafe=validate` 下只記一行 DEBUG 並忽略（`server/session.c:415`）；client 對未知型別一律只記 DEBUG 並忽略（`client/session.c` 的 `handle_packet` default 分支）。
  - `bn_type_name()` 依本列舉回傳名稱，供 TRACE log（`tx/rx pkt type=...`）使用。
- **關係**：被誰使用 → `lib/proto.c`（`bn_send`、`bn_type_name`）、`server/session.c`（`handle_packet` 的 `switch`）、`client/session.c`（`handle_packet`）、`tools/bench-evil.c`；`tools/benchd.lua` 的 `types` 表是同一組數字的複本。

### `enum bn_err`
*位置：`include/bench_proto.h:41`　簽章：`enum bn_err { BN_ERR_BAD_MAGIC = 1, ..., BN_ERR_BAD_REQUEST = 12 }`*

- **用途**：`BN_ERROR` 封包 `bn_error_hdr.code` 的錯誤碼，也是 `bn_recv` 透過 `*perr` 回報協定錯誤的代碼。
- **設計**：從 1 開始編號，0 保留為「沒有錯誤」（`bn_err_name(0)` 回 `"OK"`，`bn_recv` 一開始就把 `*perr` 設成 0）。依嚴重度分兩類：協定違規（1–5，以及 INPUT/BAUD 長度錯誤的 3）會讓 server 送 ERROR 後斷線；其餘（6、7、12）只是拒絕這個請求，連線保持開啟。第 12 項 `BAD_REQUEST` 是本實作加的延伸，註解寫明 "request refused, connection stays open"（DECISIONS「Protocol」）。
- **細節**：

| 碼 | 名稱 | 誰在什麼時候送出 | 之後 |
|---|---|---|---|
| 1 | `BAD_MAGIC` | `bn_recv` 回報；`violation()`（`server/session.c:45`）送出 | 斷線 |
| 2 | `BAD_VERSION` | 同上 | 斷線 |
| 3 | `BAD_LENGTH` | `bn_recv`（`len` 超過上限或超過 `cap`）；HELLO 小於 56 bytes；INPUT 超過 256；BAUD 長度不是 4 | 斷線 |
| 4 | `BAD_CRC` | `bn_recv`（payload 的 CRC32 不符） | 斷線 |
| 5 | `BAD_TYPE` | 第一個封包不是 HELLO（`:224`）；握手後出現未知型別（`:417`） | 斷線 |
| 6 | `NOT_CONTROLLER` | 非控制者送 INPUT（`:354`）或 BAUD（`:400`） | 連線保持 |
| 7 | `DEVICE_LOST` | 控制者在裝置斷線時送 INPUT（`:356`），輸入被丟棄 | 連線保持 |
| 8 | `SERVER_BUSY` | 已達 `--max-clients`（`server/main.c:128`），或 `fork()` 失敗（`:178`） | master 關閉該 socket |
| 9 | `HANDSHAKE_TIMEOUT` | 5 s 內沒收到完整 HELLO（`server/session.c:206`、`:216`） | 斷線 |
| 10 | `IDLE_TIMEOUT` | 60 s 沒收到任何封包（`:331`、`:463`） | 斷線 |
| 11 | `SERVER_SHUTDOWN` | 已定義，`bn_err_name` 與 Lua 表也有，但目前沒有任何程式碼用 `bn_send_error` 送出；關機是用 `EV_SERVER_SHUTDOWN` 事件通知 | 無 |
| 12 | `BAD_REQUEST` | BAUD 要求不支援的鮑率（`:402`） | 連線保持 |

  - 1–4 之外的錯誤碼不會出現在 `*perr`。
  - 受 R3（validate）影響：關閉後 1–5 的檢查不做，後果是 child 崩潰而非回 ERROR（`demo_r3.sh`）；受 R5（timeout）影響：關閉後不會有 9、10。
  - `bn_err_name(code)` 用陣列以編號索引（`lib/proto.c:164`），新增錯誤碼時要同步補名稱陣列。
- **關係**：被誰使用 → `lib/proto.c`（`bn_recv`、`bn_err_name`）、`server/session.c`、`server/main.c`、`client/session.c`（以 `BN_ERR_NOT_CONTROLLER` 決定用警告色或錯誤色顯示）；`tools/benchd.lua` 的 `errors` 表。

### `enum bn_evt`
*位置：`include/bench_proto.h:48`　簽章：`enum bn_evt { EV_DEVICE_LOST = 1, ..., EV_SERVER_SHUTDOWN = 7 }`*

- **用途**：`BN_EVENT` 封包 `bn_event_hdr.code` 的事件碼，同時也是 ring record（`struct ring_rec.code`）中事件的類別。
- **設計**：事件與 UART 資料放在同一個 ring、用同一個 offset 軸，所有 viewer 看到的資料與事件順序一致，連歷史重播也一樣（README「Architecture」）。事件本身的人類可讀說明放在後面的 UTF-8 文字，`code` 讓 client 可以做程式邏輯（例如更新裝置狀態、顯示不同樣式）而不必解析文字。
- **細節**：

| 碼 | 名稱 | 產生處 | 文字範例 |
|---|---|---|---|
| 1 | `EV_DEVICE_LOST` | `server/reader.c:100`（reader） | `device lost` |
| 2 | `EV_DEVICE_BACK` | `server/reader.c:111` | `device reconnected` |
| 3 | `EV_CTRL_TAKEN` | `server/control.c:58`（`ctrl_take_locked`） | `control taken by <name>` |
| 4 | `EV_CTRL_RELEASED` | `server/control.c:46`、`:75` | `control released by <name> (<reason>)` |
| 5 | `EV_BAUD_CHANGED` | `server/reader.c:180` | `baud <old> -> <new> by <name>` |
| 6 | `EV_DROPPED` | `server/session.c:100`（`send_dropped`） | `dropped <size>` |
| 7 | `EV_SERVER_SHUTDOWN` | `server/main.c:218` | `server shutdown` |

  - 1–5、7 是透過 `ring_event()` 寫進 ring，因此所有 client 都會收到並且有相同順序。6 是特例：它不進 ring，而是某個 child 發現自己的讀取位置被覆蓋（落後超過 ring 容量）時，只對那一個 client 直接送出，`offset` 填新的讀取位置、時間戳用當下的 `now_ns()`。
  - client 收到 1／2 會更新自己的 `dev_state`（`client/session.c:265`）；`EV_DROPPED` 由 `client/ui.c:262`（互動式）與 `client/subcmd.c:221` 特別處理。
  - 沒有 code 0；`bn_evt_name(0)` 回 `"?"`（`lib/proto.c:175` 的陣列第 0 項）。
- **關係**：被誰使用 → `server/reader.c`、`server/control.c`、`server/main.c`、`server/session.c`、`client/session.c`、`client/ui.c`、`client/subcmd.c`、`lib/proto.c`（`bn_evt_name`）；`tools/benchd.lua` 的 `events` 表。

### `enum { DEV_LOST, DEV_UP }`
*位置：`include/bench_proto.h:53`　簽章：`enum { DEV_LOST = 0, DEV_UP = 1 };`*

- **用途**：UART 裝置目前是否可用。
- **設計**：值同時用在共享記憶體 `bench_shm.dev_state`（`int`）、`bn_welcome.dev_state` 與 `bn_status.dev_state`（`uint8_t`）、client 的 `app.dev_state`，因此是匿名列舉而非 C++ 式的強型別；數值 0 代表「失去」，使全零初始化的狀態偏向保守（不允許輸入）。
- **細節**：
  - master 在建立共享記憶體時先設 `DEV_LOST`（`server/main.c:352`），reader 開啟裝置成功後設 `DEV_UP`（`server/reader.c:109`、`:226`），讀到 EOF／EIO 時由 `dev_lost()` 設回 `DEV_LOST`（`:99`）；reader 啟動時打不開裝置也設 `DEV_LOST`（`:222`）。
  - `INPUT` 在 `dev_state != DEV_UP` 時回 `BN_ERR_DEVICE_LOST`（`server/session.c:349`–`356`）。
  - client 在尚未收到 WELCOME 前預設 `DEV_UP`（`client/session.c:338`），收到 WELCOME／STATUS／事件後更新；狀態列在非 `DEV_UP` 時顯示裝置斷線（`client/ui.c:409`）。
  - `server/sysinfo.c:158` 把它印成 `dev.state=UP|LOST`。
- **關係**：被誰使用 → `server/reader.c`、`server/main.c`、`server/session.c`、`server/sysinfo.c`、`client/session.c`、`client/ui.c`。

### `struct bn_hdr`
*位置：`include/bench_proto.h:56`　簽章：`struct bn_hdr { uint16_t magic; uint8_t ver; uint8_t type; uint32_t len; uint32_t crc32; }`*

- **用途**：解碼後（host byte order）的封包標頭，是 `bn_recv` 回傳給呼叫者的「這個封包是什麼」。
- **設計**：它**不是** packed，也不直接對應線上位元組。編譯器在這個結構上剛好不插 padding（偏移 0/2/3/4/8、總長 12，與線上排列一致），但程式從不依賴這個巧合：編碼與解碼一律由 `bn_hdr_encode`／`bn_hdr_decode` 用 `memcpy` + `htons/htonl`／`ntohs/ntohl` 逐欄處理，所以不受 ABI 與 endian 影響。接收端拿到的是已轉成本機位元組序、可以直接比較的數字。
- **細節**：
  - 欄位：`magic`（應為 `BN_MAGIC`）、`ver`（應為 `BN_VERSION`）、`type`（`enum bn_type` 的值）、`len`（payload 位元組數，不含標頭）、`crc32`（payload 的 CRC32；`len == 0` 時為 0，見 `bn_send`）。
  - `bn_recv` 驗證完成後，呼叫者用 `h.type` 與 `h.len` 分派（`server/session.c:340` 的 `switch (h.type)`）。
  - `--unsafe=validate` 下 `h.len` 未經檢查就被用來決定讀多少 bytes。
- **關係**：被誰使用 → `lib/proto.c`（`bn_hdr_decode`、`bn_recv`）、`server/session.c`（`handshake`、`handle_packet`）、`client/session.c`（`conn_recv`、`handle_packet`）、`client/subcmd.c`。

### `BN_PACKED`
*位置：`include/bench_proto.h:65`　簽章：`#define BN_PACKED __attribute__((packed))`*

- **用途**：讓下面所有 payload 結構沒有編譯器自動插入的 padding，且對齊要求為 1。
- **設計**：線上格式必須由協定定義，不能隨編譯器／架構而變，所以用 packed 保證 `sizeof` 與欄位位移固定。對齊為 1 還有另一個好處：可以安全地把 `uint8_t` 緩衝區（例如 `server/session.c` 的靜態 `sendbuf`）強制轉型成這些結構的指標去填欄位，GCC 對 packed 成員會自動產生不要求對齊的存取，不會在嚴格對齊的架構上出問題。
- **細節**：
  - 各結構仍然保留明確的 `pad` 欄位，使每個多位元組欄位在線上也落在自然對齊的位置，並讓 Lua dissector 的位移容易計算。實測各結構大小（用 `gcc` 編譯 `sizeof` 驗證）：`bn_hello` 56、`bn_welcome` 208、`bn_data_hdr` 16、`bn_event_hdr` 20、`bn_replay_begin` 16、`bn_replay_end` 8、`bn_ctrl_result` 44、`bn_status` 56、`bn_error_hdr` 4、`bn_ping` 8、`bn_pong` 16。
  - 結構內的整數**存放的就是線上的 big-endian 值**：寫入時要先 `htobe16/32/64`，讀取時要 `be16/32/64toh`。型別系統不會提醒是否已轉換。單 byte 欄位（`dev_state`、`granted`…）與字元陣列不需轉換。
  - 接收端的慣例是先 `memcpy` 到區域變數再轉換（`server/session.c:227`、`client/session.c` 各 `case`），避免對 packed 欄位取址。
  - 屬性是用巨集而不是每個結構重寫，語法上要放在 `struct` 與標籤之間：`struct BN_PACKED bn_hello { ... }`。
- **關係**：被誰使用 → 本檔所有 `bn_*` payload 結構。

### `struct bn_hello`
*位置：`include/bench_proto.h:67`　簽章：`struct BN_PACKED bn_hello { char name[32]; uint32_t replay_bytes; uint8_t has_resume; uint8_t pad[3]; uint64_t resume_offset; uint64_t last_server_start_ns; }`*

- **用途**：client 連上後送的第一個封包（`BN_HELLO`）的 payload，共 56 bytes：告訴 server「我是誰、要多少歷史、是否要從上次中斷處續傳」。
- **設計**：
  - `name[32]`（位移 0）：顯示在控制權訊息與 server log 裡的暱稱，固定放在第一個欄位，使 master 在伺服器已滿時不必解析整個 HELLO 就能偷看名字（`reject_busy`）。server 收到後一定要 `sanitize_name`，因為對端不保證 NUL 結尾。
  - `replay_bytes`（位移 32）：新加入且不續傳時，希望從目前 `head` 往回重播多少。client 預設 8K（`client/main.c:43`，`-n` 可改）。server 計算 `want = head - replay_bytes`；若比 `tail` 更舊，就從 `tail` 開始並在 `REPLAY_BEGIN.dropped_bytes` 報告缺了多少（`server/session.c:275`–`280`）。
  - `has_resume`（位移 36）、`pad[3]`、`resume_offset`（位移 40）、`last_server_start_ns`（位移 48）：斷線重連續傳用。`pad[3]` 是保留欄位，client 填 0、server 不檢查，作用是讓兩個 64 位元欄位落在 8 的倍數位移。`resume_offset` 是 client 最後看到的 DATA/EVENT offset（或 `REPLAY_END.head`）；`last_server_start_ns` 是 client 上次 WELCOME 帶回的 `server_start_ns`。ring 的 offset 在 server 重啟後會從 0 重新開始，單憑 offset 無法判斷是不是同一個 server，所以要多帶一個「server 啟動時間」當世代編號。
  - server 只有在 `has_resume && last_server_start_ns == g_shm->server_start_ns && resume_offset <= head` 時才續傳；續傳不送 `REPLAY_BEGIN/END`，若 `resume_offset < tail`（來不及了）就從 `tail` 開始並送 `EV_DROPPED` 事件（`server/session.c:266`–`273`、`:303`–`305`）。其他情況走一般歷史重播。
- **細節**：
  - 欄位都是 big-endian（`replay_bytes` 用 `htobe32`，64 位元用 `htobe64`，`client/session.c:37`–`41`）。client 只有在 `have_pos && have_start`（曾經收到過 offset 和 server 啟動時間）時才設 `has_resume = 1`。
  - server 要求 `h.len >= sizeof(struct bn_hello)`，超過的部分被忽略；不足視為 `BAD_LENGTH`（`--unsafe=validate` 下不擋）。
  - `tools/bench-evil.c:make_hello` 用它建構惡意／慢速的 HELLO。
- **關係**：被誰使用 → 寫入：`client/session.c:conn_connect`、`tools/bench-evil.c`；讀取：`server/session.c:handshake`、`server/main.c:reject_busy`；`tools/benchd.lua` 以位移 0/32/36/40/48 解析，且要求 `len >= 56`。

### `struct bn_welcome`
*位置：`include/bench_proto.h:76`　簽章：`struct BN_PACKED bn_welcome { uint8_t proto_ver; uint8_t dev_state; uint16_t pad; uint32_t baud, viewers, history_cap; uint64_t head, server_start_ns; char dev_path[128], ctrl_name[32], server_version[16]; }`*

- **用途**：server 對 HELLO 的回覆（`BN_WELCOME`），共 208 bytes，一次交代 server 的目前狀態。
- **設計**：欄位位移為 0（`proto_ver`）、1（`dev_state`）、2（`pad`，補到 4）、4（`baud`）、8（`viewers`）、12（`history_cap`）、16（`head`）、24（`server_start_ns`）、32（`dev_path[128]`）、160（`ctrl_name[32]`）、192（`server_version[16]`）。全部都在持有共享記憶體鎖的期間一次複製成快照（`server/session.c:243`–`263`），解鎖後才送出，所以內容彼此一致，而且網路寫入不會在鎖內進行（DECISIONS「Threads and synchronisation」）。
- **細節**：
  - `viewers` 已包含剛完成握手的這個 client（`slots_recount` 在填欄位前執行）。
  - `head` 是當下 ring 的 head offset，`server_start_ns` 是 server 啟動的 `CLOCK_REALTIME` 奈秒；client 存下後者，以便下次 HELLO 的 `last_server_start_ns` 使用，並在 server 重啟（值變了）時丟掉舊 offset 並在畫面顯示 "server restarted"（`client/session.c:handle_welcome`）。
  - `history_cap` 是 `(uint32_t)g_shm->cap` 的 big-endian 值；目前 `client/` 沒有使用，只有 Wireshark dissector 顯示它。
  - `dev_path`、`ctrl_name`、`server_version` 都是以 `bench_strlcpy` 填入並保證 NUL 結尾（長度不足時截斷）；`ctrl_name` 在沒有人持有控制權時是空字串。
  - 整個結構先 `memset` 為 0，所以 `pad` 為 0。
  - client 收到時若 `len < sizeof(struct bn_welcome)` 會忽略該封包（`client/session.c:handle_welcome`，`conn_wait_welcome` 則視為失敗）。
- **關係**：被誰使用 → 寫入：`server/session.c:handshake`；讀取：`client/session.c`（`handle_welcome`、`conn_wait_welcome`）、`client/subcmd.c`；`tools/benchd.lua` 要求 `len >= 208`。

### `struct bn_data_hdr`
*位置：`include/bench_proto.h:90`　簽章：`struct BN_PACKED bn_data_hdr { uint64_t offset; uint64_t ts_ns; }`*

- **用途**：`BN_DATA` 封包的前 16 bytes，後面緊接 UART 收到的原始位元組。
- **設計**：
  - `offset`：這筆 ring record 結束處的邏輯 offset（單位含 record 標頭與 padding，見檔案概述），不是這批資料第一個 byte 的位置。這樣 client「最後看到的 offset」就是下一筆 record 的起點，續傳時不必再算。server 端的計算是 `o += REC_SIZE(len)` 後填入（`server/session.c:159`–`161`、`:94`）。
  - `ts_ns`：reader 把資料寫進 ring 時的 `CLOCK_REALTIME` 奈秒（`ring_rec.ts_ns`），不是 client 收到的時間；所有 viewer 看到同一個時間戳，包含歷史重播的資料，`:ts` 時間戳功能用它。
  - 資料長度沒有獨立欄位，是 `h.len - 16`。
- **細節**：
  - 單一 DATA 的資料最多 1024 bytes（`REC_MAX_DATA`），所以 payload ≤ 1040 bytes。
  - client 先檢查 `h->len >= sizeof(struct bn_data_hdr)` 才解讀（`client/session.c:237`、`client/subcmd.c:146`），避免畸形封包造成長度下溢。
  - `--unsafe=overrun` 下 server 可能把 ring 中被覆蓋的垃圾當成 record 送出，那種封包仍然用這個格式。
- **關係**：被誰使用 → 寫入：`server/session.c:send_record`；讀取：`client/session.c`、`client/subcmd.c`。

### `struct bn_event_hdr`
*位置：`include/bench_proto.h:95`　簽章：`struct BN_PACKED bn_event_hdr { uint64_t offset; uint64_t ts_ns; uint16_t code; uint16_t pad; }`*

- **用途**：`BN_EVENT` 封包的前 20 bytes，後面接 UTF-8 說明文字（不含 NUL，長度為 `h.len - 20`）。
- **設計**：前兩個欄位的意義與 `bn_data_hdr` 完全相同（同一個 offset 軸），讓 client 對 DATA 與 EVENT 可以用同樣的方式更新「最後看到的 offset」。`code` 是 `enum bn_evt`，big-endian 16 位元；`pad` 填 0，使整個結構長度 20、payload 文字從 4 的倍數位移開始。
- **細節**：
  - 事件文字來自 `ring_event()` 以 256 bytes 緩衝區格式化；client 讀取時額外把文字上限限制在 299 bytes 並補 NUL（`client/session.c:handle_packet` 的 `BN_EVENT` 分支）。
  - `EV_DROPPED` 的 `offset` 是 client 被推進到的新位置（`tail`），不是某筆 record 的結尾。
- **關係**：被誰使用 → 寫入：`server/session.c`（`send_record`、`send_dropped`）；讀取：`client/session.c`、`client/subcmd.c:print_event`；`tools/benchd.lua` 要求 `len >= 20`。

### `struct bn_replay_begin`
*位置：`include/bench_proto.h:102`　簽章：`struct BN_PACKED bn_replay_begin { uint64_t from_offset; uint64_t dropped_bytes; }`*

- **用途**：`BN_REPLAY_BEGIN` 的 payload（16 bytes），表示接下來的 DATA/EVENT 是歷史重播，不是即時資料。
- **設計**：`from_offset` 是重播實際開始的 offset（已對齊到一筆完整 record，並且不早於 ring 的 `tail`）；`dropped_bytes` 是 client 要求的範圍中已經被 ring 覆蓋、拿不到的量（`tail - want`）。client 因此能顯示「歷史不完整」，並把 `from_offset` 記為目前位置。
- **細節**：
  - 只在**非續傳**的路徑送出；續傳時沒有 begin/end，若有缺漏則以 `EV_DROPPED` 事件表達（`server/session.c:303`–`315`）。
  - 兩個欄位都是 big-endian（`htobe64`）。client 收到後設 `in_replay = 1` 並更新 `c.pos`（`client/session.c:217`–`225`）。
- **關係**：被誰使用 → 寫入：`server/session.c:handshake`；讀取：`client/session.c`。

### `struct bn_replay_end`
*位置：`include/bench_proto.h:103`　簽章：`struct BN_PACKED bn_replay_end { uint64_t head; }`*

- **用途**：`BN_REPLAY_END` 的 payload（8 bytes），標示歷史重播結束、之後是即時串流。
- **設計**：欄位名稱叫 `head`，但 server 填的是這個 child 重播完後的讀取位置 `s->off`（`server/session.c:312`），也就是重播所到達的 offset，而不是此刻共享記憶體的 `head`。重播的上限是握手時取得的快照 `snap_head`，握手之後新進來的資料由後面的串流迴圈送，所以 client 看到的 offset 軸不會出現空洞或重複。
- **細節**：client 收到後清除 `in_replay`，把 `c.pos` 設為這個值，並在畫面畫出 "replay end · live" 分隔線（`client/session.c:226`–`235`）。
- **關係**：被誰使用 → 寫入：`server/session.c:handshake`；讀取：`client/session.c`。

### `struct bn_ctrl_result`
*位置：`include/bench_proto.h:105`　簽章：`struct BN_PACKED bn_ctrl_result { uint8_t granted; uint8_t pad[3]; uint32_t holder_idle_sec; uint32_t auto_release_in_sec; char holder[32]; }`*

- **用途**：`BN_TAKE` 的回應（44 bytes）：是否取得控制權，被拒絕時說明是誰持有、閒置多久、多久後會自動釋放。
- **設計**：把「為什麼被拒絕」所需的資訊一次回給 client，使它能顯示 "held by alice (idle 12s, auto-release in 108s)" 而不必再發 INFO 請求。這個結構由 `ctrl_take_locked`（`server/control.c:38`）在持有共享記憶體鎖時填寫，讓「判斷能不能拿」與「讀取持有者資訊」是同一個原子決定。
- **細節**：
  - `granted`：1 取得（包括已經是自己持有而再次 TAKE），0 拒絕。
  - `holder_idle_sec`：只在拒絕時填入；成功時為 0（結構先 `memset`）。
  - `auto_release_in_sec`：`UINT32_MAX` 表示「不會自動釋放」，條件是 `--idle-release 0`（沒有閒置釋放期限）或 `--unsafe=ctrl-release`（R6，`server/control.c:32`）。client 看到 `UINT32_MAX` 就顯示 "no auto-release"（`client/session.c`）。
  - `holder[32]`：成功時是自己的暱稱，拒絕時是現任持有者的暱稱，以 `bench_strlcpy` 填入，保證 NUL 結尾。
  - 整數欄位為 big-endian；`UINT32_MAX` 的四個 byte 全是 `0xFF`，轉換前後相同，但 server 仍然呼叫 `htobe32` 以維持一致。
  - 成功／失敗都不是用 `BN_ERROR` 回報，因為「被拒絕」是正常結果而非協定錯誤。
- **關係**：被誰使用 → 寫入：`server/control.c:ctrl_take_locked`，由 `server/session.c:handle_packet` 的 `BN_TAKE` 分支送出；讀取：`client/session.c`、`client/subcmd.c:259`（`bench send` 先 TAKE）。

### `struct bn_status`
*位置：`include/bench_proto.h:113`　簽章：`struct BN_PACKED bn_status { uint8_t dev_state; uint8_t you_are_ctrl; uint16_t pad; uint32_t baud; uint32_t viewers; uint32_t ctrl_idle_sec; uint64_t rx_bytes; char ctrl_name[32]; }`*

- **用途**：`BN_STATUS`（56 bytes），讓 client 的狀態列保持最新：裝置是否在線、自己是不是控制者、鮑率、觀看人數、控制者閒置時間、累計 UART 接收量、控制者暱稱。
- **設計**：
  - 由每個 child 在串流迴圈中主動推送（`pump(..., want_status=1)`，`server/session.c:132`、`:166`–`179`），client 不需要輪詢。`you_are_ctrl` 是「對這個收件人而言」的值，所以 STATUS 每個 client 各自填寫，不能廣播同一份。
  - 避免洪水：`rx_bytes` 與 `ctrl_idle_sec` 幾乎每次都在變，所以送出條件是「距離上次已超過 1 秒」或「忽略這兩欄後內容改變」。比較用 `memcmp`，這依賴整個結構（包含 `pad`）先 `memset` 為 0（`fill_status`，`server/session.c:117`），否則 padding 的隨機位元會讓比較永遠不相等。
  - `ctrl_name` 在沒有人持有控制權時是空字串（只在 `ctrl_pid != 0` 時才複製）。
- **細節**：client 收到後更新 `dev_state`、`is_ctrl`、`baud`、`viewers`、`ctrl_idle`、`rx_bytes`、`ctrl_name`，且若 `was_ctrl` 變成非控制者（例如被閒置釋放）就清掉尚未送出的輸入緩衝（`client/session.c:296`–`313`）。client 的 `len < sizeof` 檢查會丟棄過短的封包。
- **關係**：被誰使用 → 寫入：`server/session.c:fill_status`／`pump`；讀取：`client/session.c:handle_packet`；`tools/benchd.lua`。

### `struct bn_error_hdr`
*位置：`include/bench_proto.h:124`　簽章：`struct BN_PACKED bn_error_hdr { uint16_t code; uint16_t pad; }`*

- **用途**：`BN_ERROR` 封包的前 4 bytes，後面接人類可讀的錯誤說明文字（不含 NUL，長度 `h.len - 4`）。
- **設計**：用機器可判讀的 `code`（`enum bn_err`，big-endian）搭配自由文字，client 依 `code` 決定行為與顏色、文字直接顯示給使用者。`pad` 為 0，讓文字從 4 bytes 後開始。
- **細節**：
  - `bn_send_error` 在堆疊上組裝：`uint8_t buf[sizeof(struct bn_error_hdr) + 256]`，以 `vsnprintf` 格式化進 256 bytes，並把長度限制在 255 以內（`lib/proto.c:62`–`74`），所以整個 ERROR 封包 payload ≤ 259 bytes。
  - `bench` 互動模式用閃現訊息顯示（`code == BN_ERR_NOT_CONTROLLER` 為警告色，其餘為錯誤色）；`conn_wait_welcome` 在握手時收到 ERROR 會印出 "server error <NAME>: <text>" 並回 -2，`errno = ECONNREFUSED`。
- **關係**：被誰使用 → 寫入：`lib/proto.c:bn_send_error`；讀取：`client/session.c`、`client/subcmd.c:63`（`print_error_pkt`）。

### `struct bn_ping`
*位置：`include/bench_proto.h:125`　簽章：`struct BN_PACKED bn_ping { uint64_t client_ts_ns; }`*

- **用途**：`BN_PING` 的 payload（8 bytes）：client 發送時的 `CLOCK_REALTIME` 奈秒（big-endian）。
- **設計**：時間戳由 client 填、server 原封不動地帶回，協定因此允許 client 用自己的 `now - client_ts_ns` 算來回延遲而不必假設兩端時鐘一致（目前的 client 並沒有計算，見下）。主要作用是保持連線活著：server 收到任何封包都會更新 `last_rx_ms`，因此 PING 能避開 60 秒的 `IDLE_TIMEOUT`（R5）。
- **細節**：
  - 互動式 client 與 `bench watch` 每 15 秒送一個（`client/session.c:412`、`client/subcmd.c:183`）。
  - server 只在 `h.len >= sizeof(struct bn_ping)` 時才複製這 8 bytes（直接 `memcpy`，不做位元組序轉換，因為只是原樣回傳）；payload 太短則 `client_ts_ns` 回 0（`server/session.c:406`–`411`）。
  - 目前 client 收到 PONG 時什麼也不做（`client/session.c` 的 `case BN_PONG: break;`），但任何封包到達都會刷新 client 的 15 秒「沒有資料」看門狗（`conn_recv` 更新 `last_rx_ms`）。
- **關係**：被誰使用 → 寫入：`client/session.c`、`client/subcmd.c`；讀取：`server/session.c`。

### `struct bn_pong`
*位置：`include/bench_proto.h:126`　簽章：`struct BN_PACKED bn_pong { uint64_t client_ts_ns; uint64_t server_ts_ns; }`*

- **用途**：`BN_PONG` 的 payload（16 bytes）：回傳 client 的時間戳，並附上 server 回覆時的時間戳。
- **設計**：保留 `client_ts_ns` 讓 client 可以對應是哪一次 PING；`server_ts_ns` 是 server 回覆當下的 `htobe64(now_ns())`，目前沒有任何 client 程式碼使用它，只有 dissector 會顯示。結構以 `{ 0 }` 初始化，缺少的欄位就是 0。
- **細節**：`tools/benchd.lua` 只列出 `client_ts_ns` 與 `server_ts_ns` 兩個欄位；client 不使用其中內容。
- **關係**：被誰使用 → 寫入：`server/session.c:handle_packet` 的 `BN_PING` 分支。

## `include/bench_net.h`

這個標頭只宣告 socket 輔助函式，供 client 與 server 共用，實作全在 `lib/net.c`（編進 `libbench.so.1`）。使用者有：per-client child（`writen` 把輸入 pipe 訊息寫給 reader，`server/session.c:59`；`set_keepalive`、`set_io_timeouts`，`server/session.c:236`、`:443`、`:444`；child 自己不直接呼叫 `readn`，socket 的讀寫都經 `bn_recv`／`bn_send`）、master（`tcp_listen`，`server/main.c:378`；`sock_peer_str`，`server/main.c:150`）、reader（`readn` 讀輸入 pipe，`server/reader.c:263`、`:265`）、`bench`（`client/session.c:31`）與 `bench-evil`（`tools/bench-evil.c`）的 `tcp_connect_timeout`，以及 `lib/proto.c` 的 `bn_send`／`bn_recv`（`writen`／`readn`）。`set_nonblock` 目前只被 `lib/net.c` 的 `tcp_connect_timeout` 自己呼叫。依賴只有 `<stddef.h>`、`<stdint.h>`、`<sys/types.h>`（為了 `ssize_t`）。本檔沒有結構、列舉或巨集，所以不另列 `###` 區塊，所有宣告都整理在下表；每個函式的實作設計請見 `lib/net.c` 的說明。

| 宣告（`include/bench_net.h`） | 實作 | 契約（依標頭註解與實作驗證） |
|---|---|---|
| `ssize_t readn(int fd, void *buf, size_t n)`（13） | `lib/net.c:20` | 讀滿 n bytes；`EINTR` 重試；回傳 n，遇到 EOF 回傳較小的計數，錯誤回 -1。是 R4（readn）的核心：TCP 是位元組串流，一個封包可能被拆成任意片段，單次 `read()`／`recv()` 不保證讀滿標頭。實作用的是 `read()` 而非 `recv()`，所以同一個函式也能讀 reader 的輸入 pipe。呼叫端以「回傳值 != n」判斷 EOF 或截斷，回傳短計數時不會設 errno |
| `ssize_t writen(int fd, const void *buf, size_t n)`（14） | `lib/net.c:38` | 寫滿 n bytes；處理 partial write 與 `EINTR`；回傳 n 或 -1 |
| `int tcp_listen(uint16_t port, int backlog)`（16） | `lib/net.c:54` | 建立監聽 socket，先試 IPv6 dual-stack，失敗退回 IPv4；一定設 `SO_REUSEADDR`；失敗回 -1 並保留 errno |
| `int tcp_connect_timeout(const char *host, uint16_t port, int timeout_ms)`（17） | `lib/net.c:95` | `getaddrinfo` + 非阻塞 `connect` + `poll` 逾時；成功回已恢復阻塞模式並開啟 `TCP_NODELAY` 的 fd，失敗回 -1 |
| `int set_nonblock(int fd, int on)`（18） | `lib/net.c:86` | 開／關 `O_NONBLOCK`；成功 0，失敗 -1 |
| `int set_keepalive(int fd, int idle_s, int intvl_s, int cnt)`（19） | `lib/net.c:144` | 啟用 TCP keepalive 並設定 idle／interval／count（server 呼叫的值是 30／10／3，`server/session.c:443`）。設計理由：對端整台機器斷電或網路線被拔時不會有 FIN／RST，child 只會永遠卡在 `read`；30 s 閒置後每 10 s 探測、3 次無回應（約 60 s）核心便判定連線已死，讓 child 的 `read` 失敗並離場、釋放 slot 與控制權。只有 `SO_KEEPALIVE` 失敗才回 -1，三個 `TCP_KEEP*` 選項的回傳值沒有檢查。server 只在 `!UNSAFE(R_TIMEOUT)` 時呼叫，所以 `--unsafe=timeout` 會讓這類死連線永遠佔著 child |
| `int set_io_timeouts(int fd, int rcv_s, int snd_s)`（20） | `lib/net.c:155` | 設定 `SO_RCVTIMEO`／`SO_SNDTIMEO`；參數為 0（或負值）代表該方向不變（標頭註解 "0 = unchanged"）；任一 `setsockopt` 失敗則回傳非 0。server 先呼叫 `(5, 60)`（`server/session.c:444`），握手完成後改 `(60, 60)`（`:236`）。設計理由：`SO_RCVTIMEO` 讓卡在 `read` 的 child 在期限到時以 `EAGAIN` 返回，才有機會送出 `HANDSHAKE_TIMEOUT`／`IDLE_TIMEOUT` 後離場，這是 R5 能擋住 slowloris 式「連上不講話」的機制；`SO_SNDTIMEO` 則避免對端不收資料、送出緩衝區塞滿時 child 卡死在寫入。兩處呼叫同樣由 `!UNSAFE(R_TIMEOUT)` 包住 |
| `char *sock_peer_str(int fd, char *buf, size_t n)`（21） | `lib/net.c:169` | 把對端位址格式化成 `"1.2.3.4:5678"`；IPv4-mapped IPv6 會還原成 IPv4 文字；失敗時內容為 `"?:0"` |

## `include/bench_util.h`

放 benchd 所有可執行檔共用的小工具與 **robustness 開關表**。工具函式（時間、大小字串、跳脫字元、hexdump、名稱清理、`host:port` 解析）實作在 `lib/util.c`；開關表（`enum bench_robust`、`bench_unsafe_mask`、`UNSAFE()`）則讓同一個二進位檔可以在執行時用 `--unsafe=a,b|all` 關掉個別防護，展示「有／沒有該機制」的差別（規格 §9）。使用它的行程：master（`server/main.c` 啟動時設定遮罩）、reader、per-client child、`bench` client 以及 `lib/proto.c`。依賴 `<stddef.h>`、`<stdint.h>`、`<stdio.h>`（`hexdump` 的 `FILE *`）。

函式宣告整理如下，設計請見 `lib/util.c` 的說明：

| 宣告（`include/bench_util.h`） | 實作 | 契約 |
|---|---|---|
| `uint64_t now_ns(void)`（18） | `lib/util.c:15` | `CLOCK_REALTIME` 奈秒；用於會顯示或傳給對端的時間戳 |
| `uint64_t mono_ns(void)`（19）、`uint64_t mono_ms(void)`（20） | `lib/util.c:22`、`:29` | `CLOCK_MONOTONIC`；用於逾時與間隔計算，不受時鐘調整影響 |
| `void sleep_ms(unsigned ms)`（21） | `lib/util.c:31` | 睡滿指定毫秒，`EINTR` 時繼續睡剩餘時間 |
| `char *fmt_clock(uint64_t, char *, size_t)`（22）、`fmt_hms`（23） | `lib/util.c:38`、`:48` | 本地時間 `"20:44:13.123"`／`"20:44:13"`，回傳 `buf` |
| `int parse_size(const char *s, uint64_t *out)`（26） | `lib/util.c:57` | 解析 `64K`、`1M`、`64KiB` 等；成功 0，格式錯誤 -1 |
| `char *fmt_size(uint64_t, char *, size_t)`（27） | `lib/util.c:87` | 例如 12620 → `"12.3K"` |
| `size_t bench_strlcpy(char *dst, const char *src, size_t n)`（30） | `lib/util.c:100` | 一定 NUL 結尾（`n > 0`），回傳 `strlen(src)` 以便偵測截斷 |
| `size_t unescape(const char *in, uint8_t *out, size_t cap)`（31） | `lib/util.c:119` | 解析 `\n \r \t \\ \xHH \e \0`，供 `bench send "reset\n"` 使用 |
| `void sanitize_name(char *s, size_t n)`（32） | `lib/util.c:156` | 強制可印 ASCII、NUL 結尾、去前後空白、空字串改 `"anon"` |
| `int parse_hostport(const char *s, char *host, size_t hostsz, uint16_t *port, uint16_t defport)`（33） | `lib/util.c:177` | 支援 `host`、`host:port`、`[::1]:9000`；成功 0 |
| `void hexdump(FILE *, const void *, size_t, uint64_t)`（34）、`int hexdump_line(char *, size_t, const uint8_t *, size_t)`（35） | `lib/util.c:239`、`:216` | 每行 16 bytes 的十六進位與 ASCII 欄；`hexdump_line` 被 `client/ui.c` 的 `:hex` 模式使用 |
| `const char *bench_robust_name(int r)`（61） | `lib/util.c:254` | 編號轉開關名稱，超出範圍回 `"?"` |
| `int bench_unsafe_parse(const char *list, uint32_t *mask)`（62） | `lib/util.c:259` | 把 `"a,b"`／`"all"` 以 OR 加進 `*mask`；遇到未知名稱回 -1。呼叫端要先把 mask 設成 0（`server/main.c:280`、`client/main.c:44` 都是先設 0 再傳入）；分隔字元是逗號或空白，輸入先複製到 256 bytes 的內部緩衝區，過長會被截斷 |
| `void bench_unsafe_format(uint32_t mask, char *buf, size_t n)`（63） | `lib/util.c:285` | 反向，格式化成 `"a,b"`，全空時是 `"(none)"` |

### `enum bench_robust`
*位置：`include/bench_util.h:42`　簽章：`enum bench_robust { R_ZOMBIE = 0, R_SIGPIPE, ..., R_RECONNECT, R_COUNT }`*

- **用途**：為 12 個 robustness 機制（R1–R12）編號。這些編號就是遮罩 `bench_unsafe_mask` 的 bit 位置：`R_ZOMBIE` = bit 0，`R_RECONNECT` = bit 11。
- **設計**：用列舉而不是 12 個獨立的 `#define`，一來有型別名稱，二來 `R_COUNT` 這個哨兵值自動等於機制數量（12），讓名稱表 `robust_names[R_COUNT]`、解析迴圈與「all」遮罩 `(1u << R_COUNT) - 1`（= 0xFFF）都不用手寫數字。名稱表（`lib/util.c`）以相同順序列出字串，所以新增機制時列舉與名稱表必須一起改。編號與註解的 R1–R12 順序對應 README 的「Robustness demos」表；`bench_unsafe_parse` 也接受 `R1`…`R12`（`i + 1`）以及 `all`、`none`，大小寫不拘。
- **細節**：每個機制的名稱（`--unsafe=` 用）、哪個行程實作，以及關掉之後的行為：

| 編號 | 列舉 | 名稱 | 實作處 | 關掉後 |
|---|---|---|---|---|
| R1 | `R_ZOMBIE` | `zombie` | master：`server/main.c:392`（不安裝 SIGCHLD handler） | 結束的 child 不被 `waitpid`，變 `<defunct>` |
| R2 | `R_SIGPIPE` | `sigpipe` | `server/main.c:338`、`client/main.c:74` | 不忽略 SIGPIPE，寫入已 RST 的連線會殺死 master |
| R3 | `R_VALIDATE` | `validate` | `lib/proto.c:101`、`:128`；`server/session.c` 多處 | 不檢查 magic／ver／len／CRC／型別，長度謊報會寫爆緩衝區 |
| R4 | `R_READN` | `readn` | `lib/proto.c:86`、`:117`、`:125` | 標頭與 payload 各用一次 `recv()`，不迴圈 |
| R5 | `R_TIMEOUT` | `timeout` | `server/session.c`（HELLO 期限、`SO_RCVTIMEO`、keepalive、60 s 閒置） | 沉默的連線永遠佔著 child |
| R6 | `R_CTRL_RELEASE` | `ctrl-release` | `server/control.c`、`server/session.c:72`、`server/main.c:95` | 控制者當機後沒人能再拿控制權 |
| R7 | `R_ROBUST_MUTEX` | `robust-mutex` | `server/robust.c:33` | 持鎖者死亡後整個 server 凍結 |
| R8 | `R_DEV_REOPEN` | `dev-reopen` | `server/reader.c:252`、`:272` | 裝置斷線後 reader 不重開、空轉耗 CPU |
| R9 | `R_MAX_CLIENTS` | `max-clients` | `server/main.c:164` | 不限制連線數，一路 `fork()` 到失敗 |
| R10 | `R_OVERRUN` | `overrun` | `server/session.c:138`、`server/ring.c:118` | 落後的 client 讀到被覆蓋的 ring 內容 |
| R11 | `R_TERM_RESTORE` | `term-restore` | `client/term.c:136` | 異常結束時終端機留在 raw mode |
| R12 | `R_RECONNECT` | `reconnect` | `client/session.c:133` | 斷線後 client 直接離開 |

  - `R_COUNT` 不是機制，只是結尾哨兵；`bench_robust_name()` 與 `bench_unsafe_parse()` 都以 `< R_COUNT` 判斷有效範圍。
  - client 和 server 共用同一個列舉，所以兩邊的 `--unsafe` 都能接受全部 12 個名稱；只是各自忽略自己不使用的（DECISIONS「Robustness switches」）。
- **關係**：被誰使用 → `UNSAFE()` 的所有呼叫端（見上表）、`lib/util.c`（名稱表與解析）。

### `bench_unsafe_mask`
*位置：`include/bench_util.h:58`　簽章：`extern uint32_t bench_unsafe_mask;`（定義在 `lib/util.c:13`）*

- **用途**：目前行程哪些機制被關掉的位元遮罩，bit i 為 1 表示第 i 個機制被關閉（不安全）。
- **設計**：預設值 0（全域變數零初始化）代表所有防護都開啟，也就是「不特別指定時是安全的」；要展示不安全行為必須明確傳 `--unsafe`。在 `main()` 解析完參數後設定一次（`server/main.c:329`、`client/main.c:73`），之後只讀不寫。master 在 `fork()` 之前就設好，因此 reader 與每個 child 繼承到相同值，不需要透過共享記憶體同步。
- **細節**：
  - master 另外把同一個值存進 `g_shm->unsafe_mask`（`server/main.c:358`），純粹讓 `INFO` 回覆與 INFO 顯示（`server/sysinfo.c:112`）可以讀到，行程內判斷仍然用這個全域變數。
  - 變數定義在共享函式庫中，`UNSAFE()` 在可執行檔與 `libbench.so` 內（`lib/proto.c`）讀到的是同一個符號。
  - 因為啟動後不再改變，不需要鎖，也沒有訊號處理的重入問題。
  - `server/robust.c:print_unsafe_banner` 在遮罩非 0 時於 stderr 印出醒目的 "UNSAFE MODE" 橫幅。
- **關係**：被誰使用 → 寫入：`server/main.c`、`client/main.c`；讀取：`UNSAFE()` 與 `server/robust.c`、`server/sysinfo.c`。

### `UNSAFE`
*位置：`include/bench_util.h:59`　簽章：`#define UNSAFE(r) ((bench_unsafe_mask >> (r)) & 1u)`*

- **用途**：測試機制 `r` 是否被關閉，結果是 0 或 1（`unsigned`）。
- **設計**：把「可關閉的防護」都寫成同一種慣用語：`if (!UNSAFE(R_SIGPIPE)) signal(SIGPIPE, SIG_IGN);`（標頭註解的範例）。安全路徑與不安全路徑在同一份程式碼、同一個二進位檔內，只差一個執行期判斷，示範時不必重新編譯，也就能證明差別只來自那一個機制。以巨集而非函式是因為它只是一次位移與遮罩，且每次呼叫都能在編譯期看到常數的 `R_*`。
- **細節**：
  - 參數 `r` 只出現一次，所以即使有副作用也只計算一次；應傳入 `enum bench_robust` 的值（0–11），位移量超過 31 在 C 是未定義行為。
  - `R_VALIDATE` 的判斷在 `server/session.c` 與 `lib/proto.c` 內分散多處；`UNSAFE(R_OVERRUN)`、`UNSAFE(R_CTRL_RELEASE)` 也會在持有共享記憶體鎖的程式片段中被呼叫，這是安全的，因為只讀一個行程私有的全域變數。
- **關係**：被誰使用 → `lib/proto.c`（`bn_recv`）、`server/main.c`、`server/session.c`、`server/control.c`、`server/robust.c`、`server/reader.c`、`server/ring.c`、`client/main.c`、`client/session.c`、`client/term.c`。

## `include/bench_log.h`

兩階段除錯 log（規格 §7）的公開介面：編譯期用 `LOG_COMPILE_LEVEL` 決定哪些巨集真的產生程式碼，執行期用一個「層級指標」決定已編入的訊息是否印出。每個行程都用它：master、reader、child 在 `server/`；`bench`、`benchsim`、`bench-evil` 在 `client/` 與 `tools/`；`lib/` 內部（如 `bn_send`、`tcp_connect_timeout`）也用。實作在 `lib/log.c`；執行期層級平常指向行程私有變數，server 啟動後 `bench_log_set_level_ptr(&g_shm->log_level)` 讓它指向共享記憶體（`server/main.c:360`），於是對 master 送 `kill -USR1` 改變層級，所有行程立刻一起生效。標頭依賴 `<stdio.h>`（`BENCH_LOG_OFF` 用到 `printf` 作型別檢查）。

函式宣告如下，設計請見 `lib/log.c` 的說明：

| 宣告（`include/bench_log.h`） | 實作 | 契約 |
|---|---|---|
| `void bench_log_set_role(const char *fmt, ...)`（23） | `lib/log.c:29` | 設定每行 log 中的角色標籤（如 `master`、`child:4321`）；帶 printf 格式檢查 |
| `void bench_log_set_level_ptr(volatile int *p)`（24） | `lib/log.c:37` | 把層級指向 `p`（並把目前值複製過去）；`NULL` 退回私有層級 |
| `void bench_log_set_level(int lvl)`（25） | `lib/log.c:48` | 設定層級，夾在 `LOG_ERROR`–`LOG_TRACE` 之間 |
| `int bench_log_get_level(void)`（26）、`bench_log_compile_level(void)`（27）、`bench_log_enabled(int lvl)`（28） | `lib/log.c:57`、`:58`、`:59` | 取得執行期層級／編譯期層級；`lvl <= 目前層級` 時為真 |
| `int bench_log_level_from_str(const char *s)`（29） | `lib/log.c:66` | `"debug"`／`"3"` → 3，無法辨識回 -1 |
| `const char *bench_log_level_name(int lvl)`（30） | `lib/log.c:61` | `"ERROR"`…`"TRACE"`，超出範圍 `"?"` |
| `int bench_log_init_level(int verbose_count)`（31） | `lib/log.c:80` | 從 INFO 起算，`BENCH_LOG` 環境變數覆寫，再加 `-v` 的次數 |
| `int bench_log_open_file(const char *path)`（32） | `lib/log.c:91` | 另外把每行 log 以 append 寫入檔案；成功 0，失敗 -1。互動式 `bench` 搭配 `bench_log_set_stderr(0)` 使用（`client/main.c:119`–`120`，檔案為 `/tmp/bench-<pid>.log`） |
| `void bench_log_set_stderr(int on)`（33） | `lib/log.c:102` | 開關 stderr 輸出（互動式 `bench` 會關掉以免弄亂畫面） |
| `void bench_log_write(int lvl, const char *file, int line, const char *fmt, ...)`（34） | `lib/log.c:104` | 格式化一整行後，對每個輸出目標（stderr、log 檔）各以單次 `write()` 輸出，行與行不會在多行程間交錯；結束前還原 `errno`；帶 printf 格式檢查 |

### `enum { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE }`
*位置：`include/bench_log.h:17`　簽章：`enum { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3, LOG_TRACE = 4 };`*

- **用途**：五個 log 層級。數字越大越囉唆。
- **設計**：用遞增整數，使「是否要印」變成一個比較：`lvl <= 目前層級`（`bench_log_enabled`，`lib/log.c:59`）。`LOG_ERROR` 是 0，執行期層級最低只會被夾到 0（`bench_log_set_level`），所以錯誤訊息永遠不會被執行期關掉。`LOG_WARN` 是額外加入的一級，讓 SIGUSR1 循環（INFO → DEBUG → TRACE → ERROR → WARN → INFO）能經過每一級（DECISIONS「Logging」）。
- **細節**：
  - 註解特別說明下面的 `#if LOG_COMPILE_LEVEL >= N` 使用數字字面值，因為前置處理器看不到列舉常數，不能寫成 `>= LOG_WARN`。這些數字必須與本列舉保持一致。
  - 名稱字串陣列 `names[]`（`lib/log.c:27`）依相同順序，也是 log 行 `[INFO ]` 欄位與 `bench_log_level_from_str` 的比對來源。
  - 預設層級是 INFO（`bench_log_init_level` 與私有層級初值），`-v` 每次加一級。
- **關係**：被誰使用 → `lib/log.c`、`server/main.c:cycle_log_level`、各 `LOGx` 巨集。

### `LOG_COMPILE_LEVEL`
*位置：`include/bench_log.h:19`　簽章：`#ifndef LOG_COMPILE_LEVEL / #define LOG_COMPILE_LEVEL 2 / #endif`*

- **用途**：編譯期上限：高於此值的 `LOGW/I/D/T` 巨集展開成不產生程式碼的形式。
- **設計**：目的是 release 版二進位裡根本不存在 TRACE／DEBUG 的格式字串（README 的驗證：`strings build/bin/benchd | grep 'ring append'` 找不到，debug 版找得到），體積較小、也不洩漏內部細節，同時保留執行期可調整的彈性（規格的「兩階段」）。
- **細節**：
  - 可由編譯器旗標 `-DLOG_COMPILE_LEVEL=N` 覆寫；沒定義時預設 2（INFO）。Makefile 中 debug 為 4（`Makefile:18`）、release 為 2（`Makefile:21`）。
  - 在 `lib/` 與各可執行檔使用同一個旗標，所以同一種建置模式下所有程式的上限一致。
  - `bench_log_compile_level()` 回傳的是 `libbench.so` 編譯時的值（`lib/log.c:58`）；SIGUSR1 把層級調到超過它時，server 會警告 "this build only contains messages up to ..."（`server/main.c:205`）。
  - `LOGE` 完全不受這個值影響。
- **關係**：被誰使用 → `bench_log.h` 內的 `LOGW/LOGI/LOGD/LOGT`、`lib/log.c`；由 `Makefile` 設定。

### `BENCH_LOG_AT`
*位置：`include/bench_log.h:37`　簽章：`#define BENCH_LOG_AT(lvl, ...) do { if (bench_log_enabled(lvl)) bench_log_write((lvl), __FILE__, __LINE__, __VA_ARGS__); } while (0)`*

- **用途**：「編進去的」log 巨集的實作主體：執行期層級夠高才呼叫 `bench_log_write`。
- **設計**：
  - 先判斷再呼叫，是為了讓層級被關掉時**格式化的參數不會被求值**，所以像 `strerror(errno)` 這類有成本或有副作用的引數在不印時不會執行。
  - 用 `__FILE__`／`__LINE__` 自動帶入呼叫位置，log 行因此有 `server/session.c:88` 這樣的來源，不必手寫。
  - `do { ... } while (0)` 讓巨集在 `if (x) LOGI(...); else ...` 這種用法下仍是單一陳述式。
- **細節**：
  - `__VA_ARGS__` 的第一個引數必須是格式字串（至少要有一個）；`bench_log_write` 宣告了 `format(printf, 4, 5)`，加上 `-Wformat=2`，格式錯誤在編譯期被抓到。
  - `lvl` 被展開兩次，呼叫端只傳字面的列舉常數。
  - 即使是執行期關閉，每次仍會有一次函式呼叫（`bench_log_enabled` 在共享函式庫中，不是行內）。
- **關係**：被誰使用 → `LOGE`、`LOGW`、`LOGI`、`LOGD`、`LOGT`；呼叫 → `lib/log.c:bench_log_enabled`、`bench_log_write`。

### `BENCH_LOG_OFF`
*位置：`include/bench_log.h:39`　簽章：`#define BENCH_LOG_OFF(...) do { (void)sizeof(printf(__VA_ARGS__)); } while (0)`*

- **用途**：被編譯期層級關掉的 log 巨集的展開結果。
- **設計**：不用空的 `do {} while (0)`，而是把整個 `printf(...)` 放進 `sizeof` 內（不求值運算元）。這樣：(1) 不產生任何機器碼，也沒有任何引數被求值；(2) 格式字串字面值不會被放進二進位檔（DECISIONS「Logging」說明已用 `strings` 驗證）；(3) 編譯器仍然對引數做型別檢查與 `-Wformat=2` 檢查，並且引數中的變數仍算「被使用」，不會出現 "unused variable" 警告，例如只為了 log 而存在的區域變數。
- **細節**：
  - 以 `printf` 代替 `bench_log_write` 只是借它的格式屬性與傳回型別，因此要 include `<stdio.h>`。
  - `(void)` 避免 "value computed is not used" 警告。
  - 副作用在這個形式下**不會**發生，所以不能把必要的動作（例如 `i++`）寫在 log 的引數裡。
- **關係**：被誰使用 → 編譯期層級不足時的 `LOGW`、`LOGI`、`LOGD`、`LOGT`。

### `LOGE`
*位置：`include/bench_log.h:41`　簽章：`#define LOGE(...) BENCH_LOG_AT(LOG_ERROR, __VA_ARGS__)`*

- **用途**：記錄錯誤。唯一一個沒有編譯期條件的 log 巨集。
- **設計**：錯誤訊息在任何建置、任何執行期層級下都存在且會輸出，因為出錯時需要看到原因；同時 `LOG_ERROR` 是最低層級 0，執行期也不會被關掉。
- **細節**：用於無法繼續或不預期的失敗，例如 `fork()` reader 失敗（`server/main.c:68`）、reader 死亡（`:86`）、輸入 pipe 寫入失敗（`server/session.c:60`）、鎖失敗（`server/robust.c:55`）。
- **關係**：被誰使用 → `server/main.c`、`server/reader.c`、`server/session.c`、`server/robust.c`、`tools/benchsim.c`；呼叫 → `BENCH_LOG_AT`。

### `LOGW`
*位置：`include/bench_log.h:42`　簽章：`#define LOGW(...)`（`LOG_COMPILE_LEVEL >= 1` 時為 `BENCH_LOG_AT(LOG_WARN, ...)`，否則 `BENCH_LOG_OFF(...)`）*

- **用途**：記錄警告：不會終止程式但值得注意的狀況，如協定違規斷線、拒絕連線、client 落後被丟資料、層級被切換。
- **設計**：編譯期門檻 1；release 與 debug 都滿足（2 與 4），預設一定編入。
- **細節**：範例 → `violation()` 的 "protocol violation from ..."（`server/session.c:47`）、`reject_busy` 的 "rejected ... server full"（`server/main.c:130`）、`send_dropped` 的 "fell behind"（`server/session.c:110`）。
- **關係**：被誰使用 → server、`client/session.c`、`tools/benchsim.c`（`lib/` 目前沒有 `LOGW`）；呼叫 → `BENCH_LOG_AT` 或 `BENCH_LOG_OFF`。

### `LOGI`
*位置：`include/bench_log.h:47`　簽章：`#define LOGI(...)`（`LOG_COMPILE_LEVEL >= 2` 時為 `BENCH_LOG_AT(LOG_INFO, ...)`，否則 `BENCH_LOG_OFF(...)`）*

- **用途**：記錄一般事件：client 加入／離開、控制權取得／釋放、啟動與關機訊息。
- **設計**：編譯期門檻 2，是 release 的預設上限；執行期預設層級也是 INFO，所以平常啟動就看得到。
- **細節**：範例 → 加入訊息（`server/session.c:285`）、離開訊息（`:74`）、"control granted"（`:367`）、server 啟動訊息（`server/main.c:401`）。
- **關係**：被誰使用 → server、client；呼叫 → `BENCH_LOG_AT` 或 `BENCH_LOG_OFF`。

### `LOGD`
*位置：`include/bench_log.h:52`　簽章：`#define LOGD(...)`（`LOG_COMPILE_LEVEL >= 3` 時為 `BENCH_LOG_AT(LOG_DEBUG, ...)`，否則 `BENCH_LOG_OFF(...)`）*

- **用途**：除錯訊息：連線細節、被忽略的封包、`getaddrinfo` 失敗等。
- **設計**：編譯期門檻 3，release 版（上限 2）完全不包含這些字串；debug 版預設也要用 `-v` 或 SIGUSR1 才會印出。
- **細節**：範例 → "accepted ... -> child"（`server/main.c:196`）、`tcp_connect_timeout` 的 `getaddrinfo` 失敗（`lib/net.c:102`）、"ignoring unknown packet type"（`server/session.c:418`）。
- **關係**：被誰使用 → server、client、`lib/`；呼叫 → `BENCH_LOG_AT` 或 `BENCH_LOG_OFF`。

### `LOGT`
*位置：`include/bench_log.h:57`　簽章：`#define LOGT(...)`（`LOG_COMPILE_LEVEL >= 4` 時為 `BENCH_LOG_AT(LOG_TRACE, ...)`，否則 `BENCH_LOG_OFF(...)`）*

- **用途**：最細的追蹤訊息，只存在於 debug 建置。
- **設計**：門檻 4。最主要的用途是 `bn_send`／`bn_recv` 對每個封包印出 `tx/rx pkt type= len= crc=`，可以拿 log 對照 tcpdump／Wireshark 的擷取（DECISIONS「Logging」）。因為這是每個封包都會呼叫的熱路徑，在 release 版中展開成 `BENCH_LOG_OFF`，連執行期的層級判斷都不會有。
- **細節**：範例 → `lib/proto.c:56`、`:135`；`server/reader.c:165`、`:250`（"ring append ..."）；`server/session.c:358`。README 的 `strings build/bin/benchd | grep 'ring append'` 就是用這個巨集驗證 release 版沒有 TRACE 字串。
- **關係**：被誰使用 → `lib/proto.c`、`server/reader.c`、`server/session.c`；呼叫 → `BENCH_LOG_AT` 或 `BENCH_LOG_OFF`。

---

## `server/server.h`

`server.h` 是 benchd 伺服器端所有 `.c` 檔共用的內部標頭（被 `main.c`、`reader.c`、`session.c`、`ring.c`、`control.c`、`robust.c`、`sysinfo.c` 七個檔案 include，client 與 tools 不會用到）。它本身不含任何可執行程式碼，只定義三種東西：(1) master 在 `fork()` 之前建好、之後三種行程（master / reader / 每個 client 的 child）共同使用的共享記憶體結構 `struct bench_shm`（含環形緩衝區 `struct ring_rec`、client 名冊 `struct bench_slot`）；(2) child 傳給 reader 的管線訊息 `struct pipe_msg`；(3) 命令列設定 `struct server_cfg` 與跨檔案的函式原型。檔頭註解說明整個 IPC 只有三樣東西：匿名 `MAP_SHARED` 對映、對映內一把 process-shared robust mutex、以及一條 children → reader 的 pipe，且全部在第一次 `fork()` 之前建立（見 `server/main.c:344-362`），所以不需要 socketpair、signal 傳資料或任何 child 之間的通訊。它依賴 `include/bench_proto.h`（提供 `BN_NAME_LEN`=32、`BN_MAX_INPUT`=256、`DEV_UP/DEV_LOST`、`struct bn_ctrl_result`）與 `<pthread.h>`（只為 `pthread_mutex_t`，benchd 從不建立 thread，見 docs/DECISIONS.md「Threads and synchronisation」）。整個檔案以 `BENCH_SERVER_H` include guard 包起來（`server/server.h:15-16`、`:142`）。

### `BENCH_MAX_SLOTS`
*位置：`server/server.h:24`　簽章：`#define BENCH_MAX_SLOTS 1024`*

- **用途**：共享記憶體中 client 名冊 `slots[]` 的固定長度，也是 `--max-clients` 的絕對上限。
- **設計**：共享記憶體在 `fork()` 之前一次建好，之後不能再增長，所以名冊必須是編譯期固定大小的陣列，而不是動態結構。1024 遠大於預設的 32 個 client，讓 `--max-clients` 有調大的空間。
- **細節**：
  - `main.c:326-327` 把使用者給的 `--max-clients` 夾在 1..`BENCH_MAX_SLOTS`。
  - 所有掃描名冊的迴圈都是 `for (i < BENCH_MAX_SLOTS)`：`robust.c:73`（`slots_recount`）、`robust.c:83`（`slots_in_use`）、`main.c:97`（回收 child）、`main.c:155`（找空 slot）、`main.c:227`、`main.c:241`（shutdown）。
  - `main.c:239` 的 `pid_t pids[BENCH_MAX_SLOTS]` 把它也當成 stack 陣列長度（4096 位元組）。
  - 名冊佔用 `1024 * 120 = 122880` 位元組（見 `struct bench_slot`），是 `sizeof(struct bench_shm)` 的絕大部分。
- **關係**：被誰使用 → `struct bench_shm.slots`、`main.c`、`robust.c`。

### `BENCH_SERVER_H`
*位置：`server/server.h:15-16`、`:142`　簽章：`#ifndef BENCH_SERVER_H` / `#define BENCH_SERVER_H` … `#endif`*

- **用途**：include guard，避免 `server.h` 在同一個編譯單元被重複展開。
- **設計**：標頭裡有 `struct` 與 `enum` 定義，重複展開會造成重複定義錯誤。七個 server `.c` 都 include 它，而 `server.h` 自己又 include `bench_proto.h`，保護是必要的。
- **關係**：只有前處理器使用；沒有任何 `.c` 檢查這個巨集。

### `REC_DATA`
*位置：`server/server.h:25`　簽章：`#define REC_DATA 1`*

- **用途**：`struct ring_rec.type` 的值之一，代表這筆記錄的 payload 是從 UART 讀到的原始位元組。
- **設計**：資料與事件放進「同一條」ring，而不是兩條佇列，這樣每個 viewer 依 offset 順序讀出來時，資料與事件的相對順序對所有人一致，歷史重播時也一樣（README「Broadcast needs no child-to-child IPC」、`ring.c` 檔頭）。因為兩者共用一條 ring，才需要 type 欄位區分。
- **細節**：
  - 只由 reader 寫入（`reader.c:246`，`code` 固定為 0）。
  - 值從 1 開始，0 不是合法 type：`ring_repair` 遇到 type 不是 `REC_DATA` 或 `REC_EVENT` 就判定鏈已損壞（`ring.c:152`），所以被清成 0 的記憶體也會被視為損壞。
  - `session.c:send_record`（`session.c:81`）沒有專門判斷 `REC_DATA`：非 `REC_EVENT` 的記錄一律當 `BN_DATA` 送出（註解說明 `--unsafe=overrun` 時讀到垃圾也會走這條路）。
- **關係**：被誰使用 → `reader.c`（寫）、`ring.c`（`ring_repair` 驗證）、`session.c`。

### `REC_EVENT`
*位置：`server/server.h:26`　簽章：`#define REC_EVENT 2`*

- **用途**：`struct ring_rec.type` 的值之一，代表 payload 是 benchd 自己產生的事件文字（裝置斷線/恢復、取得/釋放控制權、改 baud、DROPPED、伺服器關閉）。
- **設計**：事件寫進 ring 而不是另開通知管道，是為了讓事件與它前後的 UART 資料有確定的先後順序；每個 child 只要照 offset 讀同一條 ring，就會對所有 client 產生一致的畫面，不需要 child 之間互相通知。
- **細節**：
  - 只由 `ring_event()` 寫入（`ring.c:84`）；`code` 是 `include/bench_proto.h` 的 `enum bn_evt`（`EV_DEVICE_LOST`…`EV_SERVER_SHUTDOWN`）。
  - `session.c:send_record` 以 `r->type == REC_EVENT` 判斷，是則把 `code`、`ts_ns` 轉成 big-endian 包成 `BN_EVENT`（`session.c:83-90`）。
- **關係**：被誰使用 → `ring.c`、`session.c`。

### `REC_MAX_DATA`
*位置：`server/server.h:27`　簽章：`#define REC_MAX_DATA 1024`*

- **用途**：單一 ring 記錄 payload 的最大長度（位元組）。
- **設計**：reader 每次最多 `read()` 這麼多位元組並剛好包成一筆 DATA 記錄（`reader.c:232` 的 `uint8_t buf[REC_MAX_DATA]`），所以一筆記錄永遠遠小於 ring 容量（最小 4096，見 `struct server_cfg.history`），驅逐舊記錄時一次最多只需丟掉少數幾筆。同一個常數也兼作「長度是否合理」的檢查門檻：任何 `len > REC_MAX_DATA` 都被視為鏈損壞或讀到被覆寫的垃圾。
- **細節**：
  - `ring_append` 會把過長的 `len` 截為 `REC_MAX_DATA`（`ring.c:51-52`）。
  - 驗證點：`ring_append` 驅逐時（`ring.c:59`）、`ring_seek`（`ring.c:94`）、`ring_copy`（`ring.c:117`）、`ring_repair`（`ring.c:152`）。
  - `session.c:40` 的 `sendbuf` 大小為 `sizeof(struct bn_event_hdr) + REC_MAX_DATA + 8`，剛好容得下最大一筆記錄加 header。
  - `--unsafe=overrun`（R10）時，`ring_copy` 讀到超過此值的 len 不丟棄，只夾成 `REC_MAX_DATA` 繼續走（`ring.c:117-123`），這就是畫面上出現殘缺位元組的原因。
- **關係**：被誰使用 → `ring.c`、`reader.c`、`session.c`。

### `HELLO_TIMEOUT_MS`
*位置：`server/server.h:28`　簽章：`#define HELLO_TIMEOUT_MS 5000`*

- **用途**：R5（`--unsafe=timeout`）的握手期限：新連線必須在 5 秒內送出完整 HELLO。
- **設計**：fork-per-client 的模型下，每個沉默的 socket 都佔一個 child 與一個 slot（`slots_in_use` 計入 `--max-clients` 額度），若不設期限，32 個什麼都不送的連線就能把名額占滿、鎖死其他人（`demo_r5.sh`）。5 秒對正常 client 的 HELLO 綽綽有餘，對攻擊者則很短。數值以毫秒存放，因為 `session.c` 用 `mono_ms()` 加 `poll()` 的毫秒計時；傳給 `set_io_timeouts()`（單位為秒）時再除以 1000。
- **細節**：
  - 完全沒送：`session.c:198` 用 `poll()` 等到 deadline，逾時回 `BN_ERR_HANDSHAKE_TIMEOUT`（`:206`）。
  - 只送半個 HELLO：`session.c:444` 把 `SO_RCVTIMEO` 設為 `HELLO_TIMEOUT_MS/1000` = 5 秒，`recv` 於是在 5 秒後得到 `EAGAIN`（`session.c:214-217`）。這兩層缺一不可，因為 `poll()` 只保證「第一個位元組何時到」，不管後面。
  - 加 `--unsafe=timeout` 時 `session.c:196`、`:442` 兩處略過這些期限。
- **關係**：被誰使用 → `session.c`（`handshake`、`session_main`）。

### `IDLE_TIMEOUT_MS`
*位置：`server/server.h:29`　簽章：`#define IDLE_TIMEOUT_MS 60000`*

- **用途**：R5 的連線閒置期限：握手完成後，client 連續 60 秒沒有送任何封包就被斷線；同時也是 `SO_SNDTIMEO` 的值。
- **設計**：
  - 接收側：靠 `session.c:462` 比較 `mono_ms() - last_rx_ms > IDLE_TIMEOUT_MS`，送 `BN_ERR_IDLE_TIMEOUT` 後離開。正常 client 即使使用者沒操作，也每 15 秒送一個 `BN_PING`（`client/session.c:411-414`、`client/subcmd.c:182-184`），遠小於 60 秒，所以不會被誤殺。握手後也把 `SO_RCVTIMEO` 改成 60 秒（`session.c:236`）。
  - 送出側：`SO_SNDTIMEO` = 60 秒（`session.c:444`），讓「連線仍在、但對方永遠不讀」的 client 不能讓 child 卡在 `write()` 上無限期占住名額。slow client 只阻塞自己的 child 是 fork 模型的優點，但那個 child 與 slot 仍要能被回收。
- **細節**：
  - 與 `server_cfg.idle_release`（預設 120 秒）不同：後者是「控制權」閒置多久自動釋放（R6），與連線是否閒置無關。
  - `--unsafe=timeout` 時 `session.c:235`、`:442`、`:462` 都略過。
- **關係**：被誰使用 → `session.c`（`handshake`、`session_main`）。

### `struct ring_rec`
*位置：`server/server.h:32-37`　簽章：`struct ring_rec { uint32_t len; uint16_t type; uint16_t code; uint64_t ts_ns; }`*

- **用途**：環形緩衝區裡每一筆記錄的固定 16 位元組表頭，後面緊接 `len` 位元組的 payload，再補到 8 的倍數。
- **設計**：
  - 欄位順序 `len`(4) + `type`(2) + `code`(2) + `ts_ns`(8)，剛好 16 位元組、沒有 padding，且 `ts_ns` 落在 offset 8，符合 8 位元組對齊（實測 `sizeof == 16`、`offsetof(ts_ns) == 8`）。
  - 使用主機位元組序（註解 "never leaves the machine"）：它只存在於同一台機器的共享記憶體，不需要像網路封包那樣轉 big-endian；真正送出時才在 `session.c:send_record` 轉成 `htobe64/htobe16`。
  - `ts_ns` 是 `CLOCK_REALTIME`，在 `ring_append` 內持鎖時取得（`ring.c:66`），所以時間戳反映「進入 ring 的時刻」，而不是 client 收到的時刻。
  - 把 `code` 放在表頭而不是 payload 內，讓 event 的種類不必解析文字就能還原成 `bn_event_hdr.code`。
- **細節**：
  - `len` 只算 payload，不含 16 位元組表頭與尾端 padding；記錄實際佔用大小用 `REC_SIZE(len)`。
  - `code`：`REC_EVENT` 時是 `enum bn_evt`，`REC_DATA` 時為 0（註解）。
  - 寫入者：只有 `ring_append`（持鎖），而 `ring_append` 的呼叫者是 reader（DATA）與各處 `ring_event`（EVENT）。讀取者：`ring_append`（驅逐時讀最舊記錄的表頭，`ring.c:57`）、`ring_seek`、`ring_copy`、`ring_repair`，以及 child 在 `pump()` 內把 `ring_copy` 複製出來的記錄直接以此型別解讀（`session.c:158`）。
  - 表頭可能因為環繞而被切成兩段放在 ring 尾端與開頭，所以存取一律透過 `ring_put/ring_get` 兩段式 memcpy（`ring.c:24-40`），不能直接 cast ring 內的指標。`pump()` 的 cast 是對 `copybuf` 做的，`copybuf` 宣告為 `aligned(8)`（`session.c:39`）且每筆記錄大小都是 8 的倍數，因此 cast 後的指標自然對齊。
- **關係**：被誰使用 → `ring.c`（全部）、`session.c`（`send_record`、`pump`）。

### `REC_SIZE`
*位置：`server/server.h:38`　簽章：`#define REC_SIZE(len) ((uint64_t)((sizeof(struct ring_rec) + (uint64_t)(len) + 7u) & ~(uint64_t)7u))`*

- **用途**：算出一筆 payload 為 `len` 位元組的記錄在 ring 中實際佔用的位元組數：表頭 16 + payload，向上補齊到 8 的倍數。
- **設計**：
  - 讓每一筆記錄的起點（邏輯 offset）都是 8 的倍數，因為 head、tail 都從 0 開始、每次只加 `REC_SIZE`；再加上 `main.c:323` 把 ring 容量 `history` 向上取整到 8 的倍數，所以物理位置 `offset % cap` 也維持 8 對齊。這樣表頭裡的 `uint64_t ts_ns` 不會跨出對齊邊界，`pump()` 對 `copybuf` 的 cast 也安全。
  - `len` 先轉成 `uint64_t` 再相加，避免 32 位元 `len` 運算溢位；整個結果也是 `uint64_t`，與 `head/tail/offset` 同型別，不會在比較時發生符號或寬度問題。
  - 以 macro 而非函式實作，可在 `static` 初始值與陣列大小等編譯期情境使用；參數 `len` 在展開中只出現一次，傳入有副作用的運算式是安全的。
- **細節**：
  - 實際值：`REC_SIZE(0)=16`、`REC_SIZE(1)=24`、`REC_SIZE(9)=32`、`REC_SIZE(1024)=1040`（本機編譯實測）。
  - 被用於：`ring.c:53,58,96,124,153,157`（寫入、驅逐、seek、copy、repair 的步進）與 `session.c:159`（`pump` 逐筆走過 `copybuf` 並累加 offset，作為送給 client 的 `end_off`）。
  - 呼叫者必須先確保 `len <= REC_MAX_DATA`；巨集本身不檢查，對垃圾 `len` 也會照算（`ring.c` 的呼叫點都在算之前或之後檢查 `len > REC_MAX_DATA`）。
- **關係**：被誰使用 → `ring.c`、`session.c`。

### `struct bench_slot`
*位置：`server/server.h:41-48`　簽章：`struct bench_slot { pid_t pid; int joined; char name[BN_NAME_LEN]; char peer[64]; uint64_t offset; uint64_t joined_ns; }`*

- **用途**：共享記憶體內每個 client 一格的名冊，讓 master 能數 child 數量（`--max-clients`、`slots_in_use`）、讓 child 能算 viewer 數（`slots_recount`），讓 shutdown 知道哪些 child 還沒把 `SERVER_SHUTDOWN` 事件送完。
- **設計**：
  - 欄位大小實測 `sizeof == 120`：`pid`(0) `joined`(4) `name`(8..40) `peer`(40..104) `offset`(104) `joined_ns`(112)，完全沒有空洞，且 64 位元欄位都在 8 的倍數上。
  - slot 以陣列下標（0..`BENCH_MAX_SLOTS-1`）定址，下標在 `fork()` 時由 master 決定並以參數傳給 `session_main(fd, slot, …)`（`main.c:188`），所以 child 不需要掃描就能找到自己那一格。
  - `pid` 有三種狀態：`0` 為空閒；`-1` 為 master 剛選定、尚未 `fork()` 完成的「預留」（`main.c:161`），標示這一格已被 master 選定、在 `fork()` 返回前不再被視為空閒；正值為 child 的 pid。`fork()` 之後 master 把 `-1` 改成真正的 pid（`main.c:192-193`），child 在握手時也會自己寫入自己的 pid（`session.c:246`），兩者誰先到都正確。
  - 因為預留中的 `-1` 也算「非 0」，`slots_in_use` 會把它算進佔用數；而 shutdown 只針對 `pid > 0` 的 slot 計算與送 SIGTERM（`main.c:228,242`），不會對 `-1` 送 signal。
- **細節**：
  - `joined`：握手（HELLO 驗證、WELCOME 資料備妥）完成後才設為 1（`session.c:247`）。`slots_recount` 只計 `pid && joined`（`robust.c:74`），所以還在握手的連線不算 viewer；但 `slots_in_use` 只看 `pid`，仍然佔用 `--max-clients` 的額度。
  - `name`：已經過 `sanitize_name` 的 client 名稱，長度 `BN_NAME_LEN`=32；`peer`：`"ip:port"` 字串（`sock_peer_str`，master 以 64 位元組緩衝區產生，`main.c:149-150`）；`joined_ns`：握手完成的 `CLOCK_REALTIME` 時間。這三個欄位目前只有 `session.c:248-250` 寫入，原始碼中沒有任何地方讀取，用途是留在共享記憶體中供除錯檢視。
  - `offset`：該 child 目前串流到的 ring 位置。`pump()` 每輪在持鎖時寫入本輪起點 `start`（`session.c:147-148`）。shutdown 對 `pid > 0 && joined` 的 slot 比較 `slots[i].offset < target`，判斷這個 child 是否已經收完 `SERVER_SHUTDOWN` 事件所在的位置（`main.c:228`），所以尚未完成握手的 slot 不會被等待。
  - 清除：child 正常結束時 `finish()` 自己清（`session.c:67-68`，且先檢查 `pid == me`）；child 被殺或崩潰時由 master 的 `reap_children` 以 `memset` 清（`main.c:97-99`）；EOWNERDEAD 復原時不清 slot，只呼叫 `slots_recount` 重算 `viewers`（`robust.c:50`）。
  - 所有讀寫都必須持有 `bench_shm.lock`。
- **關係**：被誰使用 → `main.c`（`handle_accept`、`reap_children`、`shutdown_server`）、`session.c`（`handshake`、`pump`、`finish`）、`robust.c`（`slots_recount`、`slots_in_use`）。

### `struct bench_shm`
*位置：`server/server.h:50-80`　簽章：`struct bench_shm { pthread_mutex_t lock; uint64_t cap, head, tail; …; struct bench_slot slots[BENCH_MAX_SLOTS]; uint8_t ring[]; }`*

- **用途**：整個伺服器的共享狀態：一把鎖、環形緩衝區的描述子、裝置狀態、控制權狀態、統計與設定，以及 client 名冊與 ring 本體。master 以一次匿名 `mmap(MAP_SHARED|MAP_ANONYMOUS)` 建立（`main.c:344-345`，大小 `sizeof(struct bench_shm) + g_cfg.history`），之後所有 `fork()` 出的 reader 與 child 都繼承同一塊實體記憶體，指標值 `g_shm` 也直接有效。
- **設計**：
  - 「欄位全部放同一個 struct」讓整個 IPC 只需要一次 mmap 與一把鎖；並且 `mmap` 的匿名頁由核心清零，所以 `slots[]` 全部是「空閒」、所有計數器從 0 開始，`main.c` 只需補寫非零的初值（`main.c:350-360`）。
  - 最後一個成員 `uint8_t ring[]` 是 C99 彈性陣列成員：`sizeof(struct bench_shm)` 不含它，ring 的位元組數 `cap` 由 `main.c:344` 在配置時加上去。把 ring 接在結構尾端，使 ring 本體與狀態在同一段連續位址、同一個 `munmap`（`main.c:267`）處理。本機實測 `sizeof(struct bench_shm)=123208`（8 的倍數），所以 `ring` 起點對齊 8。
  - 全部欄位由「單一把 mutex」保護，不存在巢狀鎖；因此沒有鎖順序問題，且「持鎖時不做 I/O」成為規則（docs/DECISIONS.md：復原訊息、控制權釋放訊息都在解鎖後才印）。唯一例外是 `log_level`（見下）。
- **細節**（欄位分組，括號內為寫入者與行程）：
  - **`lock`**（`:53`）：`pthread_mutex_t`，大小 40 位元組，位於 offset 0。由 master 在 fork 前以 `shm_mutex_init` 初始化成 `PTHREAD_PROCESS_SHARED` 並（預設）加上 `PTHREAD_MUTEX_ROBUST`（`robust.c:28-37`）。註解強調它「純粹是行程間的鎖」，benchd 不建立 thread。`--unsafe=robust-mutex`（R7）時不設 ROBUST，持鎖者死亡就會讓所有行程永遠卡住。
  - **`cap` / `head` / `tail`**（`:55-57`，offset 40 起）：ring 的容量與兩個邏輯 offset。offset 只會增加、不回繞，物理位置是 `offset % cap`，所以 client 可以用「最後看到的 offset」續傳（docs/DECISIONS.md「Protocol」）。`head` 是下一筆記錄的寫入位置，也是「發佈點」：`ring_append` 最後才推進 `head`，行程在寫到一半死掉最多留下 head 之外看不見的半筆記錄（`ring.c` 檔頭）；`tail` 是最舊的有效記錄起點。不變式：`tail <= head` 且 `head - tail <= cap`，`ring_repair` 檢查的就是它（`ring.c:144`）。`cap` 由 `ring_init` 設定（master），`head/tail` 由 `ring_append`、`ring_repair` 推進。
  - **裝置**：`dev_state`（`DEV_UP`/`DEV_LOST`，常數在 `bench_proto.h`；master 初值 `DEV_LOST`，之後只有 reader 改，`reader.c:99,109,222,226`）；`dev_path[128]`（master 啟動時複製，大小與 `bn_welcome.dev_path[128]` 一致，WELCOME 直接 `strlcpy` 過去）；`baud`（master 初值、reader 處理 `PIPE_BAUD` 時更新，`reader.c:179`）；`rx_bytes` / `tx_bytes`（reader 累計，`reader.c:247,162`）；`dev_reconnects`（`reader.c:110`）；`last_rx_ns`（`reader.c:248`）。這些都由 reader 寫、child 與 `sysinfo_build` 讀。
  - **控制權**：`ctrl_pid`（0 = 沒人）、`ctrl_name`、`ctrl_last_input_ns`、`idle_release_sec`。`ctrl_pid/ctrl_name` 只在 `control.c` 的 `ctrl_*_locked` 與 `shm_lock` 的 EOWNERDEAD 復原中改變；`ctrl_last_input_ns` 在取得控制權與 child 轉送 INPUT/BAUD 時更新（`control.c:56`、`session.c:351,397`）；`idle_release_sec` 由 master 從 `--idle-release` 填入（0 代表永不自動釋放）。
  - **統計與設定**：`viewers`（已完成握手的 client 數，只由 `slots_recount` 更新）；`max_clients`（master 填入，目前僅 `sysinfo_build` 讀取顯示，實際的上限檢查用的是 `g_cfg.max_clients`，`main.c:164`）；`server_start_ns`（master 填入；WELCOME 回報，並用來判斷 client 的 `last_server_start_ns` 是否同一次啟動，決定能否續傳，`session.c:266`；供 client 重連（R12）續傳時使用）；`unsafe_mask`（master 複製一份給 `sysinfo_build` 顯示；各行程實際判斷 `UNSAFE()` 用的是 fork 時繼承的全域 `bench_unsafe_mask`）；`master_pid`（child 與 reader 啟動時用 `getppid() != master_pid` 判斷 master 是否在 `fork` 與 `prctl(PR_SET_PDEATHSIG)` 之間死亡，`reader.c:208`、`session.c:432`）；`reader_pid`（`spawn_reader` 寫入，`main.c:75`）；`lock_recoveries`（`shm_lock` 每次 EOWNERDEAD 復原加 1，`robust.c:51`，INFO 的 `server.lock_recoveries` 顯示）。
  - **`log_level`**（`:74`，offset 304）：宣告為 `volatile int`，因為它是「不經鎖」被多個行程讀寫的：master 在 SIGUSR1 後的 `cycle_log_level` 寫（`main.c:202-204`），所有行程的 log 巨集讀。`main.c:360` 以 `bench_log_set_level_ptr(&g_shm->log_level)` 把 `lib/log.c` 的 level 指標指到這裡，讓全部行程共用一個等級。`volatile` 防止編譯器把它快取在暫存器；log 不需持鎖，因此 log 巨集在任何位置（含持鎖區之外）都能安全判斷等級。
  - **`slots[BENCH_MAX_SLOTS]`**（`:78`，offset 328）：見 `struct bench_slot`。
  - **`ring[]`**（`:79`，offset 123208）：`cap` 位元組的記錄區，內容是首尾相接的 `struct ring_rec` + payload。
  - 實測偏移（本機 x86_64 glibc，以 `offsetof` 取得）：`cap=40`、`log_level=304`、`slots=328`、`ring=123208`；其他平台的 `pthread_mutex_t` 大小不同，數字會不同，但程式碼不依賴這些數值。
- **關係**：建立 → `main.c`（mmap、`ring_init`、`shm_mutex_init`）；寫入 → reader（裝置與統計欄位）、child（控制權、slots）、master（slots 清理、`log_level`、shutdown 事件）；讀取 → `sysinfo.c`（以鎖快照全部欄位）、`session.c`（`fill_status`、`handshake`）。

### `PIPE_INPUT`
*位置：`server/server.h:83`　簽章：`enum { PIPE_INPUT = 1, … }`（匿名 enum 的第一個成員）*

- **用途**：`struct pipe_msg.type` 的值，代表「把這些位元組寫到 UART」，由持有控制權的 child 轉送給 reader。
- **設計**：只有 reader 持有 serial fd（README：避免行程互搶位元組），所以 child 不能自己 `write()` 到 tty，只能經 pipe 交給 reader。pipe 同時是順序保證：同一個 child 先後送出的 INPUT 與 RELEASE 在 pipe 內保持先後，見 `PIPE_RELEASE`。
- **細節**：
  - data 是原始位元組，`len` ≤ `BN_MAX_INPUT`（256）。
  - reader 在 `handle_pipe_msg` 持鎖再檢查一次 `m->pid == ctrl_pid`，不是控制者就丟棄，因為 child 檢查控制權之後到 reader 處理之前，控制權可能已被釋放（`reader.c:143`、`:150-153`）。
  - 這些成員是匿名 enum 常數，不是型別；`struct pipe_msg.type` 以 `uint16_t` 存。
- **關係**：產生 → `session.c`（`pipe_send`）；消費 → `reader.c`（`handle_pipe_msg`）。

### `PIPE_BAUD`
*位置：`server/server.h:83`　簽章：`PIPE_BAUD = 2`*

- **用途**：`struct pipe_msg.type` 的值，代表「把 serial 埠改成這個 baud」。
- **設計**：`tcsetattr` 要作用在 reader 持有的 serial fd 上，child 沒有這個 fd，所以改 baud 也必須由 reader 執行。走同一條 pipe 則讓它與先前的 INPUT 保持順序（先送出的輸入以舊 baud 寫完，再切換）。child 在送出前已用 `baud_supported` 驗證過，不支援的值回 `BAD_REQUEST` 而不會進 pipe（`session.c:401`）。
- **細節**：
  - data 是 4 位元組 `uint32_t`（主機位元組序，child 已先 `be32toh`，`session.c:393`），`len` 必須等於 4（`reader.c:170`）。
  - reader 處理時更新 `g_shm->baud` 並寫一筆 `EV_BAUD_CHANGED` 事件（`reader.c:177-181`），所以所有 viewer 都看得到是誰改的。
- **關係**：產生 → `session.c`；消費 → `reader.c`（`handle_pipe_msg`）。

### `PIPE_RELEASE`
*位置：`server/server.h:83`　簽章：`PIPE_RELEASE = 3`*

- **用途**：`struct pipe_msg.type` 的值，代表「釋放這個 child 持有的控制權」。
- **設計**：RELEASE 刻意不由 child 直接改 shm，而是走 pipe 由 reader 執行。若 child 直接釋放，而它先前送出的 INPUT 還在 pipe 裡排隊，reader 處理 INPUT 時 `ctrl_pid` 已被清掉，輸入會被當成「非控制者」丟棄；`bench send "x"`（TAKE、INPUT、RELEASE）就會丟掉輸入（docs/DECISIONS.md「Server」中 "An explicit RELEASE…" 一條）。經由同一條 FIFO pipe，RELEASE 一定排在同一個 child 的 INPUT 之後。master 的 SIGCHLD 路徑（child 已崩潰、無法再寫 pipe）則直接改 shm（`main.c:93-102`）。
- **細節**：
  - data 是釋放原因文字（不含 NUL），例如 `"disconnect"`（`session.c:73`）、`"released"`（`session.c:377`）；`len == 0` 或 ≥ 32 時 reader 用預設 `"released"`（`reader.c:186-190`）。原因會變成 EVENT 文字與 `ctrl_note.reason`（32 位元組）。
  - reader 再確認 `m->pid == ctrl_pid` 才釋放（`reader.c:192-193`）。
  - `--unsafe=ctrl-release`（R6）時，child 離開不再送 `PIPE_RELEASE`（`session.c:72`）。
  - 其他 type 值（包括 0）落入 `default`，記一筆 WARN（`reader.c:198-199`）。
- **關係**：產生 → `session.c`；消費 → `reader.c`（`handle_pipe_msg`）。

### `struct pipe_msg`
*位置：`server/server.h:84-89`　簽章：`struct pipe_msg { int32_t pid; uint16_t type; uint16_t len; uint8_t data[BN_MAX_INPUT]; }`*

- **用途**：child → reader 管線上的訊息格式：誰送的（`pid`）、什麼動作（`type`）、帶多少資料（`len`）、資料本體。
- **設計**：
  - 表頭固定 8 位元組（`int32_t`+`uint16_t`+`uint16_t`，無 padding），後面才是長度 `len` 的資料；整個結構最大 `sizeof == 264`（8 + 256）。
  - 註解寫明「Whole message <= PIPE_BUF: atomic write」。POSIX 保證寫入長度不超過 `PIPE_BUF`（本機為 4096）的 `write()` 到 pipe 時不會與其他寫者交錯。因為每個 child 寫入的長度最多是 8 + 256 = 264，遠小於 `PIPE_BUF`，多個 child 同時寫同一條 pipe 也不會互相切碎，reader 因此可以放心用「先讀 8 位元組表頭、再讀 `len` 位元組資料」的兩段式讀取。
  - 傳送端只寫 `PIPE_MSG_HDR + len` 位元組而不是整個 264 位元組（`session.c:59`），避免每次浪費 pipe 空間。
  - 用主機位元組序，因為 pipe 只在同一台機器的行程間。
  - `pid` 用 `int32_t`（Linux 上 `pid_t` 也是 `int`），讓訊息格式與 `pid_t` 的實作脫鉤；reader 以它對照 `g_shm->ctrl_pid` 判斷訊息來源是否仍是控制者。
- **細節**：
  - 傳送：`pipe_send` 在 stack 上建好結構，再用 `writen` 寫出（`session.c:54-61`）。`session.c:345` 在 R3 unsafe 時仍把 `h.len` 夾到 `BN_MAX_INPUT`，所以 `memcpy(m.data, …)` 不會超出 `data[256]`。
  - 接收：`reader.c:261-266` 先 `readn(pipe_rd, &m, PIPE_MSG_HDR)`，確認 `m.len <= BN_MAX_INPUT` 之後才讀資料，再交給 `handle_pipe_msg`。
  - 管線的讀端（`g_cfg.input_pipe[0]`）只留給 reader，child 關掉它；寫端只留給 child，reader 關掉它（`reader.c:215`、`session.c:439`）。
- **關係**：被誰使用 → `session.c`（寫）、`reader.c`（讀）。

### `PIPE_MSG_HDR`
*位置：`server/server.h:90`　簽章：`#define PIPE_MSG_HDR 8`*

- **用途**：`struct pipe_msg` 的表頭位元組數（`pid`+`type`+`len`），也就是 `data` 之前的長度。
- **設計**：寫死為 8 而不是用 `offsetof(struct pipe_msg, data)`，因為它同時是「線上格式」的一部分：傳送端寫 `PIPE_MSG_HDR + len` 位元組、接收端先讀 `PIPE_MSG_HDR` 位元組，兩端必須一致。本機實測 `offsetof(struct pipe_msg, data) == 8`，兩者相等。
- **細節**：`session.c:59`（寫入長度）、`reader.c:263-264`（讀表頭並檢查讀到的位元組數剛好是 8）。
- **關係**：被誰使用 → `session.c`、`reader.c`。

### `struct server_cfg`
*位置：`server/server.h:92-102`　簽章：`struct server_cfg { const char *device; uint32_t baud; uint16_t port; uint64_t history; uint32_t max_clients; uint32_t idle_release; int input_pipe[2]; int listen_fd; int fault_die_in_lock_at; }`*

- **用途**：master 解析命令列後得到的啟動設定與幾個「行程私有」的檔案描述符，以全域變數 `g_cfg` 保存。
- **設計**：
  - 它是普通全域變數（`.bss`），不在共享記憶體；`fork()` 時每個行程各得一份寫入時複製的副本。所有設定與 fd 都在第一次 `fork()` 之前就定案，所以 reader 與 child 看到的是 fork 當下的值，之後 master 改變也不會影響它們。需要「執行期被別的行程看見」的值（`baud`、`max_clients`、`idle_release_sec`、`dev_path`）master 另外複製一份進 `bench_shm`（`main.c:353-356`）。
  - `device` 是指向 `argv` 的指標（`main.c:290`），不複製；reader 開裝置、重開裝置、寫 log 時使用。
  - `history` 是 ring 的位元組數：最小夾到 4096（`main.c:321-322`），並向上取整為 8 的倍數（`main.c:323`）以維持記錄對齊。預設 64 KiB（`main.c:284`）。
  - 欄位型別：`baud`、`max_clients`、`idle_release` 用 `uint32_t`，`port` 用 `uint16_t`，`history` 用 `uint64_t`。預設值 115200、`BN_DEFAULT_PORT`(9000)、64 KiB、32、120 秒（`main.c:282-286`）；`idle_release` 的單位是秒，0 代表永不自動釋放控制權。
  - `max_clients` 夾在 1..`BENCH_MAX_SLOTS`（`main.c:324-327`）。
- **細節**：
  - `input_pipe`：`pipe()` 建立（`main.c:362`），`[0]` 讀端給 reader，`[1]` 寫端給 child；兩端在 fork 之後由 reader 與 child 各自關掉不用的那端。
  - `listen_fd`：`tcp_listen()` 回傳的 listening socket（`main.c:378`），只有 master 需要，reader 與 child 啟動就 `close` 掉（`reader.c:214`、`session.c:438`）；listening socket 只由 master 負責 accept 與在 shutdown 時關閉（`main.c:215`）。
  - `fault_die_in_lock_at`：R7 的故障注入（`BENCH_FAULT=die_in_lock[:N]`），只有在編譯時有 `BENCH_FAULT_INJECTION`（debug build，`Makefile` 的 debug 模式）才會被 `main.c:367-376` 設值；欄位本身不放在 `#ifdef` 內，使結構在 release 與 debug 兩種建置下的佈局相同。預設 0 代表停用。`session.c:291` 比較 `conn_seq` 是否等於此值，是的話那個 child 在握手後持鎖並 `abort()`。
- **關係**：寫入 → `main.c`（`main`）；讀取 → `main.c`、`reader.c`、`session.c`。

### `g_shm`
*位置：`server/server.h:104`（宣告）；定義在 `server/main.c:33`　簽章：`extern struct bench_shm *g_shm;`*

- **用途**：指向共享記憶體的全域指標，所有 `.c` 檔直接以它存取共享狀態。
- **設計**：因為 `mmap` 發生在 `fork()` 之前（`main.c:345`），指標值本身在每個行程中都有效且指向同一塊實體記憶體，不需要把指標當參數傳遞。這也是為什麼介面上凡是要「持鎖」的函式（`ring_*`、`ctrl_*_locked`）都還是要明確接收 `struct bench_shm *s`：參數只是讓函式脫離全域、方便閱讀與測試，實體永遠是 `g_shm`。
- **細節**：master 在 `shutdown_server` 最後 `munmap(g_shm, shm_size)`（`main.c:267`）。
- **關係**：定義 → `main.c`；使用 → 所有 server `.c`。

### `g_cfg`
*位置：`server/server.h:105`（宣告）；定義在 `server/main.c:34`　簽章：`extern struct server_cfg g_cfg;`*

- **用途**：見 `struct server_cfg`。
- **設計**：不在共享記憶體，各行程私有；由於是 `static` 儲存期，未明確設值的欄位（如 `fault_die_in_lock_at`、`device` 在不給 `-d` 時）預設為 0／NULL。
- **關係**：定義 → `main.c`；使用 → `main.c`、`reader.c`（`device`、`listen_fd`、`input_pipe`）、`session.c`（`input_pipe`、`listen_fd`、`fault_die_in_lock_at`）。

### `struct ctrl_note`
*位置：`server/server.h:124`　簽章：`struct ctrl_note { int happened; char who[BN_NAME_LEN]; char reason[32]; };`*

- **用途**：一個「待印的 log 訊息」暫存結構：記錄「控制權剛剛被誰以什麼理由釋放」，讓呼叫者在持鎖時只填資料，解鎖後再由 `ctrl_log_note` 印出。
- **設計**：目的是遵守「不在鎖內做 I/O」。`ctrl_release_locked` 在持鎖狀態下必須同時寫 ring 事件與清 `ctrl_pid`，但 `LOGI` 會做 `write()`，I/O 可能花時間，若在鎖內做會讓所有想拿鎖的行程一起等，所以改成先把 `who`/`reason` 拷到呼叫者 stack 上的 `ctrl_note`，離開臨界區再印（docs/DECISIONS.md「Logging and other I/O never happen inside the lock」）。`who` 大小等於 `BN_NAME_LEN`(32)，`reason` 固定 32 位元組（與 `reader.c:186` 的 `reason[32]` 同大小），兩者都用 `bench_strlcpy` 截斷（`control.c:78-79`）。`sizeof == 68`。
- **細節**：
  - 使用方式一致：呼叫者宣告 `struct ctrl_note note = { 0 };`（`reader.c:141,279`、`main.c:93`），把 `&note` 傳給 `ctrl_release_locked` / `ctrl_check_idle_locked`（也可傳 `NULL` 表示不需要記錄），解鎖後呼叫 `ctrl_log_note(&note)`。
  - `happened == 0` 代表沒有發生釋放，`ctrl_log_note` 什麼也不印（`control.c:96`）。
- **關係**：被誰使用 → `control.c`、`reader.c`（`handle_pipe_msg`、idle 檢查）、`main.c`（`reap_children`）。

### `函式原型一覽`
以下函式在這個標頭檔只宣告原型，設計與用途寫在實作它們的 `.c` 章節，這裡不重複：

- `ring_init` → server-ring
- `ring_append` → server-ring
- `ring_event` → server-ring
- `ring_seek` → server-ring
- `ring_copy` → server-ring
- `ring_repair` → server-ring
- `shm_mutex_init` → server-robust
- `shm_lock` → server-robust
- `shm_unlock` → server-robust
- `print_unsafe_banner` → server-robust
- `slots_recount` → server-robust
- `slots_in_use` → server-robust
- `ctrl_take_locked` → server-control
- `ctrl_release_locked` → server-control
- `ctrl_check_idle_locked` → server-control
- `ctrl_idle_sec` → server-control
- `ctrl_auto_release_in` → server-control
- `ctrl_log_note` → server-control
- `sysinfo_build` → server-sysinfo
- `reader_main` → server-reader
- `session_main` → server-session
- `baud_supported` → server-reader

---

## `client/client.h`

`client/client.h` 是 `bench` 客戶端的內部標頭檔，只被 `client/` 底下的六個 `.c`（`main.c`、`session.c`、`term.c`、`ui.c`、`cmd.c`、`subcmd.c`）include，不對外公開。程式碼跑在**單一的 `bench` 用戶端行程**裡（沒有 fork、沒有執行緒），互動模式與 `info/tail/watch/send` 子命令都用這裡定義的型別。檔案本身沒有任何函式實作，只有：一個巨集（`ESC_KEY`）、三個 struct（`client_opts`、`conn`、`app`）、三組 enum（ANSI 過濾狀態、連線狀態、flash 顏色），以及各 `.c` 對外函式的原型。它依賴 `bench_proto.h`（`BN_NAME_LEN`、`struct bn_hdr`、`struct bn_welcome` 等線上格式），並透過 `<stdio.h>` 取得 `FILE`、`size_t`。include guard 為 `BENCH_CLIENT_H`。`Makefile` 的物件檔規則把 `client/*.h` 列為依賴，改動本檔會重編所有 client 物件。

設計上最重要的一點：**線上格式（big-endian、packed）只出現在 `session.c` 的收發邊界**；`struct app` 與 `struct conn` 裡的數值一律是主機位元組序，由 `session.c` 在收到封包時轉換一次，`ui.c`、`cmd.c` 不必再碰 `be32toh()`。

### `BENCH_CLIENT_H`
*位置：`client/client.h:11-12`（結尾 `#endif` 在 142）　簽章：`#ifndef BENCH_CLIENT_H / #define BENCH_CLIENT_H`*

- **用途**：include guard，避免同一個 `.c` 經由多條路徑重複 include `client.h` 時，struct 與 enum 被重複定義而編譯失敗。
- **設計**：`client.h` 內含匿名 enum 與 struct 定義，重複展開一定會報錯，所以 guard 是必要的，不是風格選擇。它也 include `bench_proto.h`，所以 guard 只負責 `client.h` 自己的內容，不處理共用標頭。
- **關係**：被誰使用 → 只是前置處理器巨集，沒有執行期行為；六個 `client/*.c` 都 `#include "client.h"`。

### `ESC_KEY`
*位置：`client/client.h:19`　簽章：`#define ESC_KEY 0x1D   /* Ctrl-] */`*

- **用途**：互動模式的「跳脫鍵」，即 Ctrl-]（ASCII 0x1D）。按下後下一個鍵被解讀成命令（`t` take、`r` release、`:` 命令列等）。
- **設計**：選 Ctrl-] 的原因在程式碼中沒有明說；可確認的是 raw mode 下它不會被終端機當成訊號（`term_init` 已關掉 `ISIG`）。要把 0x1D 本身送到裝置時，連按兩次 Ctrl-]。
- **細節**：
  - 值是 0x1D（十進位 29）。`cmd_keys()` 看到它就設 `a->esc_pending = 1`；`escape_key()` 的 `case ESC_KEY` 在「持有控制權、已連線、`ibuf` 未滿」時把一個 0x1D 放進 `a->ibuf`，否則直接丟掉。
  - 命名容易與下面的 `ESC_NONE`…`ESC_ONE_MORE` 混淆：`ESC_KEY` 是**使用者鍵盤**的跳脫鍵，`ESC_*` enum 是**裝置輸出**裡的 ANSI ESC (0x1B) 序列狀態，兩者無關。
- **關係**：被誰呼叫 → `client/cmd.c` 的 `cmd_keys`（`cmd.c:168`）與 `escape_key`（`cmd.c:123`、`cmd.c:124`）使用。

### `struct client_opts`
*位置：`client/client.h:21`　簽章：`struct client_opts { char host[256]; uint16_t port; char name[BN_NAME_LEN]; uint64_t replay; int replay_set; int reconnect; int verbose; double duration; const char *out; unsigned wait_ms; int raw; };`*

- **用途**：把命令列解析的結果集中成一個 struct。`main()` 填寫，之後整個行程唯讀地使用；互動模式把它整份放在 `struct app` 的 `o` 欄位，子命令則直接以指標傳進 `sub_*()`。
- **設計**：把「連線所需」的欄位（`host/port/name/replay`）與「只有子命令用」的欄位分成上下兩組，讓 `conn_connect()` 這種共用函式只需要一個 `const struct client_opts *`，不必知道目前是互動還是子命令模式。`--unsafe=...` 的結果**不在**這個 struct 裡，而是寫進全域的 `bench_unsafe_mask`（`main.c:73`），由各處的 `UNSAFE(R_...)` 巨集讀取，所以沒有 `unsafe` 欄位。
- **細節**（編譯後 `sizeof == 344`；offset：`host` 0、`port` 256、`name` 258、`replay` 296、`duration` 320、`out` 328、`wait_ms` 336、`raw` 340，`port` 與 `name` 後面、`verbose` 後面各有編譯器補的對齊空洞）：
  - `host[256]`、`port`：由 `parse_hostport(argv[ai], o->host, sizeof o->host, &o->port, BN_DEFAULT_PORT)` 填（`main.c:88`），預設埠 `BN_DEFAULT_PORT` = 9000。`conn_connect()` 把兩者交給 `tcp_connect_timeout()`。
  - `name[BN_NAME_LEN]`（32 位元組）：長度刻意等於 `struct bn_hello` 的 `name[BN_NAME_LEN]`，`conn_connect()` 以 `bench_strlcpy(h.name, o->name, sizeof h.name)` 複製。來源是 `--name`，沒給時 `main.c:98` 組成 `"%.15s@%.15s"`（`$USER@hostname`，最長 15+1+15 = 31 字元加 NUL，剛好放得下），最後 `sanitize_name()` 把不可列印位元組換成 `?`、去掉頭尾空白，若結果為空則改成 `anon`。
  - `replay`：加入時要求伺服器重播多少位元組的歷史。`main()` 預設 `8 * 1024`；`-n` 以 `parse_size()` 解析（支援 K/M 後綴），並檢查不得超過 `0xFFFFFFFF`，因為 `bn_hello.replay_bytes` 只有 32 位元，`conn_connect()` 的參數也是 `uint32_t`。型別用 `uint64_t` 是為了讓 `parse_size()` 能偵測溢位。
  - `replay_set`：使用者有沒有明講 `-n`。因為不同子命令的「未指定」預設不同：`sub_tail` 用 16 KiB（`subcmd.c:137`），`sub_watch` 用 0（`subcmd.c:162`），互動模式永遠用 `replay`（預設 8 KiB）。只靠 `replay == 0` 無法區分「沒給」與「明確要 0」，所以另設旗標。
  - `reconnect`：預設 1；`--no-reconnect` 設 0。`session.c` 的 `app_disconnect()` 與 `try_connect()` 檢查它；在 `--unsafe=reconnect`（R12）時 `app_disconnect()` 無論此值為何都直接放棄（設 `quit = 2`）。
  - `verbose`：`-v` 每出現一次加 1，傳給 `bench_log_init_level()` 決定執行期 log 等級。
  - `duration`：`--duration SEC`，以 `atof` 解析的秒數；0（預設）代表 `bench watch` 不限時（`subcmd.c:174` 把結束時間設成 `UINT64_MAX`）。
  - `out`：`--out FILE` 的字串指標，**直接指向 `argv`**，不複製；行程存活期間有效。`bench watch` 用它決定是否 `fopen`。
  - `wait_ms`：`--wait MS`，`bench send` 送完後再等多久把裝置回應印出來；0 表示不等。
  - `raw`：`--raw`，讓 `bench info` 在終端機上也輸出原始 `key=value` 而不是框線表格（`subcmd.c:103`）。
  - 互動模式不使用 `duration`、`out`、`wait_ms`、`raw`。
- **關係**：被誰呼叫／使用 → 填寫：`client/main.c`（`main`）；讀取：`client/session.c`（`conn_connect`、`try_connect`、`app_disconnect`）、`client/subcmd.c`（`sub_info/sub_tail/sub_watch/sub_send`）、`struct app` 的 `o` 欄位。

### `struct conn`
*位置：`client/client.h:37`　簽章：`struct conn { int fd; uint64_t pos; int have_pos; uint64_t server_start_ns; int have_start; uint64_t last_rx_ms; uint64_t last_ping_ms; };`*

- **用途**：一條 TCP 連線的狀態，互動模式與子命令共用。除了 socket fd，還記著「我看到串流的哪個位置」與「我連的是哪一次啟動的伺服器」，供斷線重連時送出 resume 資訊。
- **設計**：把「連線」與「畫面」分開。`struct conn` 不含任何終端機狀態，所以 `subcmd.c` 的 `info/tail/watch/send` 不需要 `struct app` 也能用 `conn_connect/conn_recv/conn_send/conn_bye`。`pos` 與 `server_start_ns` 放在這裡而不是 `struct app`，是因為它們就是 HELLO 的 resume 欄位，由 `conn_connect()` 直接取用。
- **細節**（`sizeof == 56`）：
  - `fd`：連線中的 socket，**關閉後為 -1**。`conn_close()` 在 `close` 後把它設成 -1，`conn_bye()` 與 `conn_close()` 都以 `fd < 0` 判斷「已關」，因此重複呼叫是安全的。互動模式的 `struct app` 是 `calloc` 出來的（`fd` 初值 0 = stdin 的號碼），所以 `app_run()` 開頭必須明確 `a->c.fd = -1`（`session.c:335`）；子命令的 `open_session()` 則是 `memset` 後再設 -1。
  - `pos`、`have_pos`：`pos` 是**最後一筆收到的 record 結尾的串流偏移**（DECISIONS：DATA/EVENT 的 `offset` 是 record 結束處）。`handle_packet()` 在收到 REPLAY_BEGIN（`from_offset`）、REPLAY_END（`head`）、DATA（`offset`）、EVENT（`offset`）時更新並設 `have_pos = 1`。`have_pos` 區分「從未收過東西」與「偏移剛好是 0」。
  - `server_start_ns`、`have_start`：來自 WELCOME 的 `server_start_ns`（轉成主機序）。`conn_wait_welcome()`（子命令）與 `handle_welcome()`（互動）都會寫入。重連時若新 WELCOME 的值和舊的不同，`handle_welcome()` 判定「伺服器重啟」，把 `have_pos` 清 0，因為舊偏移在新的 ring 裡已無意義。
  - 兩組旗標都成立時 `conn_connect()` 才在 HELLO 設 `has_resume = 1` 並帶上 `resume_offset = pos`、`last_server_start_ns = server_start_ns`（`session.c:38-42`）。子命令每次都 `memset` 歸零，所以永遠不 resume。
  - `last_rx_ms`：最後一次**成功收到一個完整封包**的單調時鐘毫秒（`mono_ms()`）。`conn_recv()` 回傳 1 時更新，`conn_connect()` 也先設成現在。`app_run()` 以「距上次收包超過 15000 ms」判定伺服器無回應並斷線（`session.c:409`）；`sub_watch` 同理（`subcmd.c:195`）。用單調時鐘是為了不受系統時間調整影響。
  - `last_ping_ms`：最後一次送 PING 的時間，`app_run()` 每 15000 ms 送一次（`session.c:411`）；`conn_connect()` 重設為現在。`sub_watch` 用自己的區域變數 `last_ping`，不使用這欄。
- **關係**：被誰呼叫／使用 → `client/session.c`（`conn_*` 系列、`handle_packet`、`app_run`）、`client/subcmd.c`（`open_session`、`sub_*`）；內嵌在 `struct app` 的 `c` 欄位。

### `ESC_* states`
*位置：`client/client.h:55`　簽章：`enum { ESC_NONE, ESC_GOT_ESC, ESC_CSI, ESC_STRING, ESC_STRING_ESC, ESC_ONE_MORE };`*

- **用途**：裝置輸出的 ANSI 過濾器（`ui.c` 的 `esc_byte()`）的狀態機狀態，存在 `a->esc_state`。值依序為 0、1、2、3、4、5。
- **設計**：狀態放在 `struct app` 而不是 `esc_byte()` 的區域變數，是因為一個 escape 序列可能被切在兩筆 DATA record 之間（DECISIONS「Client」一節）。**`ESC_NONE` 必須是 0**：`struct app` 由 `calloc` 配置所以起始就是正常狀態，`ui_data()` 也直接用 `if (a->esc_state)` 判斷「正在序列中」。這是匿名 enum，沒有型別，`esc_state` 因此宣告為 `int`。
- **細節**：
  - `ESC_NONE`：一般文字。`ui_data()` 遇到 0x1B 時設 `ESC_GOT_ESC` 並把 `esc_len` 歸零。
  - `ESC_GOT_ESC`：剛收到 ESC。下一個位元組若是 `[` 進 `ESC_CSI`；`]`、`P`、`_`、`^`、`X` 進 `ESC_STRING`；`(`、`)`、`*`、`+`、`#`、`%` 進 `ESC_ONE_MORE`；其他（`ESC 7`、`ESC 8`、`ESC c`、`ESC M`、`ESC =` 等）直接丟棄回 `ESC_NONE`。
  - `ESC_CSI`：在 `ESC [` 之後，參數位元組累積進 `esc_buf`；遇到 0x40–0x7E 的終止字元時決定放行、改寫或丟棄（只放行 `mKABCDGPX@`，`J` 改寫成 `ESC[K`，開頭是 `? > < =` 的私有序列丟棄）；遇到控制字元或 `esc_len` 已滿則整段丟棄。
  - `ESC_STRING`：OSC/DCS/APC/PM/SOS 字串，一直吞到 BEL（0x07）或 `ESC \`。
  - `ESC_STRING_ESC`：在字串中看到 ESC，等下一個位元組；若是 `\` 結束，否則回到 `ESC_STRING`。
  - `ESC_ONE_MORE`：字元集指定這類序列，再吞一個位元組就結束。
- **關係**：被誰呼叫 → 只由 `client/ui.c` 的 `esc_byte`、`ui_data` 讀寫；`struct app` 的 `esc_state` 欄位持有。

### `ST_* states`
*位置：`client/client.h:56`　簽章：`enum { ST_DISCONNECTED, ST_HANDSHAKE, ST_LIVE };`*

- **用途**：互動模式連線生命週期的三個狀態，存在 `a->st`。值為 0、1、2。
- **設計**：`ST_DISCONNECTED` 是 0，所以 `calloc` 之後的 `struct app` 天生就是「未連線」。把 `HANDSHAKE`（TCP 已連上、HELLO 已送出、尚未收到 WELCOME）與 `LIVE` 分開，是因為只有 `LIVE` 才應該送使用者按鍵與命令，但 `HANDSHAKE` 期間仍要 `poll` socket 等 WELCOME，也要讓狀態列顯示 CONNECTING。
- **細節**：
  - 轉換：`app_run()` 開頭設 `ST_DISCONNECTED`；`try_connect()` 在 `conn_connect()` 成功後設 `ST_HANDSHAKE`；`handle_welcome()` 收到 WELCOME 後設 `ST_LIVE`；`app_disconnect()` 一律回 `ST_DISCONNECTED`。`try_connect()` 在連線失敗時會先暫設 `ST_HANDSHAKE` 再呼叫 `app_disconnect()`，註解寫明是為了讓它排定退避重試，而不是顯示「連線中斷」提示（`was_live` 為假）。
  - 使用處：`app_send()` 在 `ST_DISCONNECTED` 回 -1；`app_flush_input()` 只在 `ST_LIVE` 送 INPUT；`cmd.c` 的 `send_simple()` 與 `cmd_keys()` 要求 `ST_LIVE`，否則 flash "not connected"；`app_run()` 在 `st != ST_DISCONNECTED` 才把 socket 放進 `poll`；`ui_status()` 在 `st != ST_LIVE` 顯示黃色 CONNECTING / RECONNECTING 徽章，`st == ST_HANDSHAKE` 或從未成功過時不顯示倒數秒數。
  - 結束時只有 `st != ST_DISCONNECTED` 才送 BYE（`session.c:425`）。
- **關係**：被誰呼叫 → `client/session.c`、`client/ui.c`、`client/cmd.c` 讀寫 `a->st`。

### `struct app`
*位置：`client/client.h:58`　簽章：`struct app { struct client_opts o; struct conn c; int st; int quit; ... uint64_t retry_at; };`*

- **用途**：互動模式的全部狀態，一個行程一個實例。`main()` 以 `calloc(1, sizeof *a)` 從堆積配置（`main.c:40`，`sizeof == 1408`），所以所有欄位起始為 0；之後每個 `.c` 檔都以 `struct app *a` 為第一個參數操作它。
- **設計**：不使用全域變數而把狀態集中成一個 struct，讓 `ui.c`、`cmd.c`、`session.c` 的函式都是「對 `a` 做事」的形式，關聯清楚、各檔案可獨立閱讀。程式是單行程單執行緒（一個 `poll()` 迴圈），所以沒有鎖，也不存在別人同時改這些欄位。一律「零值即合理初始狀態」：`ST_DISCONNECTED`、`ESC_NONE`、`retry_at = 0`（立刻嘗試連線）、`quit = 0`。
- **細節**（欄位依 struct 內的分組）：
  - **核心**：`o`（命令列選項的整份複本）、`c`（見 `struct conn`）、`st`（`ST_*`）。
  - **`quit`**：0 繼續；1 使用者要求結束（`:quit`、`Ctrl-] q`）；2 失敗結束（斷線且不重連、或首次連線失敗且 `--no-reconnect`）。`app_run()` 結束時 `return a->quit == 2 ? 1 : 0`，所以 2 對應行程 exit code 1。
  - **`ever_welcomed`**：是否至少成功收過一次 WELCOME。決定 (1) 首次連線失敗時要不要放棄（`!ever_welcomed && !o.reconnect`）；(2) 收到 WELCOME 時要不要畫「reconnected / server restarted」分隔線；(3) 狀態列徽章用 CONNECTING 還是 RECONNECTING。
  - **`in_replay`**：REPLAY_BEGIN 設 1、REPLAY_END 設 0。目前程式裡**只有寫入、沒有任何讀取**，是保留的狀態旗標，不影響行為。
  - **`info_pending`**：`cmd_exec` 的 `info` 在送 `BN_INFO_REQ` 前設 1；`handle_packet` 收到 `BN_INFO` 時只有它為 1 才畫資訊框，並一律清 0。這樣不會因為其他來源的 INFO 封包而彈出框。
  - **伺服器告知的狀態**（全部已轉為主機位元組序）：`dev_state`（`DEV_UP`/`DEV_LOST`，來自 WELCOME、STATUS，以及 EVENT 的 `EV_DEVICE_LOST/EV_DEVICE_BACK`）；`is_ctrl`（是否持有控制權；CTRL_RESULT 授權時設 1，STATUS 的 `you_are_ctrl` 之後會直接覆寫它，`app_disconnect` 清 0）；`baud`、`viewers`、`ctrl_idle`（控制者閒置秒數）、`rx_bytes`（伺服器收自 UART 的累計位元組，僅由 STATUS 更新）；`ctrl_name[BN_NAME_LEN]`（目前控制者名稱，32 位元組）、`dev_path[128]`（128 與 `struct bn_welcome` 的 `dev_path[128]` 相同）。`app_run()` 開頭先把 `dev_state` 設 `DEV_UP`、`dev_path` 設 `"?"`，在收到 WELCOME 前狀態列才有東西可顯示。
  - **終端機與畫面**：`rows`、`cols`（`term_size()` 取得，`ui_init/ui_resize` 更新）；`ts_mode`、`hex_mode`（`:ts`、`:hex` 切換的顯示模式）；`at_bol`（游標是否在行首，決定分隔線/提示框/時間戳前要不要先補換行）；`logf`、`logname[256]`（`Ctrl-] l` 或 `:log` 的錄製檔，`handle_packet` 對每筆 DATA 把原始位元組 `fwrite` 進去，以 `"ab"` 附加模式開啟，結束時 `fclose`）。
  - **跳脫鍵與命令列**：`esc_pending`（已按 Ctrl-]，等命令鍵）；`cmd_mode`、`cmd[128]`、`cmd_len`（`:` 命令列編輯緩衝，`cmdline_key` 只收可列印 ASCII 且 `cmd_len < sizeof cmd - 1`）。
  - **ANSI 過濾**：`esc_state`、`esc_buf[32]`、`esc_len`（見 `ESC_* states`）。放在 `app` 內是為了讓跨 DATA record 的序列可以接續解析；`esc_buf` 暫存 CSI 參數，滿 32 位元組即丟棄整段。
  - **狀態列訊息**：`flash[200]`（暫時訊息，`ui_flash` 用 `vsnprintf` 寫入）、`flash_color`（`FL_*`）、`flash_until`（`mono_ms()` 的到期時間，錯誤與警告 4000 ms，其餘 2500 ms）、`last_status_draw`（上次畫狀態列的時間；`app_run` 每 1000 ms 重畫一次，讓 idle 秒數與重連倒數會動，也在 flash 到期時重畫）。
  - **輸入批次**（註解標 §5.4）：`ibuf[64]`、`ilen`、`iflush_at`。控制者的按鍵不逐鍵送，而是累積到 `ibuf`；第一個位元組進來時設 `iflush_at = now + 5`（毫秒），滿 64 位元組立即 `app_flush_input()`，否則 `app_run` 把 `poll` 逾時縮短到 `iflush_at` 後送出。這樣貼上大段文字只產生少量 INPUT 封包，單包也遠小於 `BN_MAX_INPUT`（256）。斷線（`app_disconnect`）與失去控制權（STATUS 顯示 `was_ctrl && !is_ctrl`）時 `ilen` 會被清 0，避免把過期的按鍵在之後補送。
  - **重連**：`backoff_idx` 是 `session.c` 內 `backoff_s[] = {1, 2, 4, 8, 16, 30}`（秒）的索引，讀取時以 5 為上限（所以之後固定 30 秒），遞增到 100 為止避免溢位，收到 WELCOME 時歸 0；`retry_at` 是下次允許重連的 `mono_ms()` 時間，初值 0 代表啟動時立刻連線。
  - 寫入者/讀取者的對應：`session.c` 主要寫連線與伺服器狀態，`cmd.c` 主要寫 `ts_mode/hex_mode/logf/cmd_*/ibuf`，`ui.c` 主要讀畫面欄位並寫 `at_bol/esc_*/flash_*/last_status_draw/rows/cols`，彼此都在同一個行程、同一條執行緒，不需要同步。
- **關係**：被誰呼叫／使用 → 由 `client/main.c` 配置並傳給 `app_run`；`client/session.c`、`client/ui.c`、`client/cmd.c` 以 `struct app *a` 操作；`client/term.c` 與 `client/subcmd.c` 不使用此 struct。

### `FL_* flash colors`
*位置：`client/client.h:117`　簽章：`enum { FL_INFO, FL_OK, FL_WARN, FL_ERR };`*

- **用途**：狀態列暫時訊息（flash）的嚴重程度，傳給 `ui_flash()` 的 `color` 參數，並存在 `a->flash_color`。值為 0、1、2、3。
- **設計**：用語意名稱而不是直接傳 ANSI 色碼，讓呼叫端（`cmd.c`、`session.c`）不需要知道顏色細節；顏色對照集中在 `ui_status()`。
- **細節**：
  - 顏色（`ui.c:423-425`）：`FL_ERR` 亮紅、`FL_WARN` 亮黃、`FL_OK` 亮綠，其餘（`FL_INFO`）亮青，皆為 256 色背景 236 的底。
  - 顯示時間（`ui.c:383`）：`FL_ERR` 與 `FL_WARN` 4000 ms，`FL_OK` 與 `FL_INFO` 2500 ms；錯誤需要更久才讀得完。
  - `session.c` 在收到 `BN_ERROR` 時，錯誤碼為 `BN_ERR_NOT_CONTROLLER` 用 `FL_WARN`，其他用 `FL_ERR`（`session.c:320`）。
- **關係**：被誰呼叫／使用 → `client/ui.c`（`ui_flash`、`ui_status`）、`client/cmd.c`、`client/session.c`。

### `函式原型一覽`
以下函式在這個標頭檔只宣告原型，設計與用途寫在實作它們的 `.c` 章節，這裡不重複：

- `conn_connect` → client-session
- `conn_recv` → client-session
- `conn_send` → client-session
- `conn_close` → client-session
- `conn_bye` → client-session
- `conn_wait_welcome` → client-session
- `app_run` → client-session
- `app_send` → client-session
- `app_flush_input` → client-session
- `term_init` → client-term
- `term_restore` → client-term
- `term_size` → client-term
- `term_set_region` → client-term
- `ob_put` → client-term
- `ob_str` → client-term
- `ob_printf` → client-term
- `ob_flush` → client-term
- `ui_init` → client-ui
- `ui_resize` → client-ui
- `ui_data` → client-ui
- `ui_separator` → client-ui
- `ui_note` → client-ui
- `ui_info_box` → client-ui
- `ui_help` → client-ui
- `ui_status` → client-ui
- `ui_flash` → client-ui
- `ui_event` → client-ui
- `str_width` → client-ui
- `info_box_lines` → client-ui
- `cmd_keys` → client-cmd
- `cmd_exec` → client-cmd
- `log_toggle` → client-cmd
- `sub_info` → client-subcmd
- `sub_tail` → client-subcmd
- `sub_watch` → client-subcmd
- `sub_send` → client-subcmd

---

## `lib/crc32.c`

這個檔案實作封包 CRC32：IEEE 802.3 多項式，bit 反射形式，常數 `0xEDB88320`，演算法與 zlib 的 `crc32()` 相同，所以 Wireshark 與 Python `zlib.crc32` 算出的值和這裡一致（檔頭註解 1-4 行）。它不屬於任何單一行程，而是編進 `libbench.so.1`，由 `bn_send()`/`bn_recv()` 與 `tools/bench-evil.c` 呼叫：master 行程只在拒絕連線時經 `bn_send_error()` 間接用到（`server/main.c:128`、`:178`），per-client child 在 `server/session.c`、bench client 在 `client/session.c`、`bench-evil` 工具則在 `tools/bench-evil.c` 各自在自己的行程內呼叫；`server/reader.c` 沒有呼叫 `bn_send()`/`bn_recv()`/`bench_crc32()`。每個行程擁有自己的一份 `crc_table`。對外只提供 `bench_crc32()`（宣告在 `include/bench_proto.h:129`），內部靠 `static` 的 `crc_init()` 建表。只依賴 `bench_proto.h` 引入的 `<stdint.h>`/`<stddef.h>` 型別（該標頭第 18-19 行），不使用 log、鎖或任何 I/O。

### `crc_init`
*位置：`lib/crc32.c:10`　簽章：`static void crc_init(void)`*

- **用途**：建立 256 項的查表 `crc_table`，每項是「該位元組值（0-255）單獨餵進 CRC 暫存器 8 個 bit 後的結果」，建完把 `crc_ready` 設為 1。
- **設計**：用查表法（每個輸入位元組只查一次表、做一次 XOR 與一次位移）取代逐 bit 計算，封包 payload 最大到 `BN_MAX_PAYLOAD`（65536 位元組），每個非空 payload 的封包收發都要算 CRC，逐 bit 做法每個位元組要多做 8 次迭代。表在執行期由本函式計算，而不是把 256 個常數寫死在原始碼，所以 C 端的多項式只出現在第 15 行（Wireshark dissector `tools/benchd.lua` 另有一份）。使用反射形式（右移，遇到最低位為 1 就 XOR `0xEDB88320u`），因為這是 IEEE/zlib 的標準做法，資料可以直接以位元組順序（LSB first）處理，不用另外做 bit 反轉。
- **細節**：
  - 外層 `i` 從 0 到 255，內層 `k` 做 8 次：`c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1`（第 12-17 行，XOR 在第 15 行）。
  - 只寫入 `crc_table` 與 `crc_ready` 兩個檔案內 static 變數，沒有回傳值、不會失敗、不碰 errno，也不呼叫任何 libc 函式。
  - 沒有鎖也沒有 atomic：程式全程不使用執行緒（見 docs/DECISIONS.md「No threads anywhere」），所以不會有兩個 thread 同時初始化的問題。fork 之後每個行程各自持有表的副本；若某個行程在 fork 前還沒呼叫過 `bench_crc32()`，就會在自己第一次呼叫時各自建表，結果相同。
  - 它寫入 static 狀態且沒有保證 async-signal-safe；實際上只有 `bench_crc32()` 呼叫它（grep 全 repo 確認），而 `bench_crc32()` 的呼叫者 `bn_send()`/`bn_recv()` 是一般流程，不在 signal handler 內。
  - 不受任何 `--unsafe=<switch>` 影響。
- **關係**：被誰呼叫 → 只有同檔的 `bench_crc32()`（`lib/crc32.c:26`，第一次呼叫時）；呼叫 → 無。

### `bench_crc32`
*位置：`lib/crc32.c:21`　簽章：`uint32_t bench_crc32(const void *data, size_t len)`*

- **用途**：回傳 `data` 開頭 `len` 個位元組的 CRC32 值（初值 `0xFFFFFFFF`、最後再與 `0xFFFFFFFF` XOR，即標準 CRC-32/ISO-HDLC）。這個值就是 wire protocol 標頭中 `crc32` 欄位（見 `include/bench_proto.h:8` 的封包圖與 `:61` 的 `crc32` 欄位）。
- **設計**：
  - 以「懶初始化」方式建表：第一次呼叫時檢查 `crc_ready`，為 0 才呼叫 `crc_init()`。這樣庫不需要額外的 `init()` 函式，所有使用 `libbench.so` 的程式（server、client、`bench-evil`）不必記得先初始化。之後每次呼叫只多一次旗標判斷。
  - 主迴圈 `c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8)`（第 27-28 行）是標準的 byte-at-a-time 反射算法：低 8 位與輸入位元組 XOR 後當索引，再把暫存器右移 8 位並 XOR 表值。`while (len--)` 搭配 `*p++` 不需要索引變數。
  - 參數用 `const void *` 讓呼叫端可直接傳 `struct`、`uint8_t` 緩衝區或字串，內部再轉成 `const uint8_t *`，確保以「無號位元組」運算，不會因 `char` 帶號而在 `c ^ *p` 擴展符號。
  - 輸入和輸出結果是 host 端的 `uint32_t` 數值；轉成網路位元組序（big-endian）是呼叫端的工作，不在這裡做（`bn_hdr_encode()` 內用 `htonl()`，見 `lib/proto.c:21`）。
- **細節**：
  - 沒有錯誤回傳。`len == 0` 時迴圈不執行，結果是 `0xFFFFFFFF ^ 0xFFFFFFFF = 0`。協定本身也約定空 payload 的 crc 為 0：`bn_send()` 與 `bn_recv()` 都寫成 `len ? bench_crc32(...) : 0`，所以實際上 `len == 0` 時根本不會呼叫它。
  - `data` 為 NULL 且 `len > 0` 會直接解參考而當機；呼叫端（`bn_send()`、`bn_recv()`）只在 `len` 非 0 時傳入有效緩衝區。
  - 標準測試向量：ASCII `"123456789"` 的結果是 `0xCBF43926`（與 zlib 相同）。
  - 沒有輸入長度上限，`len` 為 `size_t`；實際最大輸入是 `BN_MAX_PAYLOAD` = 65536（發送端由 `bn_send()` 在 `lib/proto.c:48` 檢查，超過回 -1 並設 `errno = EMSGSIZE`；接收端在 `lib/proto.c:110` 檢查 `h->len > BN_MAX_PAYLOAD || h->len > cap`，超過就設 `*perr = BN_ERR_BAD_LENGTH` 並回 -2；這個檢查同樣在 `if (!UNSAFE(R_VALIDATE))` 區塊內）。
  - 與 `--unsafe` 的關係：`--unsafe=validate`（R3，`R_VALIDATE`）時，`bn_recv()` 跳過整個 CRC 檢查區塊（`lib/proto.c:128-134` 的 `if (!UNSAFE(R_VALIDATE))` 區塊），所以接收端不再呼叫本函式，錯誤的 CRC 會被放行；發送端 `bn_send()` 仍然呼叫它。安全模式下 CRC 不符會讓 `bn_recv()` 設 `*perr = BN_ERR_BAD_CRC` 並回 -2。`scripts/test.sh` 與 `scripts/demo_r3.sh` 用 `bench-evil bad-crc` 驗證這條路徑。
  - 同一演算法在 Wireshark dissector `tools/benchd.lua:69-84` 以 Lua 重寫一份（同樣的表與迴圈），用來標示 "CRC32 valid"（欄位 `f.crc_ok`，第 36、185 行；`len == 0` 時要求 CRC 欄位為 0，與 C 端一致），與這個 C 版本必須保持一致。
- **關係**：被誰呼叫 → `lib/proto.c` 的 `bn_send()`（第 52 行，算送出 payload 的 CRC）與 `bn_recv()`（第 129 行，驗證收到的 payload）；`tools/bench-evil.c:51`（`build_packet()` 組封包時算 CRC，`bad-crc` 模式再把 CRC 欄位 `pkt[8] ^= 0xFF` 弄壞）。呼叫 → 同檔 `crc_init()`（第一次呼叫時）。

---

## `lib/log.c`

`lib/log.c` 是兩層式 log（`docs/DECISIONS.md` 的 Logging (§7)，與 `lib/log.c` 檔頭註解同一個編號）中「執行期」的那一半；編譯期那一半是 `include/bench_log.h` 的 `LOGE/LOGW/LOGI/LOGD/LOGT` 巨集（高於 `LOG_COMPILE_LEVEL` 的訊息連格式字串都不會進 binary）。這個檔案編進 `libbench.so.1`，因此每一個行程都會用到：master（`server/main.c`）、reader（`server/reader.c`）、每個 client 的 child（`server/session.c`）、`bench` 客戶端（`client/main.c`）、以及 `benchsim`、`bench-evil` 兩個工具。它提供的東西有三類：目前的 log 等級（可以改指到共享記憶體，讓所有 fork 出來的行程共用同一個值）、等級名稱與字串之間的轉換、以及把一行 log 組好後以單一 `write()` 輸出到 stderr 與（可選的）檔案的 `bench_log_write`。它依賴 `lib/util.c` 的 `now_ns()`、`fmt_clock()`（`include/bench_util.h`）。SIGUSR1 循環切換等級的邏輯不在這裡，而是在 `server/main.c` 的 `cycle_log_level()`（第 199 行），它直接寫 `g_shm->log_level`；本檔案只負責讓每個行程「讀」到這個共享值。`cycle_log_level()` 以查表 `next[]` 決定下一級，順序為 ERROR → WARN → INFO → DEBUG → TRACE → ERROR（與 `docs/DECISIONS.md` 所寫的 INFO → DEBUG → TRACE → ERROR → WARN → INFO 是同一個環）；目前值若不在 0..4 則視為 INFO。切換時用 `LOGW` 回報，所以切到 ERROR 時這行回報本身會因 WARN 高於 ERROR 而不輸出。SIGUSR1 的處理常式 `on_usr1` 只設旗標，主迴圈（`server/main.c:413-415`）才呼叫 `cycle_log_level()`，原因見 `bench_log_write` 的 async-signal-safe 說明。

檔案內有五個 file-scope 的 static 狀態：`private_level`（行程私有的等級，預設 `LOG_INFO`，第 21 行）、`level_ptr`（指向目前生效的等級，預設指向 `private_level`，第 22 行，型別 `volatile int *`）、`role[32]`（每行 log 裡的行程標籤，預設 `"main"`，第 23 行）、`file_fd`（額外的 log 檔 fd，預設 -1，第 24 行）、`to_stderr`（是否輸出到 stderr，預設 1，第 25 行）。另有 `names[]`（`"ERROR"`、`"WARN"`、`"INFO"`、`"DEBUG"`、`"TRACE"`，第 27 行），索引剛好等於 `LOG_ERROR..LOG_TRACE` 的列舉值 0..4。這些都是行程內的全域變數，fork 之後各行程各自擁有一份複本（只有 `level_ptr` 所指的共享記憶體是真正共用的）。整個 log 模組沒有任何 `--unsafe=` 開關（R1-R12 皆與它無關）。

### `bench_log_set_role`
*位置：`lib/log.c:29`　簽章：`void bench_log_set_role(const char *fmt, ...)`*

- **用途**：設定出現在每一行 log 中 `[role]` 位置的行程標籤，格式由 printf 風格的 `fmt` 決定。
- **設計**：多個行程（master、reader、各個 child）共用同一個 stderr 或 log 檔，沒有標籤就分不出一行訊息是誰寫的。標籤用 printf 格式而不是純字串，是因為 child 需要把自己的 pid 放進去（`"child:%d"`）。標籤存在 static 陣列 `role`，master 在 `main` 啟動時呼叫，reader 與 child 在各自入口呼叫（fork 後各行程有自己的 `role` 複本），因此各行程互不影響。header 中以 `__attribute__((format(printf, 1, 2)))` 讓編譯器檢查格式與參數。
- **細節**：
  - 使用 `vsnprintf(role, sizeof role, ...)`，`role` 為 32 bytes，過長會被截斷並保證以 NUL 結尾。
  - 預設值是 `"main"`；沒有呼叫過就會以 `main` 出現。
  - 沒有回傳值，也不會檢查 `vsnprintf` 的錯誤。
  - 不是 async-signal-safe（`vsnprintf`），也沒有鎖；只在行程啟動時於單一執行緒呼叫。
- **關係**：被誰呼叫 → `server/main.c`（`"master"`，第 330 行）、`server/reader.c`（`"reader"`，第 205 行）、`server/session.c`（`"child:%d"`，第 429 行）、`client/main.c`（`"bench"`，第 102 行）、`tools/benchsim.c`（`"benchsim"`，第 414 行）、`tools/bench-evil.c`（`"evil"`，第 479 行）。被 `bench_log_write` 間接讀取 `role`。

### `bench_log_set_level_ptr`
*位置：`lib/log.c:37`　簽章：`void bench_log_set_level_ptr(volatile int *p)`*

- **用途**：把「目前 log 等級」的儲存位置改指到呼叫者提供的整數（例如共享記憶體中的欄位）；傳入 `NULL` 則改回行程私有的 `private_level`。
- **設計**：這是「SIGUSR1 一次改變所有行程等級」的關鍵。master 在 `mmap(MAP_SHARED)` 建好共享記憶體之後、任何 `fork()` 之前呼叫 `bench_log_set_level_ptr(&g_shm->log_level)`（`server/main.c:360`），之後 fork 出來的 reader 與 child 繼承了 `level_ptr` 這個指標值，而它指向的記憶體是共用的；因此 master 只要寫一次 `g_shm->log_level`，所有行程下一次呼叫 `bench_log_enabled()` 時就讀到新值，不需要任何行程間通知。切換前先把「目前等級」複製到新位置（`*p = *level_ptr`），讓在這之前由 `bench_log_init_level()` 依 `-v` 與 `BENCH_LOG` 算出的等級不會因為換成共享欄位（初始為 0，即 `LOG_ERROR`）而消失。設計時刻意讓 `NULL` 也有定義，以便恢復為私有等級，並且恢復時同樣把當前值帶回 `private_level`。
- **細節**：
  - 傳入非 NULL 時：`*p = *level_ptr; level_ptr = p;`，順序不可顛倒，否則會讀到新位置的舊內容。
  - 傳入 NULL 時：`private_level = *level_ptr; level_ptr = &private_level;`，在共享記憶體失效之前呼叫才安全。
  - 呼叫端必須保證 `p` 指向的記憶體在往後所有 log 呼叫期間都有效（master 只在 `shutdown_server()` 的最後一步、`LOGI("bye")` 之後才 `munmap(g_shm, ...)`，見 `server/main.c:266-267`，之後 `main` 直接 `return 0`，不再有 log 呼叫）。
  - 目前 repo 中只有 `server/main.c:360` 以非 NULL 呼叫；`NULL` 路徑沒有被任何程式碼使用。
  - 為什麼 reader 與 child 不必重新呼叫它：`fork()` 複製的 `level_ptr` 值就是同一個虛擬位址，而 `MAP_SHARED` 對應到同一塊實體頁面；若改在 fork 之後才呼叫，先前已存在的行程就看不到共享欄位。
  - 沒有鎖與原子操作：等級只是一個 `int`，單次讀寫在支援的平台上不會撕裂；讀到舊值或新值對 log 而言都無害。
- **關係**：被誰呼叫 → `server/main.c`（`main`，第 360 行）。被間接使用 → 所有 `bench_log_get_level/set_level/enabled/init_level`（都經由 `level_ptr`）。

### `bench_log_set_level`
*位置：`lib/log.c:48`　簽章：`void bench_log_set_level(int lvl)`*

- **用途**：把目前生效的等級設成 `lvl`，並先把 `lvl` 夾在 `[LOG_ERROR, LOG_TRACE]` 範圍內。
- **設計**：等級會被當成 `names[]` 的索引，也會被 `lvl <= *level_ptr` 比較，所以不能讓任意的整數（例如 `-v` 次數太多或 `BENCH_LOG` 加上 `-v` 後超過 4）存進去；夾住比回報錯誤簡單，使用者多打幾個 `-v` 只是得到最詳細的 TRACE。寫入走 `*level_ptr`，因此如果已經 `bench_log_set_level_ptr()` 指向共享記憶體，這個函式改的就是全體行程共用的值。
- **細節**：
  - 小於 0 變成 0（`LOG_ERROR`），大於 4 變成 4（`LOG_TRACE`）。
  - 不會與 `LOG_COMPILE_LEVEL` 比較：就算設成 TRACE，release 組建（`LOG_COMPILE_LEVEL=2`）中的 `LOGD/LOGT` 也早已被編譯掉，只是不會產生輸出。
  - 沒有回傳值。
- **關係**：被誰呼叫 → `bench_log_init_level`（同檔案，第 87 行）。repo 中其他檔案沒有直接呼叫它（SIGUSR1 的路徑是 `server/main.c` 的 `cycle_log_level()` 直接寫 `g_shm->log_level`，那裡自己保證值在 0..4）。

### `bench_log_get_level`
*位置：`lib/log.c:57`　簽章：`int bench_log_get_level(void)`*

- **用途**：回傳目前生效的 log 等級（0..4）。
- **設計**：一行函式，只是對 `level_ptr` 解參考；存在的目的是讓呼叫端不必知道等級是存在私有變數還是共享記憶體。
- **細節**：
  - 回傳的是 `*level_ptr` 的當下值，所以在 master 使用共享等級後，這個值會隨 SIGUSR1 改變。
  - 不做夾限（寫入端已保證範圍）。
  - 目前 repo 中沒有其他檔案呼叫它（`grep` 無結果）；`server/sysinfo.c` 是直接讀 `g_shm->log_level` 再用 `bench_log_level_name` 轉成字串。
- **關係**：被誰呼叫 → 目前無外部呼叫者。呼叫 → 無。

### `bench_log_compile_level`
*位置：`lib/log.c:58`　簽章：`int bench_log_compile_level(void)`*

- **用途**：回傳 `libbench` 在編譯時使用的 `LOG_COMPILE_LEVEL`，也就是這個組建中最詳細還有程式碼存在的等級。
- **設計**：`LOG_COMPILE_LEVEL` 是前置處理器巨集，只在編譯單元內可見；包成函式後，`server/main.c` 的 `cycle_log_level()` 才能在執行期比較「SIGUSR1 要切到的等級」與「這個 binary 實際有的訊息」。注意這個值來自 `lib/log.c` 自己的編譯參數，而 Makefile 對 lib 與執行檔使用同一組 `MODE_CFLAGS`（release 為 `-DLOG_COMPILE_LEVEL=2`，debug 為 `-DLOG_COMPILE_LEVEL=4`），所以兩者一致。
- **細節**：
  - 沒有加 `-DLOG_COMPILE_LEVEL` 時由 `include/bench_log.h` 預設為 2。
  - 回傳值是編譯常數。lib 與各執行檔都吃 Makefile 的 `CFLAGS += $(MODE_CFLAGS)`（第 23 行），所以巨集端（執行檔內展開）與這個函式（`libbench` 內）回報的上限一致；若兩者不同，回報的「上限」就會和實際被編掉的訊息不符。
- **關係**：被誰呼叫 → `server/main.c` 的 `cycle_log_level()`（第 205、207 行）：若新等級大於此值，就在 WARN 訊息中附註「this build only contains messages up to ...」（見 `docs/DECISIONS.md` Logging 一節）。

### `bench_log_enabled`
*位置：`lib/log.c:59`　簽章：`int bench_log_enabled(int lvl)`*

- **用途**：回答「等級為 `lvl` 的訊息現在要不要輸出」，條件是 `lvl <= 目前等級`。
- **設計**：這是執行期那一層的過濾點，被 `BENCH_LOG_AT` 巨集放在 `bench_log_write` 之前呼叫（`include/bench_log.h`）：`if (bench_log_enabled(lvl)) bench_log_write(...)`。把檢查放在巨集裡而不是 `bench_log_write` 裡，是為了在訊息被關掉時不必評估格式化的參數，也不必進入 `bench_log_write`。因為每次呼叫都重新解參考 `level_ptr`（指標為 `volatile int *`），共享記憶體中的等級被改掉之後立即生效。
- **細節**：
  - 回傳 0 或 1。
  - 只看執行期等級，不看 `LOG_COMPILE_LEVEL`；編譯期已被移除的 `LOGD/LOGT` 根本不會走到這裡。
  - `LOGE` 永遠被編進去（巨集沒有 `#if`），所以 ERROR 在任何組建中都存在。
- **關係**：被誰呼叫 → 所有 `LOGE/LOGW/LOGI/LOGD/LOGT` 巨集展開（遍布 server、client、tools、lib）。

### `bench_log_level_name`
*位置：`lib/log.c:61`　簽章：`const char *bench_log_level_name(int lvl)`*

- **用途**：把等級數字轉成名稱字串（`"ERROR"`、`"WARN"`、`"INFO"`、`"DEBUG"`、`"TRACE"`），超出範圍回傳 `"?"`。
- **設計**：用陣列 `names[]` 以等級為索引查表，O(1) 且沒有配置；範圍檢查（`lvl >= 0 && lvl <= LOG_TRACE`）讓傳入異常值（例如 `server/sysinfo.c` 從共享記憶體讀出的 `st.log_level`）時不會越界讀取 `names[]`。回傳指向靜態字串，呼叫端不必釋放。
- **細節**：
  - `bench_log_write` 以 `%-5s` 輸出這個名稱，因此 `WARN`、`INFO` 會補成 5 格寬，欄位對齊。
  - 回傳值永遠非 NULL。
- **關係**：被誰呼叫 → `bench_log_write`（同檔案，第 110 行）、`server/main.c` 的 `cycle_log_level()`（第 206-209 行）、`server/sysinfo.c`（第 154 行，輸出 `server.log_level=...` 給 INFO_REQ）。

### `bench_log_level_from_str`
*位置：`lib/log.c:66`　簽章：`int bench_log_level_from_str(const char *s)`*

- **用途**：把字串解析成等級數字，接受 `"0".."4"`、名稱（不分大小寫，例如 `debug`、`INFO`）以及別名 `"warning"`；無法辨識時回傳 -1。
- **設計**：目的是讓環境變數 `BENCH_LOG` 可以寫成數字或名稱。以 -1 代表失敗，是因為合法等級是 0..4，-1 不會和合法值衝突，呼叫端用 `>= 0` 就能判斷。先處理「單一數字字元」的情形（`s[1] == 0` 確保不是 `"12"` 這種多位字串），再對 `names[]` 逐一用 `strcasecmp` 比對，最後才檢查額外的別名 `"warning"`。
- **細節**：
  - `NULL` 或空字串回傳 -1，所以 `getenv("BENCH_LOG")` 在變數不存在時可以直接傳入。
  - 數字只接受 `'0'` 到 `'4'` 單一字元；`"5"`、`"-1"`、`"10"` 都回傳 -1。
  - `strcasecmp` 來自 `<strings.h>`（第 18 行引入）。
  - 不修改字串，不配置記憶體。
- **關係**：被誰呼叫 → `bench_log_init_level`（第 83 行）。repo 中其他地方沒有呼叫。

### `bench_log_init_level`
*位置：`lib/log.c:80`　簽章：`int bench_log_init_level(int verbose_count)`*

- **用途**：在行程啟動時決定初始 log 等級並設定之，回傳最後生效的等級。順序是：從 `LOG_INFO` 開始，若環境變數 `BENCH_LOG` 合法就取代它，再加上 `-v` 的次數 `verbose_count`，最後夾限。
- **設計**：把「預設 → 環境變數 → 命令列」三層組合集中在一個函式，讓 master、`bench`、`benchsim`、`bench-evil` 的行為一致。`-v` 是「相對」的增量（每個 `-v` 往 DEBUG、TRACE 方向加 1），所以可以疊加在 `BENCH_LOG` 之上；加完交給 `bench_log_set_level` 夾限，使用者多打 `-v` 也不會越界。環境變數無法解析時（`bench_log_level_from_str` 回 -1）直接忽略，不報錯、維持 INFO。
- **細節**：
  - 回傳 `*level_ptr`，是夾限之後實際存下的值。
  - 在 master 中，這個函式在 `bench_log_set_level_ptr()` 之前呼叫（`server/main.c:331` 對 `:360`）；`set_level_ptr` 會把這裡算出的值複製進共享記憶體，所以 `-v` 與 `BENCH_LOG` 的效果會保留下來。
  - 只有加法，沒有 `-q` 之類往下降的選項；要降低等級要靠 `BENCH_LOG=warn` 之類，或執行期的 SIGUSR1。
  - 在 release 組建中即使得到 DEBUG/TRACE，沒有被編進去的訊息仍不會出現（見 `bench_log_compile_level`）。
- **關係**：被誰呼叫 → `server/main.c`（第 331 行，傳入 `verbose`）、`client/main.c`（第 103 行，傳入 `o->verbose`）、`tools/benchsim.c`（第 415 行）、`tools/bench-evil.c`（第 480 行，傳 0）。呼叫 → `bench_log_level_from_str`、`bench_log_set_level`。

### `bench_log_open_file`
*位置：`lib/log.c:91`　簽章：`int bench_log_open_file(const char *path)`*

- **用途**：以附加模式開啟 `path`，之後每一行 log 除了 stderr 之外還會再寫一份到這個檔案；成功回傳 0，失敗回傳 -1。
- **設計**：開啟旗標是 `O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC`，權限 0644。`O_APPEND` 讓核心在每次 `write()` 時原子地定位到檔尾，所以 master 先開檔、再 `fork()` 出 reader 與 child 時，這些行程共用同一個 open file description，卻不會互相覆寫對方的 log。`O_CLOEXEC` 避免 fd 被日後 `exec` 的程式繼承。先開新檔成功之後才關掉舊的 `file_fd`，這樣開檔失敗時舊的 log 目的地不會被破壞（呼叫端可以繼續用原本的檔案）。
- **細節**：
  - 失敗時回傳 -1，`errno` 保留 `open()` 的錯誤；函式本身不印訊息，由呼叫端處理（`server/main.c:332-335` 印出 `cannot open log file` 後回傳 1）。
  - 呼叫端 `client/main.c:119` 不檢查回傳值，緊接著 `bench_log_set_stderr(0)`；開檔失敗時互動模式的 log 會被靜默丟棄（TUI 優先於 log）。
  - 只會保留一個檔案：再呼叫一次會關掉前一個 fd 並換成新的。
  - 不會自動關閉 `to_stderr`；要停止 stderr 輸出要另外呼叫 `bench_log_set_stderr(0)`。
  - 檔案不會被 `fsync`，也沒有輪替機制。
- **關係**：被誰呼叫 → `server/main.c`（`-l <logfile>`，第 332 行）、`client/main.c`（互動模式下開 `/tmp/bench-<pid>.log`，第 119 行）。被 `bench_log_write` 間接使用（寫入 `file_fd`）。

### `bench_log_set_stderr`
*位置：`lib/log.c:102`　簽章：`void bench_log_set_stderr(int on)`*

- **用途**：開啟或關閉把 log 輸出到 stderr 的功能（非 0 為開）。
- **設計**：互動式的 `bench` 客戶端使用全螢幕 TUI，如果 log 行寫到 stderr 會破壞畫面，所以該模式改成只寫檔案（`client/main.c` 的註解 "keep the log away from the TUI"）。伺服器與非互動子命令保留預設的 stderr 輸出。
- **細節**：
  - 預設值是 1。
  - 只是設定一個旗標，沒有回傳值，不影響 `file_fd`。
  - 若同時 `bench_log_set_stderr(0)` 且沒有開啟檔案，log 會被完全丟棄。
- **關係**：被誰呼叫 → `client/main.c`（第 120 行，互動模式在 `bench_log_open_file` 之後呼叫）。被 `bench_log_write` 讀取。

### `bench_log_write`
*位置：`lib/log.c:104`　簽章：`void bench_log_write(int lvl, const char *file, int line, const char *fmt, ...)`*

- **用途**：把一行 log 格式化成 `[時:分:秒.毫秒] [LEVEL] [role] file:line  訊息\n`，再用一次 `write()` 送到 stderr（若 `to_stderr`）與 log 檔（若 `file_fd >= 0`）。
- **設計**：整行先組在堆疊上的 `buf[2048]`，再各自用單一 `write()` 輸出（檔頭註解：多個行程共用同一個 stderr 或 log 檔時，這樣各行不會在中途交錯；一般來說小於 `PIPE_BUF` 的單次 `write` 在管線上是原子的，檔案以 `O_APPEND` 也各自附加在尾端）。程式碼直接用 `write()` 而不經 stdio，因此沒有 `FILE` 緩衝。時間戳用 `fmt_clock(now_ns(), ...)`（`now_ns()` 取 `CLOCK_REALTIME`，`fmt_clock` 以 `localtime_r` 轉成本地時間，`HH:MM:SS.mmm`），因此這些行可以和 tcpdump / 封包擷取的時間對照（`docs/DECISIONS.md` 提到 TRACE 的封包標頭 log 是為了對照 tcpdump）。呼叫端一般透過巨集，巨集以 `bench_log_enabled(lvl)` 先過濾並傳入 `__FILE__`、`__LINE__`；本函式本身不再檢查等級。
- **細節**：
  - 開頭 `int saved = errno;`，結尾 `errno = saved;`：log 常常緊接在失敗的系統呼叫之後（例如 `LOGE("...: %s", strerror(errno))`），以及 `snprintf`/`write` 都可能改寫 `errno`，還原後呼叫端之後的 `errno` 判斷不會受影響。
  - 前綴格式為 `"[%s] [%-5s] [%s] %s:%d  "`（`file:line` 後面有兩個空白）。前綴的長度 `n` 沒有再夾限到 `sizeof buf`；實務上前綴很短（時間 12 字元、等級 5 字元、`role` 最多 31 字元、`__FILE__` 為相對路徑），所以不會逼近 2048。`snprintf` 回傳負值時 `n` 設為 0。
  - 訊息部分用 `vsnprintf(buf + n, sizeof buf - n - 1, ...)`，為結尾的換行保留位置；回傳負值時 `m` 設為 0。
  - 總長度 `len = n + m` 若超過 `sizeof buf - 2`（2046）就截斷成 2046，再補上 `'\n'`；也就是說單行最長 2047 bytes（含換行），超長訊息會被截斷而不是拆成多行。
  - `write()` 的回傳值被刻意忽略（`(void)w`）：log 失敗不能再引發 log，也沒有更好的處理方式；沒有重試，因此在 stderr 是非阻塞且已滿、或 EINTR 時，這一行可能被丟失或只寫出一部分。
  - 沒有鎖；依靠單次 `write` 的原子性，而不是互斥鎖。這也讓 `docs/DECISIONS.md` 所說「持有 shm 鎖時不做 log 與其他 I/O」的規則是呼叫端要遵守的，不是這個函式提供的。
  - 不是 async-signal-safe（`snprintf`、`localtime_r` 等），所以訊號處理常式只設旗標（例如 `server/main.c` 的 `on_usr1` 設 `got_usr1`），真正的 log 在主迴圈中輸出。
  - 沒有 `--unsafe=` 開關會改變這個函式的行為。
- **關係**：被誰呼叫 → `BENCH_LOG_AT` 巨集（`include/bench_log.h`）所展開的所有 `LOGE/LOGW/LOGI/LOGD/LOGT`。呼叫 → `now_ns()`、`fmt_clock()`（`lib/util.c`）、`bench_log_level_name`（同檔案）。

---

## `lib/net.c`

這個檔案是 `libbench.so.1` 裡的 socket 輔助層，宣告在 `include/bench_net.h`。它不屬於單一行程：server 端的 master（`handle_accept` 呼叫 `sock_peer_str`、`main` 呼叫 `tcp_listen`）、per-client child（`set_keepalive`、`set_io_timeouts`、經由 `bn_send`/`bn_recv` 間接使用 `writen`/`readn`）、reader（用 `readn` 讀 input pipe）、client `bench`（`tcp_connect_timeout`）以及 `tools/bench-evil.c` 都會連結並呼叫它。檔案只提供三類東西：「讀寫剛好 n bytes」的迴圈（`readn`/`writen`）、建立連線端點的函式（`tcp_listen`、`tcp_connect_timeout`），以及設定 socket 選項與格式化對端位址的小工具。它只依賴 libc 與 `bench_log.h`（只用了 `LOGD`），本身不讀寫任何全域狀態，也不查詢 `--unsafe` 開關；R4（readn）的 safe/unsafe 分支在呼叫端 `lib/proto.c` 的 `bn_recv`，不在這裡。

### `readn`
*位置：`lib/net.c:20`　簽章：`ssize_t readn(int fd, void *buf, size_t n)`*

- **用途**：從 `fd` 讀取，直到收滿 `n` bytes、遇到 EOF 或發生錯誤為止，回傳實際讀到的 byte 數。
- **設計**：TCP 是 byte stream，沒有訊息邊界，一次 `read()` 可能只拿到封包的一部分（原始碼註解標為 R4）。所以用 `while (got < n)` 迴圈累計，每次從 `p + got` 繼續讀剩下的 `n - got`。`EINTR` 代表 `read()` 被 signal 打斷、尚未讀到任何資料，直接重試，避免把不相干的 signal 當成連線錯誤（例如 reader 對 SIGTERM 裝了 handler，`server/reader.c:212-213`，它讀 input pipe 時就可能遇到；master 也有裝 handler，`server/main.c:384-396`；per-client child 則沒有裝 handler，`server/session.c:434-437`，此處的重試是防禦性的）。其他錯誤立即回傳 -1，不吞掉。
- **細節**：
  - 回傳值有三種：等於 `n`（成功）、小於 `n` 且 >= 0（`read()` 回傳 0，即對端 EOF，函式 `break` 後回傳目前累計值，可能是 0）、-1（`errno` 保留 `read()` 的原值）。
  - 只有 EINTR 會重試。`EAGAIN`/`EWOULDBLOCK` 不重試而是直接回傳 -1：當 socket 設了 `SO_RCVTIMEO`（見 `set_io_timeouts`）而逾時時，`read()` 就是以 `EAGAIN` 失敗，這正是 `server/session.c` 的 `handshake` 在 `bn_recv` 回 -1 且 `errno == EAGAIN || EWOULDBLOCK` 時判定「HELLO incomplete」的來源（`server/session.c:214-215`）；`handle_packet` 的 idle timeout 判斷也是同樣做法（`server/session.c:330`）。
  - 若已讀到部分資料後才逾時或出錯，回傳 -1，已讀的 bytes 留在 `buf` 但呼叫端無從得知數量；`bn_recv` 在 `readn` 回 -1 時直接回傳 -1（`lib/proto.c:94-95`），呼叫端只看 `errno` 當作連線錯誤處理，所以可接受。
  - `n == 0` 時迴圈不執行，直接回傳 0。
  - 對非阻塞 fd 使用時，讀不到資料也會得到 -1/`EAGAIN`；本專案只對 `tcp_connect_timeout` 內部短暫設成非阻塞，連線成功後會改回阻塞，所以不會出現這種情形。
  - 不受任何 `--unsafe` 開關影響。R4 unsafe 是 `bn_recv` 改用單次 `recv()` 而完全繞過 `readn`（`lib/proto.c:86-91`、`117-118`）；但 `readn` 在 reader 讀 input pipe 時仍照常使用。
- **關係**：被誰呼叫 → `lib/proto.c` `bn_recv`（讀 12 bytes 標頭與 payload，`lib/proto.c:93`、`120`）、`server/reader.c`（讀 input pipe 的 `PIPE_MSG_HDR` 與 `m.data`，`server/reader.c:263-265`）；呼叫 → libc `read()`。

### `writen`
*位置：`lib/net.c:38`　簽章：`ssize_t writen(int fd, const void *buf, size_t n)`*

- **用途**：把 `buf` 的 `n` bytes 全部寫進 `fd`，處理 partial write 與 `EINTR`，成功回傳 `n`，失敗回傳 -1。
- **設計**：`write()` 對 stream socket 與 pipe 都可能只寫出一部分（send buffer 快滿、被 signal 打斷後已寫一半），單次呼叫不能保證完整送出，所以用迴圈從 `p + put` 補寫剩下的部分。與 `readn` 對稱，`EINTR` 重試，其他錯誤回傳 -1。`writen` 沒有 EOF 之類的特別分支：`write()` 回傳非負值就累加到 `put`，迴圈直到 `put == n` 為止（若 `write()` 回傳 0，迴圈會直接再試一次）。
- **細節**：
  - 失敗時不回傳已寫出的數量，只回傳 -1；呼叫端（`bn_send`、`pipe_send`）把任何 -1 都視為失敗。
  - 對設有 `SO_SNDTIMEO` 的 socket（`set_io_timeouts` 的 `snd_s`），對端一直不讀、send buffer 塞滿時 `write()` 逾時而以 `EAGAIN` 失敗，`writen` 回傳 -1；child 主迴圈用這個 errno 辨認「client 不讀資料」（`server/session.c:468`）。
  - 對已被對端 RST 的 socket，`write()` 會產生 SIGPIPE；`writen` 不處理這件事，由 `server/main.c:338` 與 `client/main.c:74` 的 `signal(SIGPIPE, SIG_IGN)` 負責，使 `write()` 改為回傳 -1/`EPIPE`。`--unsafe=sigpipe`（R2）會把處置改回 `SIG_DFL`，此時 `writen` 的 `write()` 會直接讓行程被 signal 殺死。
  - 對 pipe：呼叫端保證訊息 <= `PIPE_BUF`（`server/session.c` `pipe_send` 的註解），此時單次 `write()` 本身就是原子的，多個 child 同時寫同一條 input pipe 不會交錯；`writen` 的迴圈在這種情況下只會執行一輪。
  - `n == 0` 時回傳 0，不呼叫 `write()`。
- **關係**：被誰呼叫 → `lib/proto.c` `bn_send`（header 與 payload 組在同一個 buffer 一次送出，`lib/proto.c:57`）、`server/session.c` `pipe_send`（`server/session.c:59`）；呼叫 → libc `write()`。

### `tcp_listen`
*位置：`lib/net.c:54`　簽章：`int tcp_listen(uint16_t port, int backlog)`*

- **用途**：建立並回傳一個已 `bind` 在 `port`、已進入 listen 狀態的 TCP socket fd；失敗回傳 -1。
- **設計**：先嘗試 `AF_INET6` socket，並把 `IPV6_V6ONLY` 設為 0，這樣同一個 socket 同時接受 IPv6 與 IPv4（IPv4 以 v4-mapped 位址出現，這也是 `sock_peer_str` 要處理 `IN6_IS_ADDR_V4MAPPED` 的原因）。若 `socket(AF_INET6, ...)` 失敗（核心沒有 IPv6），退回純 `AF_INET`，用 `INADDR_ANY`。`SO_REUSEADDR` 無條件開啟，沒有對應的 `--unsafe` 開關（`docs/DECISIONS.md` 說明「always on」），目的是 server 重啟時不被 `TIME_WAIT` 的舊連線擋住而 `bind` 失敗。`V6ONLY` 明確設 0 而不是依賴系統預設，因為 Linux 預設值取決於 `net.ipv6.bindv6only`。
- **細節**：
  - 兩個分支共用 `rc`，之後 `if (rc < 0 || listen(fd, backlog) < 0)` 統一處理失敗：先存 `errno`、`close(fd)`、再還原 `errno`，讓呼叫端的 `strerror(errno)` 看到的是 `bind`/`listen` 的原因（例如 `EADDRINUSE`）而不是 `close` 的結果。
  - 第一次 `socket()` 失敗就改試 IPv4，不看 `errno`；IPv4 也失敗則直接回傳 -1。
  - `setsockopt` 的回傳值沒有檢查，選項設定失敗不會使函式失敗。
  - 回傳的 fd 是阻塞式，沒有設 `SOCK_CLOEXEC`；child 在 fork 後自己 `close(g_cfg.listen_fd)`（`server/session.c:438`）。
  - `backlog` 由呼叫端指定，`server/main.c:378` 傳 128；port 是 host order，函式內用 `htons` 轉換。
- **關係**：被誰呼叫 → `server/main.c` `main`（`g_cfg.listen_fd = tcp_listen(g_cfg.port, 128)`，`server/main.c:378`）；呼叫 → libc `socket`、`setsockopt`、`bind`、`listen`、`close`。

### `set_nonblock`
*位置：`lib/net.c:86`　簽章：`int set_nonblock(int fd, int on)`*

- **用途**：開啟或關閉 `fd` 的 `O_NONBLOCK`，成功回傳 `fcntl(F_SETFL)` 的結果（0），失敗回傳 -1。
- **設計**：先用 `F_GETFL` 讀出現有旗標，再只改 `O_NONBLOCK` 這一個 bit 後 `F_SETFL` 寫回，避免覆蓋 `O_APPEND` 等其他狀態旗標。`on` 為非 0 時設起來，為 0 時清掉，同一個函式涵蓋兩個方向，供 `tcp_connect_timeout` 先設成非阻塞做可逾時的 connect、成功後再還原。
- **細節**：
  - `F_GETFL` 失敗時直接回傳 -1，不嘗試寫入。
  - 它是 libbench 對外匯出的符號，但目前只有 `lib/net.c` 內的 `tcp_connect_timeout` 呼叫它（grep 全專案確認）。reader 開裝置時用的是 `open(..., O_NONBLOCK)` 旗標，並未使用這個函式（`server/reader.c:83`）。
  - `tcp_connect_timeout` 呼叫它時不檢查回傳值。
- **關係**：被誰呼叫 → `lib/net.c` `tcp_connect_timeout`；呼叫 → libc `fcntl()`。

### `tcp_connect_timeout`
*位置：`lib/net.c:95`　簽章：`int tcp_connect_timeout(const char *host, uint16_t port, int timeout_ms)`*

- **用途**：連線到 `host:port`，每個候選位址最多等 `timeout_ms` 毫秒，成功回傳已連線、阻塞模式、關閉 Nagle 的 fd，失敗回傳 -1 並設好 `errno`。
- **設計**：用 `getaddrinfo`（`AF_UNSPEC`、`SOCK_STREAM`）取得 IPv4/IPv6 候選位址，依序嘗試，這樣 `localhost` 同時解析到 `::1` 與 `127.0.0.1` 時，即使 server 只聽其中一個也能連上。為了能設定逾時，socket 先設成非阻塞再 `connect()`：非阻塞 connect 會立刻回傳 `EINPROGRESS`，之後用 `poll(POLLOUT)` 等待最多 `timeout_ms`；阻塞式 connect 的逾時由核心決定，無法由呼叫端指定毫秒數。`poll` 回傳後必須用 `getsockopt(SO_ERROR)` 取出真正的連線結果，因為「可寫」同時代表成功與失敗。連線成功後立刻 `set_nonblock(fd, 0)` 還原成阻塞，讓後續透過 `bn_send`/`bn_recv` 使用的 `writen`/`readn` 在回傳的 fd 上以阻塞模式運作（函式本身不設任何 `SO_RCVTIMEO`/`SO_SNDTIMEO`）。再設 `TCP_NODELAY`：這個 client 是互動式終端機，每個按鍵一個小封包，Nagle 會把它延遲合併。
- **細節**：
  - `getaddrinfo` 失敗：用 `LOGD` 記下 `gai_strerror`，把 `errno` 設為 `EHOSTUNREACH` 並回傳 -1（`getaddrinfo` 的錯誤碼不是 `errno`，所以人為指定一個）。`getaddrinfo` 本身是阻塞呼叫，不受 `timeout_ms` 約束。
  - `connect()` 若立刻成功（`rc == 0`，常見於 loopback）就跳過 `poll`；若立刻失敗且不是 `EINPROGRESS`（例如 `ECONNREFUSED`），保留該 `errno`，換下一個位址。
  - `poll` 結果：`rc == 1` 則讀 `SO_ERROR`，非 0 時 `errno = err` 並視為失敗；`rc == 0` 為逾時，`errno = ETIMEDOUT`；`rc < 0`（含被 signal 打斷的 `EINTR`）不重試，直接視為此位址失敗且 `errno` 為 `poll` 設定的值。`getsockopt` 的回傳值未檢查。
  - 每個失敗的位址都先存 `errno`、`close(fd)`、還原 `errno` 再試下一個；全部失敗時回傳 -1，`errno` 是最後一個位址的失敗原因。`socket()` 本身失敗（例如該 family 不被支援）則 `continue`，且 `fd` 保持 < 0。
  - `timeout_ms` 是每個候選位址各自的上限，不是總時間。
  - `ports` 緩衝區 8 bytes 足以放 uint16_t 的十進位字串（最多 5 位數加結尾）。
  - 不受 `--unsafe` 開關影響。
- **關係**：被誰呼叫 → `client/session.c` `conn_connect`（`client/session.c:31`；呼叫端傳 2000 或 3000 ms，見 `client/session.c:172`、`client/subcmd.c:35`）、`tools/bench-evil.c`（傳 3000，`tools/bench-evil.c:43`、`241`、`262`）；呼叫 → `set_nonblock`、libc `getaddrinfo`/`socket`/`connect`/`poll`/`getsockopt`/`setsockopt`/`freeaddrinfo`，以及 `bench_log.h` 的 `LOGD`。

### `set_keepalive`
*位置：`lib/net.c:144`　簽章：`int set_keepalive(int fd, int idle_s, int intvl_s, int cnt)`*

- **用途**：對 `fd` 開啟 TCP keepalive，並設定閒置多久開始探測、探測間隔與失敗次數；成功回傳 0，開啟 `SO_KEEPALIVE` 失敗時回傳 -1。
- **設計**：這是 R5（timeout）的其中一項機制（另有 `set_io_timeouts` 與 child 主迴圈的 idle timeout，見 `docs/DECISIONS.md` R5）。`SO_RCVTIMEO` 只涵蓋「對端活著但不說話」，而對端整台機器消失（拔線、斷電）時連線不會有任何封包也不會有 RST，沒有 keepalive 的話 child 會一直佔著 slot。keepalive 讓核心在連線閒置 `idle_s` 秒後每 `intvl_s` 秒送一次探測，連續 `cnt` 次無回應就判定連線已死，後續 `read`/`write` 會以錯誤結束。
- **細節**：
  - 只有 `SO_KEEPALIVE` 本身失敗才回傳 -1；後面三個 `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` 的結果被忽略，所以回傳 0 並不保證三個參數都生效。
  - 實際使用的值是 30/10/3（`server/session.c:443`），即閒置 30 秒後開始探測，之後每 10 秒一次，連續 3 次失敗，約 60 秒內判定斷線（`docs/DECISIONS.md` R5）。
  - 單位是秒。`TCP_KEEPIDLE` 等為 Linux 專屬選項。
  - 在 `--unsafe=timeout` 下，呼叫端 `if (!UNSAFE(R_TIMEOUT))` 不會呼叫這個函式（`server/session.c:442`），所以 unsafe 時 socket 沒有 keepalive。
- **關係**：被誰呼叫 → `server/session.c` per-client child 初始化（`server/session.c:443`）；呼叫 → libc `setsockopt()`。

### `set_io_timeouts`
*位置：`lib/net.c:155`　簽章：`int set_io_timeouts(int fd, int rcv_s, int snd_s)`*

- **用途**：設定 socket 的接收逾時 `SO_RCVTIMEO` 與傳送逾時 `SO_SNDTIMEO`（秒）；某個參數 <= 0 表示不修改該方向；全部成功回傳 0，任何一個 `setsockopt` 失敗則回傳非 0。
- **設計**：R5（timeout）的核心。沒有逾時的話，一個連上後不說話的 socket（slowloris 式）會讓 child 永遠卡在 `read`，把 `--max-clients` 的 slot 佔滿；一個不讀資料的 client 則會讓 child 卡在 `write`。設成逾時後，阻塞的 `read`/`write` 會以 `EAGAIN` 失敗，由 child 主動結束。兩個方向分開傳參數，讓呼叫端可以只調整其中一個；`rc |= ...` 累積錯誤而不是第一個失敗就返回。
- **細節**：
  - 逾時以 `struct timeval` 的 `tv_sec` 設定，`tv_usec` 為 0，所以最小單位是 1 秒，呼叫端把毫秒常數除以 1000（`HELLO_TIMEOUT_MS / 1000`）後傳入。
  - 回傳值是 `setsockopt` 結果的 OR，成功為 0，失敗時為非 0（實際是 -1）；不保證 `errno` 是哪一個呼叫造成。
  - 呼叫兩次：child 一開始設 `(HELLO_TIMEOUT_MS/1000, IDLE_TIMEOUT_MS/1000)` = (5, 60)，也就是 handshake 階段 5 秒收不到 HELLO 就逾時，傳送端 60 秒（`server/session.c:444`）；handshake 完成後再設 `(60, 60)`，把接收端放寬到 60 秒閒置逾時（`server/session.c:236`），`HELLO_TIMEOUT_MS`=5000、`IDLE_TIMEOUT_MS`=60000 定義於 `server/server.h:28-29`。
  - 兩處呼叫都被 `if (!UNSAFE(R_TIMEOUT))` 包住，`--unsafe=timeout` 時完全不設逾時，`demo_r5.sh` 的 32 個沉默 socket 因此能鎖死所有 slot。
  - 逾時不是定時器：`SO_RCVTIMEO` 計算的是單次 `read()` 阻塞多久，對 `readn` 迴圈而言每次 `read()` 各自計時；若對端每 4 秒送 1 byte，整個封包可以超過 5 秒仍不逾時。
- **關係**：被誰呼叫 → `server/session.c` per-client child 初始化（`server/session.c:444`）與 `handshake` 結束處（`server/session.c:236`）；呼叫 → libc `setsockopt()`。

### `sock_peer_str`
*位置：`lib/net.c:169`　簽章：`char *sock_peer_str(int fd, char *buf, size_t n)`*

- **用途**：把 `fd` 的對端位址格式化成 `"ip:port"` 字串寫入 `buf`，並回傳 `buf`，供 log 與 slot 表標示是哪個 client。
- **設計**：用 `sockaddr_storage` 接 `getpeername`，這樣同一份程式碼能容納 IPv4 與 IPv6。因為 `tcp_listen` 建立的是 dual-stack socket，IPv4 client 的位址會以 v4-mapped IPv6（`::ffff:a.b.c.d`）出現，若直接用 `inet_ntop(AF_INET6, ...)` 會顯示成 `::ffff:127.0.0.1`；所以偵測到 `IN6_IS_ADDR_V4MAPPED` 就取後 4 bytes（`s6_addr[12]`）用 `AF_INET` 格式化，輸出與純 IPv4 socket 一致。函式永遠回傳可用的字串、不回傳 NULL，這樣呼叫端可以直接把結果丟進 log 格式字串。
- **細節**：
  - `getpeername` 失敗（例如對端已斷線導致 `ENOTCONN`），或位址族既不是 `AF_INET` 也不是 `AF_INET6`，則保持預設值 `host="?"`、`port=0`，輸出 `"?:0"`。
  - `host` 緩衝區大小為 `INET6_ADDRSTRLEN`，足以容納任一格式；port 用 `ntohs` 轉回 host order，以 `%u` 印出。
  - 最後以 `snprintf(buf, n, ...)` 寫入，超過 `n` 會被截斷但仍以 NUL 結尾（`n > 0` 時）；呼叫端傳的 `peer` 緩衝區是 64 bytes。
  - 只用區域變數，沒有 static buffer，可安全用於多個 child；但呼叫 `snprintf`/`inet_ntop` 不是 async-signal-safe，不可在 signal handler 中使用。
  - 輸出的 IPv6 位址不加方括號，所以 IPv6 字串形如 `::1:5678`，與 IPv4 的 `1.2.3.4:5678` 格式不同，只用於顯示。
- **關係**：被誰呼叫 → `server/main.c` `handle_accept`（`accept()` 之後立刻取得 peer 字串，`server/main.c:150`，之後傳給 `session_main` 存進 child 的 `s.peer`、也用於 log）；呼叫 → libc `getpeername`、`inet_ntop`、`snprintf`。

---

## `lib/proto.c`

`lib/proto.c` 實作 benchd 的線路協定（wire protocol）收發層：12 位元組封包標頭的編碼與解碼、整包送出的 `bn_send` / `bn_send_error`、帶完整驗證的 `bn_recv`，以及把 type / error / event 數字轉成名稱的三個查表函式。它被編進 `libbench.so.1`，所以每個執行檔都用同一份：master（`server/main.c` 的 `reject_busy`、`handle_accept` 以 `bn_send_error` 回 SERVER_BUSY）、每個 client 的 child（`server/session.c`）、`bench` 客戶端（`client/session.c` 的 `conn_recv` / `conn_send` 包裝）以及 `tools/bench-evil.c`。reader 行程不收發 TCP 封包，不使用這個檔案。它依賴 `lib/crc32.c` 的 `bench_crc32`、`lib/net.c` 的 `readn` / `writen`、`include/bench_log.h` 的 `LOGT`，以及 `include/bench_util.h` 的 `UNSAFE(R_VALIDATE)`（R3）與 `UNSAFE(R_READN)`（R4）兩個強健性開關。封包格式（magic `BN_MAGIC` = 0x424E、版本 `BN_VERSION` = 1、標頭 `BN_HDR_LEN` = 12、`BN_MAX_PAYLOAD` = 65536，標頭欄位皆為 big-endian）定義在 `include/bench_proto.h`。

### `bn_hdr_encode`
*位置：`lib/proto.c:18`　簽章：`void bn_hdr_encode(uint8_t out[BN_HDR_LEN], uint8_t type, uint32_t len, uint32_t crc)`*

- **用途**：把 type、payload 長度與 payload 的 CRC32 填成 12 位元組的線路標頭，寫到 `out`。
- **設計**：標頭欄位的版面是固定的：位元組 0-1 為 magic、2 為 version、3 為 type、4-7 為 len、8-11 為 crc32。多位元組欄位先用 `htons` / `htonl` 轉成網路位元組序，再用 `memcpy` 寫進 `out`，而不是把 `out` 強制轉型成結構指標去賦值。`out` 不保證對齊，`memcpy` 避免了未對齊存取與 strict-aliasing 的問題，也讓結果與主機的 endianness 無關。magic 與 version 由函式自己填入（`BN_MAGIC`、`BN_VERSION`），呼叫端不能指定，所以正常路徑不會產生錯誤的 magic 或版本。
- **細節**：
  - 只寫 `out[0..11]`，不檢查 `len` 是否超過 `BN_MAX_PAYLOAD`（那是 `bn_send` 的責任）；`len` 與 `crc` 原樣寫入，不會自己算 CRC。
  - `tools/bench-evil.c` 刻意利用這點：bad-len 模式（第 162-163 行）先令 `len = 0x00100000`，再呼叫 `bn_hdr_encode(pkt, BN_HELLO, len, 0xDEADBEEF)` 造出宣稱 1 MiB 長度、CRC 為假值的標頭，用來示範 R3。
  - 無回傳值、無 errno，沒有鎖，純函式，可重入。
- **關係**：被誰呼叫 → `bn_send`（`lib/proto.c`）、`tools/bench-evil.c` 的 `build_packet` 與 bad-len 攻擊路徑。呼叫 → 只用 libc 的 `htons` / `htonl` / `memcpy`。

### `bn_hdr_decode`
*位置：`lib/proto.c:29`　簽章：`void bn_hdr_decode(const uint8_t in[BN_HDR_LEN], struct bn_hdr *h)`*

- **用途**：把收到的 12 位元組原始標頭解成主機位元組序的 `struct bn_hdr`（`magic`、`ver`、`type`、`len`、`crc32`）。
- **設計**：與 `bn_hdr_encode` 對稱，同樣用 `memcpy` 把欄位取到區域變數再做 `ntohs` / `ntohl`，避免對 `in + 4`、`in + 8` 做未對齊的 32 位元讀取。它只做「格式轉換」，不做任何檢查：magic、版本、長度的判斷全部留給 `bn_recv`，這樣「解碼」與「驗證」分開，R3 的 `--unsafe=validate` 只需要在 `bn_recv` 裡跳過驗證，不必改動解碼。
- **細節**：
  - `ver` 與 `type` 是單一位元組，直接複製，不需轉換。
  - 任何 12 個位元組都能被解碼成功，所以不會失敗，也沒有回傳值。
  - 沒有鎖，可重入。
- **關係**：被誰呼叫 → `bn_recv`（`lib/proto.c`）。呼叫 → `ntohs` / `ntohl` / `memcpy`。

### `bn_send`
*位置：`lib/proto.c:46`　簽章：`int bn_send(int fd, uint8_t type, const void *payload, uint32_t len)`*

- **用途**：把一個完整封包（標頭加 payload）寫到 `fd`，成功回傳 0，失敗回傳 -1 並設定 `errno`。
- **設計**：
  - 標頭與 payload 先組在檔案層級的 `static uint8_t txbuf[BN_HDR_LEN + BN_MAX_PAYLOAD]`（第 44 行，約 64 KiB），再用一次 `writen` 送出，所以每個封包只對應一次 `write()`（`writen` 在遇到部分寫入時才會再呼叫 `write()`），這是 `docs/DECISIONS.md` Protocol 一節（「`bn_send` writes header and payload in a single `write()`」）與第 43 行註解記載的決定。檔頭註解說明 TRACE 日誌的用途是對照 tcpdump 抓包。
  - 用 `static` 而不是區域陣列，是因為 64 KiB 放在 stack 上太大；這個專案沒有執行緒（fork 模型），每個行程各自有一份 `txbuf`（fork 後寫入時複製），所以不需要鎖。代價是這個函式不可重入、不是 async-signal-safe，也不能從訊號處理函式呼叫。
  - 長度先檢查 `len > BN_MAX_PAYLOAD` 就以 `EMSGSIZE` 拒絕，確保本端絕不送出自己的 `bn_recv` 會判為 BAD_LENGTH 的封包，也保證不會寫爆 `txbuf`。
  - CRC 在此計算：`len` 為 0 時 crc 固定為 0（不呼叫 `bench_crc32`），與 `bn_recv` 的「`len == 0` 時期望 crc 為 0」互相對應。
  - 在 `writen` 之前以 `LOGT` 印出 `tx pkt type=NAME(0x..) len= crc=`，用來和 tcpdump 對時間與內容。
- **細節**：
  - 回傳值：`len > BN_MAX_PAYLOAD` 時 `errno = EMSGSIZE`、回 -1；`writen` 失敗時回 -1，`errno` 保留 `write()` 的值（例如對端已關閉的 `EPIPE`、`ECONNRESET`，或 child 在 `server/session.c:444` 以 `set_io_timeouts` 設了 `SO_SNDTIMEO` 後送不出去的 `EAGAIN`；該設定只在未開 `--unsafe=timeout`（R5）時才執行）。`writen` 會重試 `EINTR` 與部分寫入。
  - `payload` 會被 `memcpy` 到 `txbuf` 後才送，所以呼叫端的 buffer 可以是區域變數，但不可以指向 `txbuf` 本身（它是 static，檔外也拿不到）。`len == 0` 時不讀 `payload`，可傳 `NULL`（`client/session.c` 的 `conn_bye` 以 `bn_send(c->fd, BN_BYE, NULL, 0)` 這樣用）。
  - 若 `writen` 在中途失敗，對端可能收到半個封包；呼叫端的處理方式都是結束該連線，沒有嘗試重送。
  - SIGPIPE：對已斷線的 socket 寫入會產生 SIGPIPE，是否殺掉行程由 R2（`--unsafe=sigpipe`，見 `server/main.c` 的 `signal(SIGPIPE, ...)`）決定；`bn_send` 本身不處理訊號，只回報 `EPIPE`。
  - 在 release 建置（`LOG_COMPILE_LEVEL=2`）中 `LOGT` 展開為 `sizeof(printf(...))`，不產生任何程式碼；debug 建置（`LOG_COMPILE_LEVEL=4`）才會依執行期等級輸出。
  - 不受 `--unsafe` 開關影響（R3 / R4 只作用於接收方向）。
- **關係**：被誰呼叫 → `bn_send_error`（本檔）、`server/session.c`（WELCOME、REPLAY_BEGIN/END、DATA、EVENT、STATUS、CTRL_RESULT、INFO、PONG 等）、`client/session.c` 的 `conn_send` 與 HELLO / BYE、`tools/bench-evil.c`。呼叫 → `bench_crc32`（`lib/crc32.c`）、`bn_hdr_encode`、`writen`（`lib/net.c`）、`bn_type_name`（供 `LOGT` 使用）。

### `bn_send_error`
*位置：`lib/proto.c:60`　簽章：`int bn_send_error(int fd, uint16_t code, const char *fmt, ...)`*

- **用途**：組一個 `BN_ERROR` 封包（`struct bn_error_hdr` 加一段 printf 格式化的文字）並送出，回傳值同 `bn_send`。
- **設計**：ERROR 的 payload 是 4 位元組的 `code`（大端）與 `pad`，後面接給人看的訊息文字。函式在區域陣列 `buf[sizeof(struct bn_error_hdr) + 256]` 上先填 `code`（`htons`）與 `pad = 0`，再用 `vsnprintf` 直接把訊息寫在標頭後面，最後交給 `bn_send`。這樣呼叫端可以像 `printf` 一樣帶參數，而且訊息長度被限制在 256 位元組的緩衝區內，不可能超過 `BN_MAX_PAYLOAD`。`include/bench_proto.h` 為它宣告了 `__attribute__((format(printf, 3, 4)))`，編譯器會檢查格式字串與參數是否一致。
- **細節**：
  - `vsnprintf` 的容量參數是 256，回傳值 `n` 是「若不截斷會寫入的長度」，所以程式把 `n` 夾在 0 到 255 之間：`n < 0`（格式化失敗）當 0，`n > 255`（被截斷）當 255。送出的長度是 `sizeof *e + n`，也就是 4 加上文字長度，不含結尾的 NUL，線路上的文字不以 NUL 結尾，接收端要用 payload 長度減 4 來決定文字長度（例如 `tools/bench-evil.c` 以 `%.*s` 配合 `h.len - 4` 印出）。
  - `code` 以 `uint16_t` 傳入並轉成網路序；常用的值是 `enum bn_err`（`BN_ERR_BAD_MAGIC` 到 `BN_ERR_BAD_REQUEST`）。
  - 不受 `--unsafe` 影響。與 `bn_send` 一樣會用到 static 的 `txbuf`，同樣不可重入。
  - 回傳值只表示是否寫出成功；呼叫端多數情況（例如 `server/session.c` 的 `violation`）忽略它，因為隨後就會結束連線。`BAD_REQUEST`（不支援的 baud）等不影響連線的錯誤則是用它的回傳值直接 `return`（`server/session.c` 第 354、356、400、402 行）。
- **關係**：被誰呼叫 → `server/main.c`（`reject_busy` 的 SERVER_BUSY 與 `handle_accept` 在 fork 失敗時的回應）、`server/session.c`（`violation`、HELLO 逾時、閒置逾時、NOT_CONTROLLER、DEVICE_LOST、BAD_REQUEST 等）。呼叫 → `bn_send`，以及 libc 的 `vsnprintf`。

### `bn_recv`
*位置：`lib/proto.c:77`　簽章：`int bn_recv(int fd, struct bn_hdr *h, void *buf, size_t cap, int *perr)`*

- **用途**：從 `fd` 讀入一個完整封包：先讀 12 位元組標頭並解碼，驗證 magic、版本、長度，再讀 payload，最後驗證 CRC32。回傳 1 表示成功、0 表示 EOF、-1 表示 I/O 錯誤（`errno`）、-2 表示協定錯誤（原因寫在 `*perr`）。
- **設計**：
  - 它是整個協定「不信任對端」的唯一關卡：所有進入 `server/session.c` 的資料都先經過這裡。驗證的順序是先檢查標頭（magic、version、`len`）、再讀 payload、最後驗 CRC，因為標頭檢查不用讀 payload，能在不吃下惡意長度的 payload 之前拒絕；CRC 則必須有完整 payload 才能算。
  - 長度上限用 `h->len > BN_MAX_PAYLOAD || h->len > cap`：前者是協定的絕對上限，後者保護「這個呼叫端」的緩衝區。`server/session.c` 的 `handle_packet` 與 `handshake` 的緩衝區是 4096 位元組的區域陣列，所以實際上限是 4096；`len` 為 1 MiB 的封包在安全模式下直接得到 BAD_LENGTH。
  - 讀取一律使用 `readn`（`lib/net.c`），迴圈直到讀滿指定位元組或 EOF，並重試 `EINTR`。TCP 是位元組串流，一個封包可能被切成任意片段到達，單次 `read` / `recv` 不保證拿到整個標頭或 payload，這就是 R4（readn）要防的問題。
  - 結果碼把「連線層」和「協定層」分開：-1 帶 `errno`（呼叫端可用 `EAGAIN` 判斷 `SO_RCVTIMEO` 逾時，該逾時由 `server/session.c:444` 與 `:236` 的 `set_io_timeouts` 設定，`--unsafe=timeout`（R5）時不設，也就不會出現 `EAGAIN`；`server/session.c` 即用它區分 HANDSHAKE_TIMEOUT / IDLE_TIMEOUT），-2 帶 `*perr`，由呼叫端轉成 `bn_send_error` 或斷線訊息，0 則是對端正常關閉。
  - 呼叫端可傳 `perr = NULL`，函式內部改指向區域變數 `dummy`；`*perr` 一開始就被設為 0，所以只有回傳 -2 時它才有意義。
- **細節**：
  - **正常流程**：`readn` 讀 12 位元組。`n < 0` 回 -1；`n < BN_HDR_LEN`（EOF，包含讀到一半就 EOF）回 0，不另外區分「標頭中途斷線」。解碼後依序檢查 `h->magic != BN_MAGIC` → `BN_ERR_BAD_MAGIC`、`h->ver != BN_VERSION` → `BN_ERR_BAD_VERSION`、長度過大 → `BN_ERR_BAD_LENGTH`，全部回 -2。`h->len` 為 0 時不讀 payload；否則 `readn` 讀 `h->len` 位元組，`n < 0` 回 -1，短讀（EOF 在 payload 中途）回 0。最後 CRC：`len` 為 0 時期望值為 0，否則 `bench_crc32(buf, h->len)`，不符回 `BN_ERR_BAD_CRC`（-2）。全部通過後以 `LOGT` 印出 `rx pkt type=... len= crc=`，回 1。
  - **只驗 magic / version / len / CRC，不驗 type**：type 不明（`BN_ERR_BAD_TYPE`）由 `server/session.c` 的 `handle_packet` 與 `handshake` 自己檢查，所以這裡的 -2 只會有四種 `perr`：BAD_MAGIC、BAD_VERSION、BAD_LENGTH、BAD_CRC。
  - 即使回 -2，`*h` 也已經被填入解碼後的標頭（`bn_hdr_decode` 在驗證之前），但 BAD_MAGIC / BAD_VERSION / BAD_LENGTH 時 payload 尚未讀取，串流位置停在標頭之後；因此呼叫端的做法是回報錯誤並關閉連線，不嘗試重新同步。
  - 逾時：child 在 socket 上設了 `SO_RCVTIMEO`（僅限未開 R5 時），`readn` 因此以 `EAGAIN` / `EWOULDBLOCK` 回 -1。就算已讀入部分位元組，只要發生錯誤，`readn` 就回 -1 而不是短計數，所以標頭或 payload 只到一半時逾時也會是 -1 / `EAGAIN`，不是 0。
  - `LOGT` 只在成功時執行；格式錯誤的封包不會被 TRACE 記錄（錯誤由呼叫端的 `LOGW` 記錄）。
  - **`--unsafe=readn`（R4，`UNSAFE(R_READN)`）**：標頭改用單次 `recv(fd, raw, 12, 0)`，事先把 `raw` 清成 0；`n <= 0` 時回 0（EOF）或 -1。若標頭被分成多片到達（例如 `tools/bench-evil.c` 的 `slow` 模式，每 50 ms 只送 1 位元組），只收到前幾個位元組，其餘維持 0，解碼後 magic 不符，得到假的 `BAD_MAGIC`，這就是 `demo_r4.sh` 展示的現象。payload 也改成單次 `recv(fd, buf, h->len, 0)`，且略過短讀檢查（`!UNSAFE(R_READN) && n < h->len` 這一條只在安全模式生效）：只收到部分 payload 時仍回傳成功，而剩下的位元組留在串流中，下一個封包因此錯位；CRC 檢查（如果沒關）則會對 `buf` 中的殘留資料計算而失敗。
  - **`--unsafe=validate`（R3，`UNSAFE(R_VALIDATE)`）**：整段 magic / version / 長度檢查與最後的 CRC 檢查都被跳過，`h->len` 被盲目採信。呼叫端 `handle_packet` 的 `buf` 只有 4096 位元組，若對端宣稱 `len` = 1 MiB 並送出那麼多資料，`readn(fd, buf, h->len)` 會寫穿 stack（`stack smashing detected` 或 SIGSEGV），這就是 `demo_r3.sh` 的 unsafe 結果（`docs/DECISIONS.md` R3 條）。這段註解寫在 payload 讀取之前（第 115 行）。
  - 兩個開關互相獨立，可單獨或同時開啟（`--unsafe=all`）。
- **關係**：被誰呼叫 → `server/session.c` 的 `handshake`（讀 HELLO）與 `handle_packet`（之後的每個封包）、`client/session.c` 的 `conn_recv`、`tools/bench-evil.c`。呼叫 → `readn`（`lib/net.c`）、`bn_hdr_decode`、`bench_crc32`（`lib/crc32.c`）、`bn_type_name`（供 `LOGT`）。

### `bn_type_name`
*位置：`lib/proto.c:139`　簽章：`const char *bn_type_name(int t)`*

- **用途**：把封包 type 數字（`enum bn_type`）轉成名稱字串，例如 `BN_DATA` 變成 `"DATA"`，未知的值回 `"?"`。
- **設計**：用 `switch` 而不是陣列，因為 type 的值不連續：client → server 為 0x01-0x08，server → client 為 0x81-0x8A，中間有空洞，陣列索引會浪費空間也容易出錯。每個 `case` 直接回傳字串常值，沒有可變狀態，所以可重入、async-signal-safe。參數用 `int` 而不是 `uint8_t`，呼叫端不必先轉型，超出範圍的值自然落到 `default`。
- **細節**：
  - 回傳的字串是靜態常值，呼叫端不可修改也不必釋放。
  - 涵蓋 `enum bn_type` 的全部 18 個值（8 個 client → server、10 個 server → client）。
  - 目前只有 `lib/proto.c` 內 `bn_send` 與 `bn_recv` 的 `LOGT` 呼叫它（其他檔案沒有用）。
- **關係**：被誰呼叫 → `bn_send`、`bn_recv`（TRACE 日誌）。呼叫 → 無。

### `bn_err_name`
*位置：`lib/proto.c:164`　簽章：`const char *bn_err_name(int c)`*

- **用途**：把 `enum bn_err` 的錯誤碼（0 到 12）轉成名稱字串，如 `"BAD_CRC"`、`"SERVER_BUSY"`；超出範圍回 `"?"`。
- **設計**：錯誤碼從 1 起連續編號，所以用 `static const char *n[]` 表以碼值直接索引：索引 0 是 `"OK"`，1 到 12 依序為 `BAD_MAGIC`、`BAD_VERSION`、`BAD_LENGTH`、`BAD_CRC`、`BAD_TYPE`、`NOT_CONTROLLER`、`DEVICE_LOST`、`SERVER_BUSY`、`HANDSHAKE_TIMEOUT`、`IDLE_TIMEOUT`、`SERVER_SHUTDOWN`、`BAD_REQUEST`。表的筆數用 `sizeof n / sizeof n[0]` 求得，新增錯誤碼時只要在表尾加字串，邊界檢查自動跟著變。檢查同時要求 `c >= 0`，因為參數是有號 `int`，負值若不擋會越界讀取。
- **細節**：
  - 這張表必須與 `include/bench_proto.h` 的 `enum bn_err` 的順序一致；`BAD_REQUEST = 12` 是擴充碼（`docs/DECISIONS.md`），位於表尾。
  - 回傳靜態常值，無鎖，可重入。
  - 用途是 log 與錯誤訊息：`server/session.c` 的 `violation` 用它組出 `"protocol error: %s"` 和 LOGW；client 端在 `client/session.c`（`conn_wait_welcome` 印出 server error、`handle_packet` 的 `LOGW`、`app_run` 在 `conn_recv` 失敗時以 `rc == -2 ? bn_err_name(perr) : ...` 組出傳給 `app_disconnect` 的原因）與 `client/subcmd.c` 印出伺服器回報的錯誤；`tools/bench-evil.c` 印出伺服器回覆。
- **關係**：被誰呼叫 → `server/session.c`、`client/session.c`、`client/subcmd.c`、`tools/bench-evil.c`。呼叫 → 無。

### `bn_evt_name`
*位置：`lib/proto.c:173`　簽章：`const char *bn_evt_name(int c)`*

- **用途**：把 EVENT 封包中的事件碼（`enum bn_evt`，1 到 7）轉成名稱，如 `"DEVICE_LOST"`、`"CTRL_TAKEN"`、`"DROPPED"`；超出範圍回 `"?"`。
- **設計**：與 `bn_err_name` 相同，用陣列索引加上 `sizeof` 求長度與 `c >= 0` 的檢查。事件碼從 `EV_DEVICE_LOST = 1` 開始，所以表的索引 0 放 `"?"` 作為佔位，讓索引直接等於碼值，之後依序是 `DEVICE_LOST`、`DEVICE_BACK`、`CTRL_TAKEN`、`CTRL_RELEASED`、`BAUD_CHANGED`、`DROPPED`、`SERVER_SHUTDOWN`。
- **細節**：
  - 表須與 `include/bench_proto.h` 的 `enum bn_evt` 保持同序。
  - 回傳靜態常值，可重入。
  - 目前只有 `tools/bench-evil.c` 在列印收到的 EVENT 時呼叫它；`bench` 客戶端 UI 沒有使用這個函式。
- **關係**：被誰呼叫 → `tools/bench-evil.c`。呼叫 → 無。

---

## `lib/util.c`

`lib/util.c` 是 `libbench.so.1` 裡的雜項工具檔，宣告在 `include/bench_util.h`，沒有狀態也沒有鎖（唯一的全域變數是 `bench_unsafe_mask`，定義於 `lib/util.c:13`，啟動時由 `server/main.c:329` 或 `client/main.c:73` 一次寫入，之後唯讀）。它不屬於特定行程：master、reader、per-client child（`benchd`）、`bench` 客戶端、`benchsim`、`bench-evil` 都連結同一份共享函式庫並呼叫這裡的函式。內容分五類：時鐘（`now_ns`、`mono_ns`、`mono_ms`、`sleep_ms`、`fmt_clock`、`fmt_hms`）、大小字串（`parse_size`、`fmt_size`）、字串處理（`bench_strlcpy`、`unescape`、`sanitize_name`、`parse_hostport`）、十六進位傾印（`hexdump_line`、`hexdump`），以及 `--unsafe=` 開關表（`bench_robust_name`、`bench_unsafe_parse`、`bench_unsafe_format`，對應 R1-R12，見 `docs/DECISIONS.md` 的 Robustness switches 一節）。檔案只依賴 libc（`clock_gettime`、`nanosleep`、`localtime_r`、`strtoull`、`strtok_r` 等）與 `bench_util.h`，不依賴 `lib/` 內其他檔案；反過來 `lib/log.c` 呼叫它的時鐘函式。檔案內所有函式都是可重入的（不使用靜態緩衝區，輸出一律寫入呼叫端提供的 buffer）。

### `now_ns`
*位置：`lib/util.c:15`　簽章：`uint64_t now_ns(void)`*

- **用途**：回傳 `CLOCK_REALTIME`（牆上時鐘）的目前時間，單位為奈秒（自 Unix epoch 起算）。
- **設計**：整個協定用牆上時間當作「跨行程、跨機器可比較、可顯示成時刻」的時間戳（ring 記錄的 `ts_ns`、`bn_event_hdr` 的 `ts_ns`、`server_start_ns`、PING 的 `client_ts_ns`、log 行首的時間），所以這些場合用 realtime 而不是 monotonic；monotonic 時間沒有絕對意義，無法顯示成時刻，也無法讓兩台機器對照。代價是牆上時鐘可能被 NTP 或手動調整而倒退，因此「逾時、截止時間」類的計算改用 `mono_ms` / `mono_ns`。
- **細節**：
  - 以 `tv_sec * 1000000000ull + tv_nsec` 組成 64 位元整數，先把 `tv_sec` 轉成 `uint64_t` 再乘，不會溢位。
  - `clock_gettime` 的回傳值沒有檢查（`CLOCK_REALTIME` 在 Linux 不會失敗）。
  - 閒置放開控制權（idle release）也用它：`server/session.c:351`、`:397` 把 `now_ns()` 存進 shm 的 `ctrl_last_input_ns`，`server/reader.c:281` 把 `now_ns()` 傳給 `ctrl_check_idle_locked`（`server/control.c:85`）比較，所以該判斷會受牆上時鐘跳動影響；`control.c:89` 先檢查 `now > ctrl_last_input_ns` 避免時鐘倒退時無號數相減變成巨大值。
- **關係**：被誰呼叫 → `lib/log.c`（`bench_log_write`，`lib/log.c:108`）、`server/ring.c:66`（每筆 ring 記錄的時間戳）、`server/session.c`（事件時間戳、`joined_ns`、`ctrl_last_input_ns`、PONG 的 `server_ts_ns`）、`server/main.c:357`（`server_start_ns`）、`server/control.c:40`、`server/reader.c`、`server/sysinfo.c:136`、`client/session.c:412` 與 `client/subcmd.c:183`（PING 時間戳）；呼叫 → `clock_gettime`。

### `mono_ns`
*位置：`lib/util.c:22`　簽章：`uint64_t mono_ns(void)`*

- **用途**：回傳 `CLOCK_MONOTONIC` 目前值，單位為奈秒。
- **設計**：需要「經過多久」而不需要「現在幾點」時用單調時鐘，不受 NTP 或手動改時間影響，間隔量測不會變成負數或暴增。
- **細節**：
  - 計算方式與 `now_ns` 相同；起點是開機後的任意時刻，不可轉成時刻字串。
  - 目前 repo 內只有 `tools/benchsim.c` 直接呼叫它（`benchsim.c:264`、`:456-464`，排程每個位元組在模擬傳輸線上的送出時間，需要奈秒精度）；其他地方都用毫秒版的 `mono_ms`。
- **關係**：被誰呼叫 → `mono_ms`、`tools/benchsim.c`；呼叫 → `clock_gettime`。

### `mono_ms`
*位置：`lib/util.c:29`　簽章：`uint64_t mono_ms(void)`*

- **用途**：單調時鐘的毫秒版，等於 `mono_ns() / 1000000ull`。
- **設計**：逾時、重連退避、輪詢截止時間都只需要毫秒精度，且 `poll()` 的 timeout 參數本來就是毫秒，所以多包一層讓呼叫端可以直接寫 `deadline = mono_ms() + N`。
- **細節**：
  - 向下取整（截斷小數毫秒）。
  - 回傳值是 `uint64_t`，呼叫端做 `deadline - mono_ms()` 時要先轉成有號數再判斷是否已過期，例如 `server/session.c:201` 與 `tools/bench-evil.c:76` 都把差值轉成 `int64_t`。
- **關係**：被誰呼叫 → `server/session.c`（HELLO 截止時間 `HELLO_TIMEOUT_MS`、`IDLE_TIMEOUT_MS` 的 `last_rx_ms` 比較，R5）、`server/reader.c`（裝置重開 `next_reopen`，R8；每秒一次的閒置放開檢查 `next_idle`，其內的 `ctrl_check_idle_locked` 受 R6 控制）、`server/main.c`（關機等待 deadline）、`client/session.c`（`last_rx_ms`、PING 間隔、重連 `retry_at`，R12）、`client/subcmd.c`、`client/cmd.c`、`client/ui.c`、`tools/bench-evil.c`；呼叫 → `mono_ns`。

### `sleep_ms`
*位置：`lib/util.c:31`　簽章：`void sleep_ms(unsigned ms)`*

- **用途**：睡眠指定的毫秒數，被信號打斷時會把剩下的時間睡完。
- **設計**：`nanosleep` 被信號（例如 master 的 SIGCHLD、SIGTERM handler）中斷時會回傳 -1 並設 `errno = EINTR`，同時把尚未睡完的時間寫回第二個參數。這裡把同一個 `ts` 同時當作請求與剩餘時間（`nanosleep(&ts, &ts)`），迴圈在 `EINTR` 時直接用剩餘時間再睡，所以呼叫端得到「至少睡滿 `ms`」的語意，不需要自己處理中斷。迴圈本體是空陳述式 `;`，所有工作都在條件式內。
- **細節**：
  - `tv_sec = ms / 1000`，`tv_nsec = (ms % 1000) * 1000000`，`(ms % 1000)` 先轉成 `long` 再乘 `1000000L`，與 `tv_nsec` 的型別一致；最大值 999000000 小於 10^9，所以 `tv_nsec` 恆在合法範圍內。
  - 若 `nanosleep` 因 `EINTR` 以外的原因失敗，迴圈直接結束，不重試（這裡的 `ts` 欄位值恆合法，實務上不會發生）。
  - 傳入的是相對時間長度，不是絕對時刻，所以不需要看牆上時鐘。
- **關係**：被誰呼叫 → `server/main.c`（`accept` 遇到 `EMFILE`/`ENFILE` 時退避 100 ms，`main.c:146`；關機流程輪詢等待各 slot 的 offset 追上 `target`（最多 1 s，`main.c:233`，間隔 20 ms），以及 SIGTERM 之後的 `waitpid` 迴圈（`main.c:263`，間隔 10 ms））、`tools/bench-evil.c`（各攻擊模式的節奏控制）；呼叫 → `nanosleep`。

### `fmt_clock`
*位置：`lib/util.c:38`　簽章：`char *fmt_clock(uint64_t ns, char *buf, size_t n)`*

- **用途**：把牆上時間奈秒值格式化成本地時間的 `HH:MM:SS.mmm`（例如 `20:44:13.123`），寫入呼叫端的 `buf` 並回傳同一個指標。
- **設計**：使用 `localtime_r`（可重入版本，結果存在呼叫端堆疊上的 `struct tm`）而不是 `localtime`（回傳共用的靜態結構），所以 log 輸出、UI 繪製等不同呼叫點之間不會互相覆蓋。毫秒部分不從 `tm` 取（`tm` 沒有亞秒欄位），而是由原始奈秒值 `(ns / 1000000ull) % 1000` 算出。回傳 `buf` 是為了能直接放進 `printf` 的參數列。
- **細節**：
  - 輸入必須是牆上時間（`now_ns()` 或協定裡的 `ts_ns`），傳 `mono_ns()` 的值只會得到 1970 年開機後幾小時的無意義時刻。
  - 顯示的是本機時區，伺服器與客戶端若在不同時區，同一個 `ts_ns` 會顯示不同的時刻。
  - `snprintf` 會截斷到 `n`；`n == 0` 時不寫入任何東西。`localtime_r` 的回傳值沒有檢查。
  - `localtime_r` 並非 async-signal-safe，不可在信號 handler 內呼叫。
  - 呼叫端用的 buffer 大小：`lib/log.c:108` 為 `ts[32]`、`client/ui.c:112` 為 `t[32]`，輸出實際只有 12 字元。
- **關係**：被誰呼叫 → `lib/log.c` 的 `bench_log_write`（每行 log 的時間戳）、`client/ui.c` 的 `ts_prefix`（`:ts` 模式下每行前綴）；呼叫 → `localtime_r`、`snprintf`。

### `fmt_hms`
*位置：`lib/util.c:48`　簽章：`char *fmt_hms(uint64_t ns, char *buf, size_t n)`*

- **用途**：同 `fmt_clock`，但只輸出 `HH:MM:SS`（沒有毫秒），例如 `20:44:13`。
- **設計**：事件訊息（控制權變更、DROPPED、SERVER_SHUTDOWN 等）與子命令輸出只需要秒級精度，省略毫秒可讓行更短；與 `fmt_clock` 分成兩個函式而不是加參數，是因為兩者的格式字串與呼叫點都固定。
- **細節**：
  - 一樣使用 `localtime_r` 與本地時區；`ns` 必須是牆上時間。
  - 呼叫端 buffer 為 `ui_event` 內的 `hms[16]`，輸出只有 8 字元。
- **關係**：被誰呼叫 → `client/ui.c` 的 `ui_event`（事件訊息前綴）、`client/subcmd.c:80`（`print_event`，`subcmd.c:73`，印出事件時間）；呼叫 → `localtime_r`、`snprintf`。

### `parse_size`
*位置：`lib/util.c:57`　簽章：`int parse_size(const char *s, uint64_t *out)`*

- **用途**：把 `64K`、`1M`、`2G`、`4096`、`64KB`、`64KiB` 這類大小字串轉成位元組數，成功回傳 0 並寫入 `*out`，格式錯誤回傳 -1。
- **設計**：命令列選項（`benchd --history`、`bench -n`）需要人類易讀的大小寫法。先用 `strtoull` 取出整數部分，再看緊接的後綴決定倍率：空字串結尾 = 1、`B` = 1、`K` = 1024、`M` = 1024^2、`G` = 1024^3（二進位倍率，與 README 的 `64K` 預設值一致）。後綴字母用 `toupper` 轉大寫，所以大小寫不拘。對有倍率的後綴再接受選用的 `I` 與選用的 `B`（`K`、`KB`、`KiB`、`Ki` 皆可），最後必須剛好到字串結尾，否則視為錯誤，避免 `64Kfoo` 被悄悄接受。
- **細節**：
  - `s` 為 `NULL` 或空字串回傳 -1；`strtoull` 前先把 `errno` 清為 0，`errno` 非 0（例如 `ERANGE`）或沒有讀到任何數字（`end == s`）回傳 -1。
  - 只有 `B` 後綴時倍率維持 1，`I` 不能單獨跟在數字後面（`default` 分支回傳 -1），`64BB` 也會被拒絕，因為只有 `mul > 1` 才會再吃 `I`/`B`。
  - 失敗時 `*out` 不會被修改。
  - 乘法 `v * mul` 沒有檢查溢位：數值本身未超出 `unsigned long long`，但乘上 1024^3 後可能繞回。另外 `strtoull` 本身接受前導空白與 `+`/`-` 符號，這裡沒有額外拒絕，所以 `-1` 會被解成 `ULLONG_MAX`。呼叫端的後續處理：`server/main.c:321-323` 只把 `--history` 小於 4096 的值提高到 4096 並以 `(history + 7) & ~7ull` 對齊 8 位元組（不檢查上限，`ULLONG_MAX` 加 7 會繞回後對齊成 0）；`client/main.c:62` 檢查 `-n` 不超過 `0xFFFFFFFF`，所以客戶端會擋下 `-1` 這類輸入。
- **關係**：被誰呼叫 → `server/main.c:294`（`--history`）、`client/main.c:62`（`-n` 重播大小）；呼叫 → `strtoull`、`toupper`。

### `fmt_size`
*位置：`lib/util.c:87`　簽章：`char *fmt_size(uint64_t b, char *buf, size_t n)`*

- **用途**：把位元組數轉成短字串：小於 1024 輸出 `<n>B`，其他輸出一位小數加 `K`/`M`/`G`（例如 `12620` → `12.3K`），回傳 `buf`。
- **設計**：用於狀態列、啟動訊息與 DROPPED 事件文字，所以要短。以 1024 為界逐級判斷，單位上限是 `G`，更大的數值仍用 `G` 表示（不會升到 `T`）。小於 1024 時用整數格式（`%llu`，先轉 `unsigned long long`，因為 `uint64_t` 在不同平台對應的型別不同）避免出現 `512.0B`。與 `parse_size` 使用同一組二進位倍率，所以 `fmt_size(parse_size("64K"))` 會得到 `64.0K`。
- **細節**：
  - 小數用 `%.1f` 四捨五入，因此接近單位邊界的值可能顯示成 `1024.0K`（例如 1048575 位元組），而不是進位為 `1.0M`。
  - `snprintf` 會依 `n` 截斷；呼叫端的 buffer 為 `sz[32]`（`send_dropped`、`ui_status`）與 `hist[32]`（`main.c`）；`uint64_t` 最大值時輸出 `17179869184.0G`（14 字元），仍遠小於 32。
- **關係**：被誰呼叫 → `server/main.c:402`（啟動訊息顯示 history 大小）、`server/session.c:103` 的 `send_dropped`（「dropped 12.3K」事件文字；`dropped` 來自 `session.c:138` 的追趕落後偵測（受 R10 控制）以及 `session.c:268-278` 的重播／續傳起點落在 `tail` 之前）、`client/ui.c:440`（狀態列的接收位元組數）；呼叫 → `snprintf`。

### `bench_strlcpy`
*位置：`lib/util.c:100`　簽章：`size_t bench_strlcpy(char *dst, const char *src, size_t n)`*

- **用途**：BSD `strlcpy` 的自製版本：最多複製 `n - 1` 個字元並保證以 NUL 結尾，回傳 `strlen(src)`（呼叫端可用 `回傳值 >= n` 判斷是否被截斷）。
- **設計**：程式裡幾乎所有固定長度欄位（協定封包的 `name`、`holder`、shm 內的 `ctrl_name`、`dev_path`、`peer`）都要「複製、不溢位、保證有結尾」。`strncpy` 在來源太長時不補 NUL，來源較短時還會把整個目的 buffer 補零，不適合這個用途；`strcpy` 不檢查長度。這個函式先 `strlen` 取得完整長度，再 `memcpy` 實際要複製的 `min(len, n-1)` 位元組，最後手動補 `\0`。
- **細節**：
  - `n == 0` 時不會碰 `dst`，只回傳長度。
  - `src` 必須是以 NUL 結尾的字串，因為先對它做完整的 `strlen`。從網路收來的固定寬度欄位（例如 HELLO 的 32 位元組 `name`）不能直接當 `src`，要先用 `sanitize_name` 之類的方式確保結尾（`server/session.c:227-229` 就是先 `memcpy` 到有大小的 `s->name` 再 `sanitize_name`）。
  - 與回傳值語意一致，常見用法是 `bench_strlcpy(x, y, sizeof x)`，呼叫端多半不檢查回傳值，截斷即接受。
- **關係**：被誰呼叫 → 幾乎所有 server/client 檔案：`server/control.c`（`ctrl_name`、`holder`、`note->who`）、`server/session.c`（slot 的 `name`/`peer`、WELCOME 欄位）、`server/main.c:353`、`server/sysinfo.c`、`client/session.c`、`client/main.c:49`、`tools/bench-evil.c`；同檔的 `sanitize_name`（補 `anon`）與 `bench_unsafe_parse`（複製選項字串）；呼叫 → `strlen`、`memcpy`。

### `hexval`
*位置：`lib/util.c:111`　簽章：`static int hexval(int c)`*

- **用途**：把一個十六進位數字字元轉成 0-15 的值，不是十六進位字元則回傳 -1。
- **設計**：只給 `unescape` 解析 `\xHH` 用，所以設為 `static`。數字直接減 `'0'`；字母先 `tolower` 再判斷 `a`-`f`，所以大小寫皆可。
- **細節**：
  - 參數是 `int`，呼叫端先轉成 `unsigned char` 再傳入（`unescape` 內為 `hexval((unsigned char)in[1])`），避免有號 `char` 的高位元組進入 `tolower` 造成未定義行為。
  - 傳入 `'\0'`（字串結尾）回傳 -1，這讓 `unescape` 不需要另外判斷是否讀到結尾。
- **關係**：被誰呼叫 → 同檔 `unescape`；呼叫 → `tolower`。

### `unescape`
*位置：`lib/util.c:119`　簽章：`size_t unescape(const char *in, uint8_t *out, size_t cap)`*

- **用途**：把帶有 C 風格跳脫序列的字串轉成原始位元組寫入 `out`（最多 `cap` 個），回傳寫出的位元組數；`bench send "reset\n"` 靠它把命令列文字變成要送給 UART 的資料。
- **設計**：Shell 難以直接輸入 CR、ESC、NUL 或任意位元組，所以讓使用者在字串內用跳脫序列表示。輸出是位元組陣列加長度，而不是 C 字串，因為 `\0` 與 `\x00` 會產生真正的 0 位元組。迴圈條件 `*in && n < cap` 同時防止讀過輸入結尾與寫出 `out` 範圍。支援的序列：`\n`(0x0a)、`\r`(0x0d)、`\t`(0x09)、`\e`(0x1b)、`\0`(0x00)、`\\`(反斜線)、`\xH`/`\xHH`（一或兩位十六進位數字）。
- **細節**：
  - 不是跳脫序列的一般字元原樣複製。輸入最後一個字元是反斜線（`!in[1]`）時視為字面反斜線複製。
  - 未知的跳脫（例如 `\q`、`\"`）輸出反斜線後面的那個字元本身（`default` 分支，反斜線被丟掉）。
  - `\x` 後面沒有任何十六進位數字時，輸出字母 `x`（程式碼註解寫「keep literally」，但實際上反斜線被丟掉，只留 `x`）；只有一位數字時，值就是那一位數字（`\x4` → 0x04）；兩位時為 `h1 * 16 + h2`。讀取 `in[2]` 前先確認 `h1 >= 0`（即 `in[1]` 不是 NUL），不會讀過字串結尾。`\x414` 只吃兩位，結果為 `A`、`4`。
  - 不會在 `out` 尾端補 NUL；`cap` 不夠時悄悄截斷，不回報。`client/subcmd.c:246` 使用 `data[4096]`，超出的部分就被丟棄。
  - 不處理八進位（`\0` 永遠是單一個 NUL，後面的數字照一般字元處理）。
- **關係**：被誰呼叫 → `client/subcmd.c:246` 的 `sub_send`（`bench send <host> "text"`）；呼叫 → 同檔 `hexval`。

### `sanitize_name`
*位置：`lib/util.c:156`　簽章：`void sanitize_name(char *s, size_t n)`*

- **用途**：就地把使用者名稱清理成可安全顯示的字串：保證以 NUL 結尾、非可見 ASCII 字元換成 `?`、去除頭尾空白、清理後若為空就改成 `anon`。
- **設計**：名稱來自網路上的 HELLO 封包（固定 32 位元組、對方想填什麼就填什麼，不保證有 NUL），之後會進入 log、事件文字、`INFO` 輸出與客戶端終端機。所以第一步 `s[n - 1] = 0` 強制結尾，後續用 `strnlen(s, n)` 取長度；控制字元（< 0x20，含 ESC 與換行）與 0x7f 以上的位元組（包含 UTF-8 多位元組字元）都換成 `?`，避免注入終端機跳脫序列或破壞 log 行。先修剪尾端空白（只要減少 `len`），再算出前導空白數 `lead`，用 `memmove` 連同結尾 NUL 一起往前搬；因為尾端先修剪過，全空白字串會先變成空字串，前導迴圈會停在 NUL 而不會越界。最後若字串為空，用 `bench_strlcpy` 填入 `anon`，所以名稱永遠非空。
- **細節**：
  - `n == 0` 直接返回，不碰 `s`。
  - 字串中途若有 NUL，只處理到第一個 NUL 為止；NUL 之後的殘留位元組不會被清理，但也不會被任何以 NUL 結尾的操作讀到。
  - 名稱太長的情況由呼叫端的 buffer 大小決定（`BN_NAME_LEN` = 32，實際最多 31 字元）。
  - 與 `--unsafe` 無關：此函式本身不檢查 `UNSAFE(...)`，名稱清理無法被任何開關關閉（R3 validate 控制的是 `lib/proto.c` 與 `server/session.c` 裡的封包驗證，不是這裡）。
- **關係**：被誰呼叫 → `server/session.c:229`（握手時清理 HELLO 的 `name`）、`server/main.c:123`（`reject_busy` 在 SERVER_BUSY 前窺看 HELLO 取名稱寫 log）、`client/main.c:100`（客戶端自己的名稱，預設 `user@host`）；呼叫 → `strnlen`、`memmove`、`bench_strlcpy`。

### `parse_hostport`
*位置：`lib/util.c:177`　簽章：`int parse_hostport(const char *s, char *host, size_t hostsz, uint16_t *port, uint16_t defport)`*

- **用途**：把 `host`、`host:port`、`[v6addr]`、`[v6addr]:port` 拆成主機字串與埠號。成功回傳 0，格式錯誤回傳 -1。沒寫埠號時用 `defport`。
- **設計**：命令列的位址參數要同時支援名稱、IPv4 與 IPv6（伺服器是 dual-stack，見 DECISIONS）。IPv6 位址本身含冒號，所以要靠方括號分隔：`s[0] == '['` 時以 `strchr` 找 `]`，括號內是主機，`]` 後面只能是 `:port` 或字串結束，其他情況（`[::1]x`）回傳 -1。沒有方括號時用 `strrchr` 找最後一個冒號；若字串裡不只一個冒號（`strchr` 找到的第一個冒號不等於最後一個），判定為沒有括號的裸 IPv6 位址，整個字串當主機、使用預設埠，不嘗試切出埠號。
- **細節**：
  - 一進函式就先寫 `*port = defport`，即使之後回傳 -1，`*port` 也已經是預設值。失敗時 `host` 可能已被部分寫入，呼叫端應整體丟棄。
  - `s` 為 `NULL` 或空字串回傳 -1。非括號形式下主機長度為 0（例如 `:9000`）回傳 -1；括號形式沒有檢查空主機，`[]` 會得到空字串主機。
  - 主機長度加 NUL 超過 `hostsz` 回傳 -1（兩種形式都檢查）。
  - 埠號以 `strtol` 解析，後面不可有多餘字元（`*end`）、必須在 1-65535 之間，`host:`（空埠）因 `p == 0` 被拒絕。
  - 此函式只做語法拆解，不做名稱解析；解析由 `lib/net.c` 的連線函式負責（`getaddrinfo`，`lib/net.c:100`）。
- **關係**：被誰呼叫 → `client/main.c:88`（`bench <host[:port]>`，預設埠 `BN_DEFAULT_PORT` = 9000）、`tools/bench-evil.c:472`；呼叫 → `strchr`、`strrchr`、`strtol`、`memcpy`。

### `hexdump_line`
*位置：`lib/util.c:216`　簽章：`int hexdump_line(char *out, size_t outsz, const uint8_t *p, size_t n)`*

- **用途**：把最多 16 個位元組格式化成一行 `xxd` 風格文字：16 欄十六進位、第 8 與第 9 欄之間多一個空格、再接 ` |ASCII|`，寫入 `out` 並回傳字串長度。
- **設計**：16 欄固定寬度，不足的欄位用三個空格補齊，所以不論 `n` 是多少，ASCII 欄都對齊。ASCII 欄把 0x20-0x7e 以外的位元組顯示為 `.`。每次 `snprintf` 後累加它的回傳值 `o`，並且在每一欄檢查 `o >= outsz`，一旦超出就提前回傳 `outsz`，避免後續的 `outsz - o` 變成負數（`size_t` 下溢）。ASCII 欄的迴圈條件 `o + 2 < outsz` 預留位置給結尾 `|` 與 NUL。
- **細節**：
  - 前提是 `n <= 16`：十六進位欄固定只寫 16 欄，但 ASCII 欄會寫 `n` 個字元，`n > 16` 時兩者不一致。呼叫端都保證不超過 16（`client/ui.c:188`、`hexdump`）。
  - 完整輸出長度最多 49（十六進位欄）+ 2（` |`）+ 16 + 1（`|`）= 68 字元加 NUL，呼叫端用 96 位元組的 buffer 足夠。
  - 在 buffer 過小的極端情況下，ASCII 欄可能在中途被截斷，此時函式不會補上結尾 NUL；實際的呼叫端 buffer 夠大，不會發生。
  - 回傳值型別為 `int`，正常情況是不含 NUL 的長度。
- **關係**：被誰呼叫 → `client/ui.c:188`（`ui_data` 在 `:hex` 模式下每 16 位元組輸出一行）、同檔 `hexdump`；呼叫 → `snprintf`。

### `hexdump`
*位置：`lib/util.c:239`　簽章：`void hexdump(FILE *fp, const void *data, size_t len, uint64_t base)`*

- **用途**：把一段記憶體以每行 16 位元組的 `xxd` 風格傾印到 `FILE *`，每行前面印出 8 位十六進位的位移（從 `base` 起算）。
- **設計**：迴圈每次取 `min(16, len - i)` 位元組交給 `hexdump_line` 格式化，自己只負責位移欄與換行。`base` 讓呼叫端可以印出邏輯位址（例如 ring 的邏輯位移），而不是從 0 開始。行緩衝 `line[96]` 放在堆疊上，大於 `hexdump_line` 的最大輸出。
- **細節**：
  - 格式為 `"%08llx  %s\n"`，位移轉成 `unsigned long long` 以配合 `%llx`。
  - `len == 0` 時什麼都不印。
  - 目前 repo 內沒有任何呼叫端（以 `grep` 搜尋 `server/`、`client/`、`lib/`、`tools/`、`scripts/` 都只找到宣告與定義），是備用的除錯輔助函式；客戶端的 `:hex` 模式用的是 `hexdump_line`，自己負責輸出。
- **關係**：被誰呼叫 → 無（僅宣告於 `include/bench_util.h:34`）；呼叫 → `hexdump_line`、`fprintf`。

### `bench_robust_name`
*位置：`lib/util.c:254`　簽章：`const char *bench_robust_name(int r)`*

- **用途**：回傳第 `r` 個健壯性機制（`enum bench_robust`，R_ZOMBIE = 0 到 R_RECONNECT = 11）的名稱字串，如 `"zombie"`、`"ctrl-release"`；`r` 超出範圍時回傳 `"?"`。
- **設計**：名稱表是檔內的 `static const char *robust_names[R_COUNT]`（`lib/util.c:249-252`），順序必須與 `bench_util.h` 的 `enum bench_robust` 一致，依序為 zombie、sigpipe、validate、readn、timeout、ctrl-release、robust-mutex、dev-reopen、max-clients、overrun、term-restore、reconnect，也就是 R1-R12。同一張表同時給 `--unsafe=` 的解析與格式化使用，客戶端與伺服器因此接受同一組名稱。把表設為 `static` 並只暴露這個查詢函式，避免外部修改。
- **細節**：
  - 同時檢查 `r >= 0` 與 `r < R_COUNT`，給非法值一個安全的字串而不是越界讀取。
  - 目前 repo 內沒有其他呼叫端（`bench_unsafe_parse` 與 `bench_unsafe_format` 直接讀 `robust_names`），保留作為 API。
- **關係**：被誰呼叫 → 無（僅宣告於 `include/bench_util.h:61`）；呼叫 → 無。

### `bench_unsafe_parse`
*位置：`lib/util.c:259`　簽章：`int bench_unsafe_parse(const char *list, uint32_t *mask)`*

- **用途**：解析 `--unsafe=` 的選項值（以逗號或空白分隔的名稱清單），把對應的位元 OR 進 `*mask`；遇到未知名稱回傳 -1，否則回傳 0。
- **設計**：每個 R1-R12 機制對應 `mask` 的一個位元（位元 `i` 對應 `robust_names[i]`），執行期以 `UNSAFE(r)` 巨集檢查（`bench_unsafe_mask >> r & 1`），所以同一個執行檔可以展示「有／沒有這個保護」的差別。每個 token 可以是：`all`（設定所有 `R_COUNT` 個位元，即 `(1u << R_COUNT) - 1`）、`none`（不做事，讓使用者可以明確寫「沒有」）、機制名稱（例如 `readn`），或編號（`R1`-`R12`），比對都不分大小寫（`strcasecmp`）。輸出採 OR 累加，因此重複給 `--unsafe` 多次的結果會合併，呼叫端在解析前先把 `unsafe` 初始化為 0（`server/main.c:280`、`client/main.c:44`）。
- **細節**：
  - 輸入先用 `bench_strlcpy` 複製到 `buf[256]`，因為 `strtok_r` 會改寫字串，不能動 `argv`；超過 255 字元會被截斷（此長度遠大於所有名稱連起來的長度 117）。
  - 使用可重入的 `strtok_r`（`save` 是區域變數），分隔字元為 `","` 與 `" "`。
  - 遇到未知 token 立即回傳 -1，此時 `*mask` 可能已含前面合法 token 的位元；兩個呼叫端失敗都會印錯誤訊息並結束程式（`server/main.c:303-306`、`client/main.c:52-55`），所以不會用到這個半成品。空字串回傳 0 且不改 `*mask`。
  - 這個表由 server 與 client 共用；`bench` 只實作 `term-restore`、`reconnect`、`readn`、`validate`、`sigpipe`，`benchd` 對客戶端專屬的名稱也照樣接受但不使用（見 DECISIONS 的 Robustness switches）。
  - 呼叫端接下來才把結果存入全域 `bench_unsafe_mask`：`server/main.c:329` 在任何 `fork()` 之前設定，所以所有子行程繼承；`client/main.c:73`。
- **關係**：被誰呼叫 → `server/main.c:303`（`benchd --unsafe`）、`client/main.c:52`（`bench --unsafe`）；呼叫 → `bench_strlcpy`、`strtok_r`、`strcasecmp`、`snprintf`。

### `bench_unsafe_format`
*位置：`lib/util.c:285`　簽章：`void bench_unsafe_format(uint32_t mask, char *buf, size_t n)`*

- **用途**：把 `mask` 轉回人類可讀字串：已啟用的名稱以逗號串接（例如 `zombie,sigpipe`），`mask` 為空時輸出 `(none)`。
- **設計**：是 `bench_unsafe_parse` 的反向操作，用於告知使用者目前哪些保護被關掉：伺服器啟動時的警告橫幅（`print_unsafe_banner`）與 `INFO` 回覆中的 `server.unsafe=` 一行（客戶端因此能看到伺服器的設定）。依 `i` 由小到大掃描，輸出順序固定為 R1 到 R12。`o` 記錄已寫入的長度，同時決定是否要加分隔逗號（`o ? "," : ""`）。
- **細節**：
  - 一開始先 `buf[0] = 0`，沒有任何位元時 `o == 0`，最後以 `(none)` 覆蓋。
  - 每次 `snprintf` 若回傳值 `w < 0` 或 `w >= n - o`（放不下）就停止迴圈；此時 `snprintf` 已把被截斷的名稱寫進 `buf`，但 `o` 不會增加，所以截斷的名稱片段會留在輸出中（只有當第一個名稱就放不下時，因 `o == 0` 會被 `(none)` 蓋掉）。全部名稱串起來是 117 字元，呼叫端的 256 位元組 buffer 不會發生。
  - 高於 `R_COUNT` 的位元被忽略。`n` 必須大於 0，否則 `buf[0] = 0` 會越界。
- **關係**：被誰呼叫 → `server/robust.c:94` 的 `print_unsafe_banner`（stderr 橫幅，`list[256]`）、`server/sysinfo.c:138`（INFO 的 `server.unsafe`，使用 shm 中的 `unsafe_mask`）；呼叫 → `snprintf`。

---

## `server/main.c`

`server/main.c` 是 benchd 的 master 行程：`main()` 解析命令列、建立共享記憶體與 input pipe、監聽 TCP、fork 出 reader，之後在 `poll()` 迴圈裡 accept 並為每個 client fork 一個 child（child 執行 `server/session.c` 的 `session_main()`）。這支檔案也負責 SIGCHLD 的收屍（R1）、SIGUSR1 切換 log level、SIGINT/SIGTERM 的有序關機。所有共享的狀態都在 fork 之前建立，所以 reader 與每個 child 都繼承同一塊 `MAP_SHARED` 映射、同一個 robust mutex 和同一條 pipe（見 `server/server.h` 開頭的說明）。本檔案定義全域變數 `g_shm`（共享記憶體指標）和 `g_cfg`（命令列組態），其他 server 檔案以 `extern` 使用；它依賴 `server/robust.c`（鎖、slot 計數）、`server/control.c`（釋放控制權）、`server/ring.c`（寫入事件）、`server/reader.c`（`reader_main`、`baud_supported`）、`server/session.c`（`session_main`）以及 `lib/` 的 log、net、proto、util 函式。

### `on_chld`
*位置：`server/main.c:40`　簽章：`static void on_chld(int sig)`*

- **用途**：SIGCHLD 的 handler，只把旗標 `got_chld` 設成 1。
- **設計**：handler 內不呼叫 `waitpid()`、不寫 log、不拿共享記憶體的鎖，因為這些都不是 async-signal-safe，而且 handler 可能正好打斷 master 在 `shm_lock()` 之內的程式。真正的收屍延後到主迴圈的 `reap_children()`（R1 zombie 機制，見 `main()`）。同一檔案的 `on_usr1` 採同樣模式。
- **細節**：
  - `got_chld` 是 `volatile sig_atomic_t`（line 36），寫入一個整數旗標是 async-signal-safe。
  - 多個 child 幾乎同時結束時 SIGCHLD 可能合併成一次，所以旗標只表示「至少有一個 child 狀態改變」；`reap_children()` 用 `while` 迴圈一次收完。
  - `--unsafe=zombie` 時 `main()` 不安裝這個 handler（line 392），旗標永遠不會被設，child 結束後變成 `<defunct>`。
- **關係**：被誰呼叫 → 核心在 SIGCHLD 送達時呼叫（由 `main()` 的 `sigaction` 安裝，line 394-396）；呼叫 → 無。

### `on_usr1`
*位置：`server/main.c:41`　簽章：`static void on_usr1(int sig)`*

- **用途**：SIGUSR1 的 handler，只設定旗標 `got_usr1`。
- **設計**：與 `on_chld` 相同，handler 內不做事，讓主迴圈呼叫 `cycle_log_level()`。這樣改 log level 時寫的 log（會做 I/O）不會發生在 signal handler 裡。
- **細節**：旗標型別為 `volatile sig_atomic_t`。由 `main()` 以 `SA_RESTART` 安裝（line 389-391）。reader 與 child 啟動後都把 SIGUSR1 設為 `SIG_IGN`（`reader.c:211`、`session.c:435`），所以只有 master 會響應。
- **關係**：被誰呼叫 → 核心（使用者 `kill -USR1 <master>`）；呼叫 → 無。

### `on_term`
*位置：`server/main.c:42`　簽章：`static void on_term(int sig)`*

- **用途**：SIGINT 與 SIGTERM 共用的 handler，把收到的訊號編號存進 `got_term`。
- **設計**：存的是訊號編號而不是布林值，這樣 `shutdown_server()` 可以在 log 裡印出是哪個訊號（`"signal %d: shutting down"`）。`got_term` 非 0 也是主迴圈 `while (!got_term)` 的結束條件，並讓 `reap_children()` 不要重啟 reader。後者只在一種時序下有意義：同一輪迴圈裡 `got_chld` 與 `got_term` 同時為真（例如 reader 剛好在收到 SIGTERM 前後死亡），此時 `reap_children()` 先於迴圈結束條件執行；一旦進入 `shutdown_server()`，就不再呼叫 `reap_children()`，由該函式自己的 `waitpid` 迴圈收屍。
- **細節**：
  - 以 `sa_flags = 0` 安裝（line 386-388），不帶 `SA_RESTART`；`poll()` 本來就會被訊號打斷並回傳 -1/EINTR，主迴圈接著檢查旗標。
  - child 與 reader 都忽略 SIGINT（`session.c:434`、`reader.c:210`），所以終端機的 Ctrl-C 只會到 master，由 master 統一協調關機（見 `docs/DECISIONS.md` Server 一節）。
- **關係**：被誰呼叫 → 核心（SIGINT/SIGTERM）；呼叫 → 無。注意 `reader.c` 有另一個同名但獨立的 static `on_term`。

### `usage`
*位置：`server/main.c:44`　簽章：`static void usage(FILE *f)`*

- **用途**：把使用說明印到指定的 `FILE *`，列出所有選項、預設值與支援的 signal。
- **設計**：用參數 `f` 決定輸出位置：`-h`/`--help` 是使用者要求的，印到 stdout 並以 0 結束；選項錯誤或缺 `-d` 是錯誤，印到 stderr 並以 1 結束（見 `main()`）。
- **細節**：
  - 文字中寫明的預設值與 `main()` 一致：baud 115200、port 9000、history 64K、max-clients 32、idle-release 120（0 表示不自動釋放）。
  - `--unsafe` 一行只列出 `zombie sigpipe validate readn timeout ctrl-release robust-mutex dev-reopen max-clients overrun` 十個伺服器端名稱；`bench_unsafe_parse()` 另外接受 `term-restore`、`reconnect`（client 專用，server 收到會忽略）、`all`、`none` 以及 `R1`..`R12` 形式。
- **關係**：被誰呼叫 → `main()`（line 309-310、314）；呼叫 → `fprintf`。

### `spawn_reader`
*位置：`server/main.c:64`　簽章：`static void spawn_reader(void)`*

- **用途**：fork 出 reader 行程（唯一持有序列埠 fd 的行程），並在 master 與共享記憶體中記下它的 pid。
- **設計**：reader 必須是獨立行程且是唯一碰序列埠的人，這樣不會有兩個行程互相搶 UART 的 bytes（README Architecture）。把它包成函式是因為有兩個呼叫點：啟動時一次，reader 意外死亡時 `reap_children()` 再呼叫一次重啟。`g_shm->reader_pid` 把 reader 的 pid 也記在共享記憶體裡（欄位定義在 `server.h`；在本 repo 的 server 程式中只有這裡寫入），寫入在鎖內完成，與其他共享欄位的規則一致。
- **細節**：
  - `fork()` 失敗：`LOGE` 後 `exit(1)`，master 無法在沒有 reader 的狀況下提供服務。
  - child 分支呼叫 `reader_main()`（`noreturn`，定義在 `reader.c:203`），不會回到這裡。
  - master 的分支設定 `reader_pid`、在 `shm_lock`/`shm_unlock` 之間寫 `g_shm->reader_pid`，鎖釋放後才 `LOGI`（鎖內不做 I/O，見 DECISIONS「Threads and synchronisation」）。
  - `spawn_reader()` 在 `main()` 裡是在 `sigaction` 安裝完之後才呼叫（line 399），reader 因此繼承 handler 後在 `reader_main()` 內自己覆蓋（SIGINT/SIGUSR1 忽略、SIGTERM 用自己的 `on_term`）。
  - 重啟 reader 時，新的 reader 重新 `dev_open()`；input pipe 和共享記憶體是同一份。
- **關係**：被誰呼叫 → `main()`（line 399）、`reap_children()`（line 90）；呼叫 → `reader_main`（`server/reader.c`）、`shm_lock`/`shm_unlock`（`server/robust.c`）。

### `reap_children`
*位置：`server/main.c:80`　簽章：`static void reap_children(void)`*

- **用途**：以 `waitpid(-1, &st, WNOHANG)` 迴圈收掉所有已結束的子行程；若是 reader 就重啟，若是 client child 就清掉它的 slot，並在它是控制者時釋放控制權。
- **設計**：
  - 在主迴圈（非 signal handler）裡做，因為需要拿 `shm_lock` 與寫 log；`WNOHANG` 搭配 `while` 是因為多個 SIGCHLD 可能合併成一個，一次只收一個會漏收（R1）。
  - reader 死亡走獨立分支：記錄 `LOGE`，`reader_pid = 0`，若不是在關機（`!got_term`）就 `spawn_reader()`；已在關機（`got_term` 非 0）時不重啟，因為新 reader 會重新 `dev_open()` 序列埠，卻馬上又要被 `shutdown_server()` 終止，沒有意義。
  - 對 client child：「控制者當機」是 master 才看得到的情況（child 正常離開時自己已經透過 pipe 送出 RELEASE，見 `session.c:finish`），所以這裡檢查 `ctrl_pid == pid` 並直接在共享記憶體裡釋放，不經 reader（DECISIONS：master 的 SIGCHLD 路徑直接釋放）。這是 R6 的四條釋放路徑之一（斷線、SIGCHLD、TAKE 時 `kill(pid,0)`、閒置逾時）。
  - slot 按 pid 全表掃描清除：child 可能在握手前就死掉，此時它自己沒機會清 slot；master 在 `handle_accept` 已把真正的 pid 寫進去，所以這裡能依 pid 找到並歸零。清完呼叫 `slots_recount()` 更新 `viewers`。
  - `ctrl_release_locked()` 只把結果填進 `struct ctrl_note`，真正的 log 在 `shm_unlock` 之後由 `ctrl_log_note()` 輸出（鎖內不做 I/O）。
- **細節**：
  - 迴圈條件 `pid > 0`：沒有子行程可收時 `waitpid` 回傳 0（WNOHANG 無已結束者）或 -1（ECHILD），迴圈結束。
  - 結束原因 log：被訊號殺死用 `LOGW`（附 `strsignal`），一般結束用 `LOGD`。reader 的 `LOGE` 會依 `WIFSIGNALED` 印 `signal` 或 `status` 與數值。
  - `--unsafe=ctrl-release`（R6）：`UNSAFE(R_CTRL_RELEASE)` 為真時跳過釋放，控制者當機後 `ctrl_pid` 永遠留在共享記憶體，沒有人能再取得控制權。
  - `--unsafe=zombie`（R1）：`main()` 不安裝 SIGCHLD handler，這個函式在主迴圈中永遠不會被呼叫（只有 `got_chld` 被設才會呼叫），因此 child 不被收屍；reader 死亡也不會被察覺。關機時 `shutdown_server()` 自己的 `waitpid` 迴圈才會收掉它們。
- **關係**：被誰呼叫 → `main()` 主迴圈（line 411，條件為 `got_chld`）；呼叫 → `spawn_reader`、`shm_lock`/`shm_unlock`、`slots_recount`（`server/robust.c`）、`ctrl_release_locked`、`ctrl_log_note`（`server/control.c`）。

### `reject_busy`
*位置：`server/main.c:114`　簽章：`static void reject_busy(int fd, const char *peer, int in_use)`*

- **用途**：伺服器已滿時，嘗試讀出對方 HELLO 裡的名字（最多等 100 ms），回傳 `ERROR SERVER_BUSY`，記錄 log 後關閉連線。
- **設計**：
  - 先 `poll(fd, 100 ms)` 再 `recv(MSG_DONTWAIT)` 偷看 HELLO 開頭，只為了讓 log 能寫出被拒絕的是誰；沒收到就名字維持 `"?"`。只讀 `BN_NAME_LEN` 位元組的名字，名字經 `sanitize_name()` 清成可印 ASCII，避免把控制字元寫進 log。
  - 用 `n >= BN_HDR_LEN + BN_NAME_LEN`（12 + 32）判斷，因為 `struct bn_hello` 的第一個欄位就是 `name[BN_NAME_LEN]`，header 後面緊接著名字；資料不足就不取名字。
  - 這是 R2（sigpipe）的實際觸發路徑：若 client 已經 RST，`recv()` 會消耗掉 ECONNRESET 錯誤，接下來的 `bn_send_error()` 的 `write()` 就會收到 SIGPIPE。master 沒忽略 SIGPIPE 就會被殺死（DECISIONS「Robustness switches」R2）。
- **細節**：
  - `recv` 回傳 -1 時只寫一行 `LOGD`（含 `strerror(errno)`），不影響後續流程。
  - `bn_send_error` 回傳 < 0 只記 `LOGW(... (ignored))`，隨後仍然 `close(fd)`。
  - 錯誤訊息是 `"server busy (%d/%u clients)"`，帶目前使用量與上限。
  - 這個函式是在 master 行程內同步執行，最壞會讓 master 的 accept 迴圈停 100 ms（poll 逾時）。
  - `--unsafe=sigpipe`：`main()` 把 SIGPIPE 設回 `SIG_DFL`，上述 write 會讓 master 以 SIGPIPE 結束（demo 中 status 141）。`--unsafe=max-clients` 時 `handle_accept` 根本不會呼叫這個函式。
- **關係**：被誰呼叫 → `handle_accept`（line 170）；呼叫 → `sanitize_name`（`lib/util.c`）、`bn_send_error`（`lib/proto.c`）。

### `handle_accept`
*位置：`server/main.c:134`　簽章：`static void handle_accept(int *conn_seq)`*

- **用途**：接受一條新連線，保留一個 slot，檢查 `--max-clients`，然後 fork 一個 child 執行 `session_main()`；master 這邊負責關掉 fd 並把 child 的 pid 填進 slot。
- **設計**：
  - **accept 重試**：`accept()` 被訊號打斷（EINTR）就重試。此行為永遠開啟，沒有 `--unsafe` 開關（DECISIONS 最後一條）。其他失敗直接 return；`EAGAIN` 與 `ECONNABORTED`（連線在 accept 前就中止）是正常現象，不記 log；`EMFILE`/`ENFILE`（fd 用完）會 `sleep_ms(100)`，否則 listening socket 仍然可讀，主迴圈會空轉吃滿 CPU。
  - **slot 保留（pid = -1）**：master 在鎖內先算 `in_use`（`slots_in_use()`，計 `pid != 0` 的 slot），再找第一個 `pid == 0` 的 slot 並暫時設成 -1（原始碼註解：reserved until fork() returns），鎖放開後才 fork。-1 讓這個 slot 在 fork 回來之前不再顯示為空。fork 後 master 再把 -1 換成 child 的真正 pid（child 在握手時也會把自己的 pid 寫進同一個 slot，見 `session.c:246`），這樣尚未握手的連線也被 `slots_in_use()` 計入，`reap_children()` 能依 pid 找到 slot，`shutdown_server()` 的 `pid > 0` 清單也包含這個 child。
  - `in_use` 是保留自己的 slot 之前的計數，所以 `in_use >= max_clients` 表示「在我之前已經滿了」。
  - **R9 檢查**：超過上限時歸還 slot（設回 0），呼叫 `reject_busy()`，不 fork。
  - **fork 失敗**：記 `LOGE`，用 `BN_ERR_SERVER_BUSY` 通知 client（訊息 `"server cannot fork: ..."`），關 fd，歸還 slot。
  - fork 之前鎖已放開；整個函式只在短暫的臨界區內動共享記憶體。
  - child 分支呼叫 `session_main(fd, slot, seq, peer)`（`noreturn`），所以後面的程式只有 master 會執行。child 的 `seq` 用來給 fault injection 比對是第幾條連線（`session.c:291`）。
- **細節**：
  - `conn_seq` 由 `main()` 持有，在 fork 前遞增（`++*conn_seq`）；fork 失敗時 seq 已被用掉。
  - `accept` 填入的 `sockaddr_storage` 這裡沒有用到；peer 字串由 `sock_peer_str(fd, ...)` 自行取得，存進 `peer[64]`。
  - master 在 fork 後立刻 `close(fd)`，因為連線屬於 child；只有 child 持有該 fd，client 斷線時才會真正收到 FIN。
  - 回填 pid 時檢查 `slots[slot].pid == -1`：如果 child 已經握手完成寫了自己的 pid（或已經清除成 0），就不覆蓋。
  - slot 表滿（`slot < 0`，1024 個全被占用）時，`--max-clients` 上限 `main()` 已 clamp 到 `BENCH_MAX_SLOTS`，所以只有在 `--unsafe=max-clients` 時才會在沒有 slot 的情況下繼續 fork；此時 child 以 `slot = -1` 執行，不做 slot 登記（`session.c` 各處都檢查 `s->slot >= 0`）。
  - `--unsafe=max-clients`（R9）：略過上限檢查（line 164），不再因人數拒絕連線；之後只有 `fork()` 本身失敗時才走 fork 失敗路徑。
- **關係**：被誰呼叫 → `main()` 主迴圈（line 418）；呼叫 → `sock_peer_str`、`sleep_ms`（`lib/`）、`shm_lock`/`shm_unlock`、`slots_in_use`（`server/robust.c`）、`reject_busy`、`bn_send_error`、`session_main`（`server/session.c`）。

### `cycle_log_level`
*位置：`server/main.c:199`　簽章：`static void cycle_log_level(void)`*

- **用途**：SIGUSR1 到達時把共享的 log level 依序切換 INFO → DEBUG → TRACE → ERROR → WARN → INFO。
- **設計**：
  - level 存在 `g_shm->log_level`（`main()` 以 `bench_log_set_level_ptr` 把 log 模組的指標指到這裡，line 360），所以 master、reader、所有 child 讀的是同一個值，改一次全部立即生效，不需要對每個行程各送訊號。
  - 用 `next[]` 陣列對照表（索引為目前 level：ERROR→WARN、WARN→INFO、INFO→DEBUG、DEBUG→TRACE、TRACE→ERROR），比算數運算更清楚。目前值超出範圍（< 0 或 > `LOG_TRACE`）時當作 INFO 處理。
  - WARN 是為了讓循環經過每個 level 才加進來的（DECISIONS「Logging」）。
- **細節**：
  - 直接寫 `g_shm->log_level` 沒有拿鎖：它是單一 `volatile int`，其他行程只讀取，不需要與別的欄位保持一致。
  - 新 level 高於編譯時上限（`bench_log_compile_level()`，release 為 2 = INFO，debug 為 4）時仍接受設定，但 log 會加註 `note: this build only contains messages up to ...`，因為更詳細的訊息在這個 binary 裡不存在（巨集被編掉）。
  - 這則訊息用 `LOGW` 輸出，而 log 的判斷用的是已經更新的 level：切到 ERROR（0）時 WARN 訊息會被自己壓掉，不會顯示。
- **關係**：被誰呼叫 → `main()` 主迴圈（line 415，條件為 `got_usr1`）；呼叫 → `bench_log_compile_level`、`bench_log_level_name`（`lib/log.c`）。

### `shutdown_server`
*位置：`server/main.c:212`　簽章：`static void shutdown_server(void)`*

- **用途**：有序關機：關閉監聽 socket、在 ring 寫入 `EV_SERVER_SHUTDOWN` 事件並等 child 送出，然後 SIGTERM 所有 child 與 reader、收屍，必要時 SIGKILL，最後 `munmap`。
- **設計**：
  - 先 `close(listen_fd)` 不再接受新連線。
  - 事件寫進 ring（`ring_event`，需持鎖）而不是由 master 直接對每個 socket 送訊息：所有 client 本來就從 ring 讀資料，用同一條路徑傳達關機通知，順序與資料一致（README：每個 child 以自己的 offset 讀 ring）。寫完在鎖內記下 `target = head`，也就是事件之後的 ring 位置。
  - 等待迴圈最長 1 秒（`mono_ms() + 1000`），每 20 ms 檢查一次共享記憶體中所有「已握手（`joined`）且 `pid > 0`」的 slot，若 `offset < target` 表示還沒送完。全部送完就提前結束。`session.c:148` 存入 slot 的是每一輪複製**開始前**的位置，所以最後一輪結束後 child 還要再多跑一輪才會更新到 `target`；這只會讓等待略長，不會漏送。尚未握手的 child 不會被等。
  - 在鎖內把所有 `pid > 0` 的 slot 複製到區域陣列 `pids[]`，鎖外再 `kill(SIGTERM)`，避免持鎖時做系統呼叫過久。**不用 `kill(0, ...)`**：從 shell script 啟動 benchd 時它和 script 同一個 process group，群組 kill 會連 script 一起殺掉（DECISIONS Server 一節）。
  - reader 若還活著（`reader_pid != 0`）也送 SIGTERM；reader 的 handler 會設旗標並 `alarm(2)`，萬一卡在持有者已死的鎖上（`--unsafe=robust-mutex` 的情形），2 秒後 SIGALRM 的預設動作仍會結束它（`reader.c:34-41`）。child 的 SIGTERM 是預設動作，直接終止。
  - 收屍迴圈：`waitpid(-1, NULL, WNOHANG)` 回 -1 且 errno 為 ECHILD 表示再也沒有子行程，結束；否則 sleep 10 ms 後重試。超過 2 秒（`deadline = now + 2000`）就對 `pids[]` 與 reader 送 SIGKILL，並把 `deadline` 設成 `UINT64_MAX`，確保只送一次。
- **細節**：
  - 這個函式內的 `waitpid` 會順便收掉任何尚未被 `reap_children()` 處理的 child，因此關機時 `got_chld` 旗標被忽略沒有問題；`--unsafe=zombie` 下的 zombie 也在這裡被清掉。
  - `pids[]` 在關機開始之後建立，其間 master 已不再 fork，所以清單不會再增加。
  - 進入時 `got_term` 非 0，`LOGI("signal %d: shutting down")` 印出訊號編號。結束前 `LOGI("bye")` 並 `munmap(g_shm, shm_size)`；之後 `main()` 回傳 0。
  - slot 仍是 -1（保留但 pid 尚未回填）的項目不在 `pid > 0` 清單中；master 是單一行程，`handle_accept` 在同一個迴圈裡執行，進入 `shutdown_server()` 時不會有 `handle_accept` 做到一半。
- **關係**：被誰呼叫 → `main()`（line 420，主迴圈因 `got_term` 離開之後）；呼叫 → `ring_event`（`server/ring.c`）、`shm_lock`/`shm_unlock`、`mono_ms`、`sleep_ms`（`lib/util.c`）。

### `main`
*位置：`server/main.c:270`　簽章：`int main(int argc, char **argv)`*

- **用途**：benchd master 的入口：解析並驗證選項、建立共享記憶體與 pipe、建立監聽 socket、安裝 signal handler、啟動 reader，然後進入 accept 主迴圈，結束時呼叫 `shutdown_server()`。
- **設計**：
  - **選項**：`getopt_long`，短選項 `d b p l v h`；`--history`、`--max-clients`、`--idle-release`、`--unsafe` 沒有短選項，用 1、2、3、4 作為內部代碼。預設值：baud 115200、port `BN_DEFAULT_PORT`（9000）、history 64 KiB、max_clients 32、idle_release 120 秒。數字用 `strtoul` 轉換且不檢查尾端，無法解析的值會變成 0；`--history` 用 `parse_size()`（支援 K/M/G 後綴），失敗印錯誤並 `return 1`；`--unsafe` 用 `bench_unsafe_parse()`，不認得的名字同樣失敗。`-h` 印使用說明到 stdout 並回傳 0，其他未知選項印到 stderr 並回傳 1。
  - **驗證與夾限**：必須有 `-d`；`baud_supported()`（`reader.c`）檢查 baud 是否在表中（1200 到 3000000），不支援則結束。`history` 最小 4096，並向上取整到 8 的倍數（`(h + 7) & ~7`），對應 ring 記錄以 8 位元組對齊（`REC_SIZE` 巨集）。`max_clients` 夾在 1 到 `BENCH_MAX_SLOTS`（1024）之間，因為 slot 表只有這麼多格。`idle_release` 為 0 表示不自動釋放。
  - **全域設定的順序**：`bench_unsafe_mask = unsafe` 要在所有 fork 前設定，所有行程才會繼承同一份開關（註解：inherited by every fork()ed process）；之後依序設定 log 角色 `"master"`、`bench_log_init_level(verbose)`（INFO，環境變數 `BENCH_LOG`，再加 `-v` 次數）、開 `-l` 指定的 log 檔、印出 `print_unsafe_banner()`。
  - **R2**：安全模式 `signal(SIGPIPE, SIG_IGN)`，寫入已死的連線回傳 EPIPE 而非殺掉 master；unsafe 模式明確設回 `SIG_DFL`，因為被忽略的 disposition 會跨 `exec` 繼承（例如 Python 啟動器），不設回的話 unsafe demo 可能偷偷表現得像 safe（DECISIONS）。
  - **共享記憶體**：大小 = `sizeof(struct bench_shm) + history`，`mmap(MAP_SHARED | MAP_ANONYMOUS)`，匿名映射預設全 0，所以所有 slot 一開始都是空的（`pid == 0`）。必須在第一次 `fork()` 之前建立，child 才會看到同一塊。接著 `shm_mutex_init()`（process-shared、robust，除非 `--unsafe=robust-mutex`）、`ring_init()`、初始 `dev_state = DEV_LOST`（reader 開成功才改為 `DEV_UP`）、並寫入 device 路徑、baud、idle_release、max_clients、啟動時間、`unsafe_mask`、`master_pid`。`master_pid` 供 reader 與 child 啟動時以 `getppid() != master_pid` 檢查自己是否已成孤兒。`bench_log_set_level_ptr(&g_shm->log_level)` 讓全部行程共用一個 log level。
  - **pipe**：`pipe(g_cfg.input_pipe)` 在 fork 之前建立，child 寫、reader 讀。
  - **fault injection**：只有 debug build 編進來（`-DBENCH_FAULT_INJECTION`，見 `Makefile`）。環境變數 `BENCH_FAULT=die_in_lock[:N]` 設定 `g_cfg.fault_die_in_lock_at`（預設第 1 條連線），由 `session.c:291` 使用，用來示範 R7。
  - **監聽**：`tcp_listen(port, 128)`（`lib/net.c`，預設建 IPv6 雙堆疊 socket（`IPV6_V6ONLY=0`），僅在 `socket(AF_INET6)` 失敗時改用 IPv4；`SO_REUSEADDR` 永遠開啟）；失敗時 `LOGE` 後結束。
  - **signal 設定**：SIGINT/SIGTERM → `on_term`（無 `SA_RESTART`）；SIGUSR1 → `on_usr1`（`SA_RESTART`）；SIGCHLD → `on_chld`（`SA_RESTART | SA_NOCLDSTOP`，只在 child 結束時通知，不因暫停/繼續而觸發），且僅在**沒有** `--unsafe=zombie` 時才安裝（R1）。handler 全部只設旗標。
  - **主迴圈**：`poll(listen_fd, 200 ms)`。每輪先處理 `got_chld`（`reap_children`）、再處理 `got_usr1`（`cycle_log_level`），最後若 `poll` 回傳 > 0 且沒有在關機才 `handle_accept()`。逾時 200 ms 的作用是：即使旗標在檢查之後、進入 `poll` 之前才被設定而沒有打斷 poll，最多 200 ms 後迴圈也會再檢查旗標。reap 放在 accept 之前，使剛結束的 child 先釋放 slot，再計算 `--max-clients`。
- **細節**：
  - 啟動前的失敗（參數錯誤、`mmap`、`pipe`、開 log 檔、`tcp_listen`）都 `return 1`，不留下已經 fork 的行程，因為 `spawn_reader()` 在這些之後才呼叫。
  - 啟動 log 一行列出版本 `BENCH_VERSION`、port、device、baud（8N1）、history（`fmt_size`）、max clients、idle-release。
  - 迴圈在 `got_term` 變成非 0 時離開，`main()` 呼叫 `shutdown_server()` 後 `return 0`。
  - `--unsafe` 對此函式的影響彙整：`zombie`（不安裝 SIGCHLD handler）、`sigpipe`（SIGPIPE 設回預設）、`robust-mutex`（`shm_mutex_init` 不設 robust）；`max-clients`、`ctrl-release` 在其他函式中生效，`unsafe_mask` 由此處寫進共享記憶體，`sysinfo.c:112` 讀取它填入狀態。
- **關係**：被誰呼叫 → C runtime；呼叫 → `getopt_long`、`parse_size`、`bench_unsafe_parse`、`baud_supported`、`print_unsafe_banner`、`shm_mutex_init`、`ring_init`、`tcp_listen`、`spawn_reader`、`reap_children`、`cycle_log_level`、`handle_accept`、`shutdown_server`、`usage`。

---

## `server/reader.c`

這個檔案是 benchd 的「reader 行程」，也是整個伺服器中唯一持有序列埠 fd 的行程（`tty_fd`）。它由 master 的 `spawn_reader()`（`server/main.c:64`）在 `fork()` 之後呼叫 `reader_main()` 進入，之後永不返回（以 `_exit(0)` 結束）；master 在 reader 死亡且不是在關機流程（`got_term` 為假）時會重新 fork 一個（`server/main.c:85-90`）。reader 做三件事：(1) 把 tty 讀到的位元組以 `REC_DATA` 記錄寫進共享記憶體的 ring；(2) 從「輸入 pipe」取出各 child 轉送來的 `PIPE_INPUT` / `PIPE_BAUD` / `PIPE_RELEASE` 訊息並執行；(3) 裝置拔除時關閉 fd、寫入 `EV_DEVICE_LOST` 事件，之後每 500 ms 嘗試重新 `open()`（R8 `dev-reopen`），並每秒檢查控制權是否閒置過久。只用一個行程碰 tty，是因為兩個行程同時 `read()` 同一個 tty 會互相搶走位元組。檔案依賴 `server/server.h`（`g_shm`、`g_cfg`、`struct pipe_msg`）、`server/ring.c`（`ring_append`、`ring_event`）、`server/robust.c`（`shm_lock`/`shm_unlock`）、`server/control.c`（`ctrl_release_locked`、`ctrl_check_idle_locked`、`ctrl_log_note`）以及 libbench 的 `readn`、`mono_ms`、`now_ns`、`bench_strlcpy`。檔案內的靜態狀態有：`tty_fd`（目前的 tty fd，-1 表示裝置不在）、`orig_tio`/`have_orig`（第一次成功 `tcgetattr` 時保存的原始 termios）、`reader_stop`（`volatile sig_atomic_t`，由 SIGTERM handler 設定）、`baud_table[]`（13 種支援的鮑率對照表）。

### `on_term`
*位置：`server/reader.c:36`　簽章：`static void on_term(int sig)`*

- **用途**：reader 的 SIGTERM 處理函式：要求主迴圈結束，並設定 2 秒的保險鬧鐘。
- **設計**：handler 只做兩件 async-signal-safe 的事：把 `reader_stop`（`volatile sig_atomic_t`）設為 1，以及呼叫 `alarm(2)`。實際的收尾（還原 termios、記錄訊息）留給主迴圈在 handler 之外做，因為 `LOGI`、`tcsetattr`、`shm_lock` 都不能在 handler 裡安全呼叫。`alarm(2)` 是保險：若 reader 卡在 `shm_lock()` 而回不到迴圈（例如 `--unsafe=robust-mutex` 下鎖的持有者已死，見 docs/DECISIONS.md「The reader's SIGTERM handler also arms `alarm(2)`」），2 秒後 SIGALRM 的預設動作會終結行程，master 的關機流程就不會被 reader 拖住。
- **細節**：
  - 參數 `sig` 未使用（`(void)sig`）。
  - 整個 `server/` 與 `lib/` 中沒有任何地方為 SIGALRM 安裝 handler，所以 SIGALRM 保持預設動作（終止行程）。
  - 安裝時 `struct sigaction sa = { .sa_handler = on_term }`，未設 `SA_RESTART`；`poll()` 被訊號打斷時本來就回傳 `EINTR`，迴圈每輪開頭都會檢查 `reader_stop`，所以最壞情況的反應延遲受 100 ms 的 poll 逾時限制。
  - 正常路徑下迴圈很快結束並 `_exit(0)`，鬧鐘不會真的觸發。
- **關係**：被誰呼叫 → 由 `reader_main()`（`server/reader.c:213`）以 `sigaction(SIGTERM, ...)` 註冊，由核心在 SIGTERM 到達時呼叫（master 的 `shutdown_server()` 流程對 `reader_pid` 送 SIGTERM，見 `server/main.c:247-248`；master 死亡時則由 `PR_SET_PDEATHSIG` 觸發）；呼叫 → `alarm()`。

### `speed_of`
*位置：`server/reader.c:49`　簽章：`static speed_t speed_of(uint32_t baud)`*

- **用途**：把數字鮑率（如 115200）換成 termios 的 `speed_t` 常數（如 `B115200`）；不支援時回傳 0。
- **設計**：用 `baud_table[]`（`server/reader.c:43-47`，共 13 筆：1200、2400、4800、9600、19200、38400、57600、115200、230400、460800、921600、1500000、3000000）做線性搜尋，表很小所以不需要雜湊或二分。把「是否支援」與「對應常數」放在同一張表，是為了讓 `baud_supported()` 與 `apply_termios()` 永遠看到同一份清單，不會一邊接受、另一邊卻不會設定。
- **細節**：
  - 用 0 當「不支援」的哨兵值是安全的：`B0` 在 termios 中的值就是 0，代表「掛斷」，不是一個可用鮑率。
  - 迴圈索引型別為 `size_t`，長度以 `sizeof baud_table / sizeof baud_table[0]` 計算。
- **關係**：被誰呼叫 → `baud_supported()`、`apply_termios()`（同檔）；呼叫 → 無。

### `baud_supported`
*位置：`server/reader.c:57`　簽章：`int baud_supported(uint32_t baud)`*

- **用途**：對外（跨檔）提供的鮑率檢查：回傳非 0 表示 benchd 支援此鮑率。
- **設計**：這是檔案中唯一非 `static` 的輔助函式（宣告在 `server/server.h`「baud helpers」），因為 master 與 child 也需要驗證鮑率，而鮑率表只放在 reader.c 一處。真正的檢查委託給 `speed_of()`，避免重複表格。
- **細節**：
  - 回傳 `speed_of(baud) != 0`，即 0 或 1。
  - 這個檢查發生在請求的來源端，而不是 reader：master 啟動時驗證 `--baud`（`server/main.c:317`，不支援就印錯並 `return 1`），child 收到 `BN_BAUD` 時驗證（`server/session.c:401`，不支援就回 `BN_ERR_BAD_REQUEST`，連線保持開啟）。`handle_pipe_msg()` 不會再檢查一次。
- **關係**：被誰呼叫 → `server/main.c`（啟動參數驗證）、`server/session.c`（`BN_BAUD` 處理）；呼叫 → `speed_of()`。

### `apply_termios`
*位置：`server/reader.c:59`　簽章：`static int apply_termios(int fd, uint32_t baud)`*

- **用途**：把 tty 設成 raw、8N1、無流量控制，並套用指定鮑率；成功回 0，失敗回 -1。
- **設計**：先 `tcgetattr()` 讀出目前設定再修改，而不是從零建立。第一次成功讀到設定時（`have_orig == 0`）把它存進 `orig_tio`，讓 reader 結束時能把連接埠「還原成找到時的樣子」。`cfmakeraw()` 關掉行編輯、回顯與輸入輸出轉換，確保裝置的位元組原樣進出 ring；其後明確設定：`CLOCAL | CREAD`（忽略 modem 控制線、啟用接收），清掉 `CSTOPB | PARENB | CRTSCTS`（1 停止位元、無同位、無硬體流量控制，即 8N1），`VMIN = 1`、`VTIME = 0`（至少 1 byte 才算可讀、不用字元間逾時）。fd 是以 `O_NONBLOCK` 開啟的（見 `dev_open()`），所以 `read()` 在沒有資料時回 `EAGAIN` 而不阻塞。最後以 `tcsetattr(fd, TCSANOW, &t)` 套用，`TCSANOW` 表示立刻生效、不等輸出佇列排空，並回傳它的結果（成功 0、失敗 -1）。
- **細節**：
  - `speed_of(baud)` 回傳 0（不支援）時不會呼叫 `cfsetispeed/cfsetospeed`，鮑率維持 `tcgetattr` 讀到的原值，函式仍可能回 0。呼叫端因此依賴 `baud_supported()` 事先把關。
  - `have_orig` 與 `orig_tio` 是檔案層級靜態變數，裝置重新開啟（USB 重新列舉）後不會被覆蓋，所以保存的是「第一次」看到的設定。
  - 若 fd 不是 tty（例如測試時指向一般檔案），`tcgetattr` 會失敗而回 -1，由 `dev_open()` 記一則警告。
  - 失敗時 `errno` 保留給呼叫者的 `strerror(errno)` 使用。
- **關係**：被誰呼叫 → `dev_open()`（開裝置時用共享記憶體裡的鮑率）、`handle_pipe_msg()` 的 `PIPE_BAUD` 分支（執行中改鮑率）；呼叫 → `speed_of()`、libc 的 `tcgetattr` / `cfmakeraw` / `tcsetattr`。

### `dev_open`
*位置：`server/reader.c:81`　簽章：`static int dev_open(void)`*

- **用途**：開啟 `g_cfg.device` 並套用共享記憶體中目前的鮑率；成功回 fd，`open()` 失敗回 -1。
- **設計**：`open()` 旗標 `O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC`：`O_NOCTTY` 避免這個 tty 變成行程的控制終端；`O_NONBLOCK` 讓 `open()` 不會因為等待載波而卡住，也讓之後的 `read()`/`write()` 不會阻塞整個 reader（它還要服務輸入 pipe 與計時器）；`O_CLOEXEC` 防止 fd 洩漏到之後 exec 的程式。鮑率不是用 `g_cfg.baud`，而是在鎖內讀 `g_shm->baud`，因為 controller 可能在執行期間改過鮑率；裝置重新插上時應該沿用「最後一次生效的鮑率」，而不是啟動參數。鎖只包住讀取那一行，不在鎖內做 `open()` 或 `tcsetattr()`，符合 docs/DECISIONS.md 的「不在鎖內做 I/O」原則。
- **細節**：
  - `open()` 失敗直接回 -1，此時 `errno` 還是 `open()` 的原因，`reader_main()` 立刻用 `strerror(errno)` 印出（中間沒有其他系統呼叫覆蓋它）。
  - `apply_termios()` 失敗只記 `LOGW("tcsetattr %s: %s (not a tty?)")`，仍回傳 fd。也就是說非 tty 的裝置（例如拿一般檔案或某些虛擬裝置測試）仍可使用。
  - 這個函式不改 `dev_state`、不寫事件，狀態轉換由呼叫端（`reader_main` 的初次開啟或 `dev_back()`）負責。
- **關係**：被誰呼叫 → `reader_main()`（啟動時，以及每 500 ms 的重開嘗試）；呼叫 → `shm_lock`/`shm_unlock`（`server/robust.c`）、`apply_termios()`。

### `dev_lost`
*位置：`server/reader.c:94`　簽章：`static void dev_lost(const char *why)`*

- **用途**：處理「裝置消失」：關閉 `tty_fd`、把 `dev_state` 設為 `DEV_LOST`，並在 ring 中寫一筆 `EV_DEVICE_LOST` 事件讓所有 client 看到。
- **設計**：先 `close(tty_fd)` 並設 `tty_fd = -1`（之後 `poll` 的 nfds 變成 1，不再監看 tty），再進鎖更新共享狀態。狀態變更與 `ring_event()` 放在同一個臨界區，讓觀察者不會看到「ring 裡已有 DEVICE_LOST 事件、`dev_state` 卻仍是 `DEV_UP`」的中間狀態。`LOGW` 在 `shm_unlock` 之後才印（docs/DECISIONS.md：鎖內不做 I/O）。
- **細節**：
  - `why` 只用於日誌（`"EOF"` 或 `strerror(errno)`），事件文字固定為 `"device lost"`。
  - 此函式不重設 `ctrl_pid`：失去裝置時控制權不會被取消。之後 child 的 `BN_INPUT` 會因 `dev_state != DEV_UP` 回 `BN_ERR_DEVICE_LOST`（`server/session.c:349-356`），已經在 pipe 裡的輸入則由 `handle_pipe_msg()` 在 `tty_fd < 0` 時丟棄。
  - 日誌訊息說「retrying every 500 ms」；重試時間由呼叫端設 `next_reopen`。
  - 在 `--unsafe=dev-reopen` 下，`reader_main()` 在呼叫本函式之前就 `continue`，所以這個函式不會被執行。
- **關係**：被誰呼叫 → `reader_main()` 的 tty 讀取分支（`read()` 回 0 或非 EAGAIN/EINTR 錯誤時）；呼叫 → `shm_lock`/`shm_unlock`、`ring_event`（`server/ring.c`）。

### `dev_back`
*位置：`server/reader.c:105`　簽章：`static void dev_back(int fd)`*

- **用途**：裝置重新開啟成功後，採用新 fd、把 `dev_state` 設回 `DEV_UP`、累加重連次數並寫入 `EV_DEVICE_BACK` 事件。
- **設計**：與 `dev_lost()` 對稱，在同一個臨界區內更新 `dev_state`、`dev_reconnects` 與 ring 事件，其他行程讀到的狀態與事件順序一致。`dev_reconnects` 提供給 INFO 統計使用。`tty_fd = fd` 在鎖外先設，因為 `tty_fd` 是 reader 私有變數，只有這個行程使用，不需要鎖保護。
- **細節**：
  - 只用於「重新連上」；程式啟動時第一次 `open()` 成功是在 `reader_main()` 直接設 `DEV_UP`，不經過本函式，因此不會增加 `dev_reconnects`，也不產生 `EV_DEVICE_BACK`。
  - 事件文字固定為 `"device reconnected"`。
- **關係**：被誰呼叫 → `reader_main()` 的定時重開區塊（`server/reader.c:272-277`）；呼叫 → `shm_lock`/`shm_unlock`、`ring_event`。

### `tty_write_all`
*位置：`server/reader.c:117`　簽章：`static ssize_t tty_write_all(const uint8_t *p, size_t n)`*

- **用途**：把 `n` 個位元組寫進非阻塞的 tty，輸出佇列滿時短暫等待；回傳實際寫出的位元組數，一個都沒寫成時回 -1。
- **設計**：tty 是 `O_NONBLOCK`，`write()` 可能只寫一部分（short write）或回 `EAGAIN`（核心的 UART 輸出佇列已滿，在低鮑率下很常見），所以必須用迴圈補寫。遇到 `EAGAIN`/`EWOULDBLOCK` 時用 `poll(POLLOUT, 50 ms)` 等待可寫，而不是忙等；`EINTR` 直接重試。等待次數上限 `spins++ < 20`（每次最多 50 ms，所以單次呼叫最多卡約 1 秒），避免卡死的裝置讓 reader 永遠停在這裡，無法讀 tty 或服務其他訊息。
- **細節**：
  - `spins` 是整個呼叫累計，成功寫出資料後不會歸零。
  - `poll` 的回傳值與 `revents` 都不檢查，等待結束後只是回到迴圈再試一次 `write()`。
  - 其他錯誤（如 `EIO`）或等待次數用完時，回傳 `done ? done : -1`，即已寫出的部分仍算數。
  - 它不負責偵測裝置消失；裝置消失由讀取端的 `read()` 偵測。
  - 迴圈內全程不持有共享鎖，統計（`tx_bytes`）由呼叫端在寫完後另外加。
- **關係**：被誰呼叫 → `handle_pipe_msg()` 的 `PIPE_INPUT` 分支；呼叫 → libc 的 `write`、`poll`。

### `handle_pipe_msg`
*位置：`server/reader.c:139`　簽章：`static void handle_pipe_msg(struct pipe_msg *m)`*

- **用途**：處理一則由 child 經輸入 pipe 送來的訊息，依 `m->type` 分為寫入 tty（`PIPE_INPUT`）、改鮑率（`PIPE_BAUD`）、釋放控制權（`PIPE_RELEASE`）。
- **設計**：開頭先在鎖內取得「目前控制者的 pid 與名稱」快照：`is_ctrl = (m->pid == g_shm->ctrl_pid)`。這是第二次檢查——child 在轉送前已檢查過自己是 controller（`server/session.c:348-357`），但在訊息躺在 pipe 裡的這段時間，控制權可能已被釋放、閒置逾時或換人，所以 reader 要在執行當下再確認一次，避免過期的輸入打到裝置上。`BAUD` 與 `RELEASE` 也走同一條 pipe，是為了和同一個 child 先前送出的 `INPUT` 保持嚴格順序：`bench send "x"` 的 TAKE、INPUT、RELEASE 不會因為 RELEASE 搶先處理而遺失輸入（docs/DECISIONS.md「Server」第二條）。`who`（控制者名稱）只用在日誌與事件文字，在鎖內以 `bench_strlcpy` 複製出來，這樣離開鎖之後使用它是安全的。
- **細節**：
  - `PIPE_INPUT`：`!is_ctrl` 時丟棄並記 `LOGD`（stale）；`tty_fd < 0`（裝置不在）時也丟棄；否則呼叫 `tty_write_all(m->data, m->len)`，若回傳 `w > 0`，在鎖內 `tx_bytes += w`（寫出幾個才算幾個）。`LOGT` 在鎖外印出實際寫入數（可能是 -1）。
  - `PIPE_BAUD`：需 `is_ctrl` 且 `m->len == sizeof(uint32_t)`，否則靜默忽略。新鮑率以 `memcpy` 讀出（避免對 `data[]` 做未對齊存取），是主機位元組序，因為 child 已經 `be32toh()` 轉過再用 `pipe_send()` 轉送（`server/session.c:392-403`）。若 `tty_fd >= 0`，先 `apply_termios(tty_fd, nb)`，失敗就記 `LOGW` 並直接返回，共享狀態不變。若裝置不在（`tty_fd < 0`），仍會把新值寫進 `g_shm->baud` 並發出 `EV_BAUD_CHANGED` 事件，等裝置回來時 `dev_open()` 就會套用它。事件文字格式為 `"baud %u -> %u by %s"`，舊值在鎖內讀取。這裡不再呼叫 `baud_supported()`，信任 child 已檢查。
  - `PIPE_RELEASE`：預設理由 `"released"`；若 `m->len` 在 1 到 31 之間，改用 `m->data` 內容（pipe 內的字串沒有結尾 NUL，所以手動 `reason[m->len] = 0`）。目前 child 送出的理由是 `"released"`（明確 RELEASE，`server/session.c:377`）與 `"disconnect"`（離線，`server/session.c:73`）。在鎖內以 `m->pid == g_shm->ctrl_pid` 再判斷一次（和開頭快照之間鎖已放開過），符合才呼叫 `ctrl_release_locked()`，收集到 `struct ctrl_note`，解鎖後用 `ctrl_log_note()` 印日誌。R6（`--unsafe=ctrl-release`）的關閉發生在寄件端：child 的 `finish()` 在該開關下不送離線的 RELEASE（`server/session.c:72-73`），本函式本身沒有判斷 R6。
  - 其他 `type`：`LOGW("unknown pipe message type %u")`。
  - 所有分支都在鎖外做寫 tty 與日誌 I/O。
- **關係**：被誰呼叫 → `reader_main()` 的 pipe 讀取分支；呼叫 → `tty_write_all()`、`apply_termios()`、`ring_event`、`ctrl_release_locked`、`ctrl_log_note`（`server/control.c`）、`bench_strlcpy`。

### `reader_main`
*位置：`server/reader.c:203`　簽章：`void reader_main(void)`*

- **用途**：reader 行程的整個生命週期：初始化、開裝置、主迴圈（tty → ring、pipe → tty、重開裝置、閒置釋放）、結束時還原 termios；不會返回（`noreturn`，以 `_exit(0)` 結束）。
- **設計**：
  - **行程設定**：`bench_log_set_role("reader")` 與 `prctl(PR_SET_NAME, "benchd-reader")` 讓日誌與 `ps` 易讀；`prctl(PR_SET_PDEATHSIG, SIGTERM)` 讓 master 死亡時 reader 一起結束（docs/DECISIONS.md）；緊接著 `getppid() != g_shm->master_pid` 時 `_exit(0)`，處理「fork 後、prctl 生效前 master 已死」的競態（此時 PDEATHSIG 不會再觸發）。`SIGINT` 與 `SIGUSR1` 設為 `SIG_IGN`：終端機 Ctrl-C 由 master 統籌關機；SIGUSR1 是 master 切換日誌等級用的，reader 從 fork 繼承到 master 的 handler 不應被執行。SIGTERM 裝上 `on_term`。
  - **關閉不需要的 fd**：`close(g_cfg.listen_fd)`（reader 不接受連線，也不應持有監聽 socket）、`close(g_cfg.input_pipe[1])`（reader 只讀 pipe，不當寫端），保留讀端 `pipe_rd`。
  - **主迴圈**：每輪用 `poll()` 同時等 pipe 與 tty，逾時 100 ms，這個 tick 讓迴圈即使沒有 I/O 也能定期檢查 `reader_stop`、重開時間與閒置計時。`struct pollfd p[2]` 中 tty 放第二個，且只在 `tty_fd >= 0` 時才把 nfds 設為 2，避免對 -1 的 fd 做 poll。tty 與 pipe 都在同一個 `poll` 裡處理，是因為 reader 單執行緒、單行程，不能為其中一個而阻塞。
  - **tty 讀取**：每次最多讀 `REC_MAX_DATA`（1024）bytes，成功後在同一個臨界區內 `ring_append(REC_DATA, 0, ...)`、`rx_bytes += r`、`last_rx_ns = now_ns()`，所以一次 `read()` 對應一筆 DATA 記錄，長度不會超過 ring 規定的上限。`LOGT` 在鎖外。
  - **pipe 讀取**：先 `readn()` 讀 `PIPE_MSG_HDR`（8 bytes：`pid`、`type`、`len`），確認 `len <= BN_MAX_INPUT`（256）後，若 `len > 0` 再 `readn()` 讀 `len` bytes 的資料（`len == 0` 時不再讀），之後才交給 `handle_pipe_msg()`。child 以單一 `writen()` 寫 `PIPE_MSG_HDR + len`（至多 264 bytes，小於 `PIPE_BUF`），寫入是原子的，不同 child 的訊息不會交錯；`readn()` 則處理一次 `read()` 沒讀滿的情況。每輪迴圈最多處理一則 pipe 訊息，pipe 內還有資料時下一輪 `poll` 會立即返回。
  - **定時工作**：用 `mono_ms()`（單調時鐘）安排 `next_reopen`（裝置不在時，每 500 ms 重試）與 `next_idle`（每 1000 ms 呼叫 `ctrl_check_idle_locked()`），單調時鐘不受系統改時間影響；閒置判斷內部用的是 `now_ns()` 的牆上時間，與 `ctrl_last_input_ns` 同一個時間基準。與 `ctrl_log_note()` 搭配：鎖內只填 note，解鎖後才印日誌。
  - **離開**：`reader_stop` 被設定後跳出迴圈；若 `tty_fd >= 0 && have_orig` 就用 `tcsetattr(TCSANOW)` 還原 `orig_tio`，讓連接埠回到被 benchd 修改之前的狀態，再 `_exit(0)`。以 `_exit(0)` 結束，不經過 `exit()`，因此不會執行 atexit 處理函式，也不會 flush stdio。
- **細節**：
  - **初次開啟**：`tty_fd = dev_open()`。失敗時記 `LOGW("cannot open ...")`、把 `dev_state` 設為 `DEV_LOST`（master 初始化時已是 `DEV_LOST`，這裡再設一次，不寫事件、`next_reopen` 為 0，所以第一個迴圈 tick 之後就會嘗試重開）；成功時設 `DEV_UP`，不寫事件、不增加 `dev_reconnects`。
  - **poll 錯誤**：`n < 0 && errno != EINTR` 記 `LOGE("poll: ...")` 並 `break`；`EINTR`（例如收到 SIGTERM）則落到後面的定時區塊，下一輪 `while` 檢查 `reader_stop`。
  - **tty 事件判斷**：檢查 `revents & (POLLIN | POLLHUP | POLLERR)`，因為裝置被拔掉時常以 `POLLHUP`/`POLLERR` 而不是 `POLLIN` 通知。`read()` 回 `r == 0` 或 `errno` 不是 `EAGAIN`/`EINTR`（例如 USB 轉接器被拔掉的 `EIO`）視為裝置遺失：呼叫 `dev_lost(r == 0 ? "EOF" : strerror(errno))`，並設 `next_reopen = mono_ms() + 500`。`EAGAIN`/`EINTR` 則什麼都不做，等下一輪。
  - **pipe 讀取的異常**：`readn` 回傳 `<= 0` 時只記 `LOGE("input pipe closed")`，不離開迴圈；回傳正數但小於 8，或 `len` 超過 `BN_MAX_INPUT`，或資料不完整時，訊息被靜默丟棄（不會呼叫 `handle_pipe_msg`）。child 端會把輸入長度限制在 `BN_MAX_INPUT` 以內，正常情況不會發生。
  - **定時重開**：條件是 `tty_fd < 0 && !UNSAFE(R_DEV_REOPEN) && now >= next_reopen`；`dev_open()` 成功就 `dev_back(fd)`，不論成敗都把 `next_reopen` 設為 `now + 500`。
  - **`--unsafe=dev-reopen`（R8）**：有兩處改變。(1) tty 讀取遇到 EOF/致命錯誤時直接 `continue`（不關 fd、不寫 DEVICE_LOST、不排程重開）。拔除後的 fd 會持續回報 `POLLHUP`/`POLLERR`，`poll()` 每次立刻返回，造成 100% CPU 的空轉，而且因為 `continue` 跳過了迴圈後半，pipe 輸入和閒置釋放也不再被處理；`SIGTERM` 仍能結束它，因為 `continue` 回到 `while (!reader_stop)`。(2) 定時重開區塊整個被略過，所以即使啟動時裝置不在，也永遠不會再嘗試開啟。`scripts/demo_r8.sh` 展示的就是這個差異（README：unsafe 為「reader at 100 % CPU, never recovers」，safe 為 DEVICE LOST → reconnected）。
  - **`--unsafe=ctrl-release`（R6）** 間接影響：`ctrl_check_idle_locked()` 在該開關下直接返回（`server/control.c`），所以每秒的閒置檢查變成空操作。
  - **`--unsafe=robust-mutex`（R7）**：reader 的每次 `shm_lock()` 都可能卡在已死持有者留下的鎖上，這就是 `on_term` 內 `alarm(2)` 存在的原因。
- **關係**：被誰呼叫 → master 的 `spawn_reader()`（`server/main.c:64-72`，fork 之後的子行程分支）；呼叫 → `dev_open`、`dev_lost`、`dev_back`、`handle_pipe_msg`、`ring_append`（`server/ring.c`）、`ctrl_check_idle_locked` / `ctrl_log_note`（`server/control.c`）、`readn`（`lib/net.c`）、`mono_ms` / `now_ns`（`lib/util.c`）、`shm_lock` / `shm_unlock`（`server/robust.c`）。

---

## `server/session.c`

這個檔案是「每個 TCP client 一個 child 行程」的全部邏輯，由 master 在 `handle_accept()`（`server/main.c:188`）`fork()` 之後在 **per-client child** 內執行，入口是 `session_main()`，且永不返回。child 的生命週期分三段：`handshake()`（HELLO 驗證、登記 slot、送 WELCOME、歷史重播或斷線續傳）、`session_main()` 內的 live loop（每 20 ms 處理封包並把 ring 新增內容送出去）、`finish()`（清理 slot、必要時交還控制權、`_exit(0)`）。檔案對外只提供 `session_main()`（宣告於 `server/server.h:137`），其餘函式都是 `static`。它依賴共用記憶體與 robust mutex（`server/robust.c` 的 `shm_lock`/`shm_unlock`/`slots_recount`）、ring（`server/ring.c` 的 `ring_seek`/`ring_copy`）、控制權仲裁（`server/control.c`）、`server/sysinfo.c` 的 `sysinfo_build`，以及 `libbench.so` 的 `bn_send`/`bn_recv`/`bn_send_error`、`writen`、`set_io_timeouts`、`set_keepalive` 與 `--unsafe` 開關表（`UNSAFE(R_*)`）。child 不碰 serial fd：要送往裝置的輸入、改 baud、釋放控制權都經由 input pipe 交給 reader 行程執行，因此這裡只做「檢查、轉送、回應」。

本檔使用的靜態狀態：`copybuf[64 KiB]`（`aligned(8)`，從 ring 拷出一批 record 的私有副本，`session.c:39`）、`sendbuf`（組 DATA/EVENT payload 用，大小為 `sizeof(struct bn_event_hdr) + REC_MAX_DATA + 8`，`session.c:40`）、`pipe_wr`（input pipe 的寫入端，`session.c:41`）。每個 child 是獨立行程，這些 static 變數互不干擾，也沒有執行緒需要保護。`struct sess`（`session.c:27`）記錄 fd、slot 編號、自己的 pid、名稱、peer 字串、下一個要送出的 ring 偏移 `off`、最後收到封包的時間 `last_rx_ms`、最後送 STATUS 的時間與內容。

### `violation`
*位置：`server/session.c:45`　簽章：`static void violation(struct sess *s, int code)`*

- **用途**：遇到協定違規（壞 magic／版本／長度／CRC、錯誤型別等）時，記一筆警告、盡力回一個 ERROR 封包，然後結束這個 session。
- **設計**：違規代表對端不是合法的 benchd client，或雙方已對不上封包邊界，繼續讀下去只會越讀越亂，所以直接斷線而不是嘗試重新同步。ERROR 封包的 `bn_send_error()` 回傳值被刻意忽略，因為對端很可能已經斷了，沒送成也不影響結果。因為要呼叫在它後面才定義的 `finish()`，檔案開頭（`session.c:43`）先宣告 `finish` 為 `__attribute__((noreturn))`，讓編譯器知道 `violation` 之後不會繼續執行。
- **細節**：
  - 錯誤碼名稱透過 `bn_err_name(code)` 取得，ERROR 文字為 `"protocol error: <NAME>"`；`code` 可能是 `bn_recv()` 在 `*perr` 回報的 `BN_ERR_BAD_MAGIC/BAD_VERSION/BAD_LENGTH/BAD_CRC`，也可能是本檔自行判斷的 `BN_ERR_BAD_LENGTH`、`BN_ERR_BAD_TYPE`。
  - 結束原因字串固定為 `"protocol violation"`。
  - `--unsafe=validate`（R3）開啟時，多數呼叫點會被繞過、不再呼叫本函式，見 `handshake()` 與 `handle_packet()` 的說明。
- **關係**：被誰呼叫 → `handshake()`、`handle_packet()`（本檔）；呼叫 → `bn_send_error()`（`lib/proto.c`）、`finish()`。

### `pipe_send`
*位置：`server/session.c:54`　簽章：`static void pipe_send(struct sess *s, uint16_t type, const void *data, uint16_t len)`*

- **用途**：把一個訊息（`PIPE_INPUT`、`PIPE_BAUD`、`PIPE_RELEASE`）經 input pipe 轉交給 reader 行程，由 reader 去寫 serial 或改控制權。
- **設計**：整個 server 只有 reader 持有 serial fd（見 README 架構），child 不能自己寫裝置。訊息格式是 `struct pipe_msg`（`server/server.h:84`）：`pid`（4 bytes）+ `type`（2）+ `len`（2）共 `PIPE_MSG_HDR = 8` 位元組標頭，加最多 `BN_MAX_INPUT = 256` 位元組資料，整則訊息最大 264 位元組，遠小於 `PIPE_BUF`。POSIX 保證不大於 `PIPE_BUF` 的 pipe `write()` 是原子的，所以許多 child 同時寫同一條 pipe 時，訊息不會互相交錯，不需要任何鎖。訊息帶 `s->me`（自己的 pid），reader 收到後會再用它跟 `ctrl_pid` 比對（`server/reader.c` `handle_pipe_msg`），即使 child 剛才檢查時還是 controller、轉送後控制權已易手，過期的輸入也會被丟掉。
- **細節**：
  - 只寫 `PIPE_MSG_HDR + len` 位元組，而不是整個 `struct pipe_msg`，所以短訊息不會浪費 pipe 容量。
  - `len` 為 0 時不 `memcpy`（`data` 可為任意值）。呼叫端必須保證 `len <= BN_MAX_INPUT`：`handle_packet()` 的 INPUT 分支在 `h.len > BN_MAX_INPUT` 時違規斷線，或在 `--unsafe=validate` 下截成 256。
  - 失敗（例如 reader 已死、`EPIPE`）只記 `LOGE("input pipe write: ...")`，沒有重試或通知 client。master 內 `SIGPIPE` 在 safe 模式被設為忽略並由 child 繼承，因此寫入死掉的 pipe 回傳 `-1` 而不是殺掉 child（`server/main.c:338-339`）。
  - `writen()` 會重試 `EINTR` 與部分寫入。
- **關係**：被誰呼叫 → `finish()`（`PIPE_RELEASE "disconnect"`）、`handle_packet()`（INPUT / RELEASE / BAUD 分支）；呼叫 → `writen()`（`lib/net.c`）；對端 → `server/reader.c` `handle_pipe_msg()`。

### `finish`
*位置：`server/session.c:63`　簽章：`static void finish(struct sess *s, const char *why)`*

- **用途**：這個 child 唯一的正常結束出口。清掉自己的 slot、更新 viewers 計數、若持有控制權就請 reader 釋放，記 log，關 socket，`_exit(0)`。
- **設計**：
  - **同一個臨界區做完所有共用狀態的修改**：先記下 `was_ctrl`（自己是不是 `ctrl_pid`），清 slot，`slots_recount()` 重算 `viewers`，再解鎖。log、pipe 寫入、`close()` 都在鎖外做，符合 DECISIONS 的「鎖內不做 I/O」。
  - **釋放控制權不直接改 shm，而是送 `PIPE_RELEASE` 給 reader**（DECISIONS「An explicit RELEASE, and the release on a normal disconnect, both travel the input pipe」）：同一個 child 之前已轉送的 INPUT 還在 pipe 裡，RELEASE 排在它們後面，由 reader 依序處理，所以 `bench send "x"`（TAKE、INPUT、RELEASE）不會因為先釋放而丟掉輸入。
  - 用 `_exit(0)` 而不是 `exit(0)`：不跑 atexit 處理器與 stdio 緩衝清理，child 只需要關掉 socket 就結束。
  - slot 只在 `g_shm->slots[s->slot].pid == s->me` 時才清除：slot 在 master 的 `handle_accept()` 先被標為 `pid = -1`，fork 之後才寫入 child pid；child 很早就結束（例如 HELLO 逾時）時 slot 可能仍是 `-1`，這時 child 不清它；master 在 fork 後會把仍為 `-1` 的 slot 填上 child pid，之後由 SIGCHLD 收屍路徑（`reap_children()`，`server/main.c:97-99`，找出 `pid` 等於死掉的 child 的 slot 並清零）清除。`s->slot < 0`（slot 表滿時沒分到 slot）則整段略過。
- **細節**：
  - `--unsafe=ctrl-release`（R6）時不送 `PIPE_RELEASE`，連同 master SIGCHLD 路徑、`ctrl_take_locked()` 的 `kill(pid,0)` 檢查與閒置釋放一起被關掉，所以離線的 controller 會永遠佔著控制權（`demo_r6.sh`）。
  - `why` 只用於 `LOGI("client %s (%s) left: %s")`。
  - 宣告為 `noreturn`；呼叫它之後的程式碼不會執行，因此 `handle_packet()` 的 `BN_BYE` 分支不需要 `break`/`return`。
  - 被 `SIGTERM`（預設處理，master 關機時送出）或其他訊號殺掉的 child 不會走到這裡。master 平常的 SIGCHLD 路徑（`reap_children()`）會替崩潰的 child 清 slot 與釋放控制權；但關機時 master 已進入 `shutdown_server()`，不再呼叫 `reap_children()`，共享記憶體隨後被 `munmap`，不需要善後。
- **關係**：被誰呼叫 → `violation()`、`handshake()`、`handle_packet()`、`session_main()`；呼叫 → `shm_lock`/`shm_unlock`/`slots_recount`（`server/robust.c`）、`pipe_send()`。

### `send_record`
*位置：`server/session.c:81`　簽章：`static int send_record(struct sess *s, const struct ring_rec *r, const uint8_t *data, uint64_t end_off)`*

- **用途**：把一筆從 ring 複製出來的 record 轉成對應的線上封包：`REC_EVENT` 送成 `BN_EVENT`，其餘送成 `BN_DATA`。
- **設計**：封包的 `offset` 欄位是 **該 record 結束處的邏輯偏移**（下一筆 record 的起點，DECISIONS「DATA/EVENT `offset` is the logical ring offset at the end of that record」），所以 client 只要記住「最後看到的 offset」，斷線重連時就能把它放進 HELLO 的 `resume_offset` 續傳。呼叫端（`pump()`）一邊走過 record 一邊累加 `REC_SIZE`，把算好的 `end_off` 傳進來。payload 先在 static `sendbuf` 組好（標頭 + 資料），再交給 `bn_send()`，後者另外把標頭與 payload 合成單一 `write()`。
- **細節**：
  - 所有多位元組欄位以 `htobe64`/`htobe16` 轉成網路序；`ts_ns` 來自 record 寫入 ring 的時間（`CLOCK_REALTIME`）。`bn_event_hdr.pad` 設為 0，`bn_data_hdr` 沒有 pad。
  - `sendbuf` 的大小為 `sizeof(struct bn_event_hdr) + REC_MAX_DATA + 8`，而 `r->len` 已被 `ring_copy()` 限制在 `REC_MAX_DATA = 1024` 以內（`ring_copy()`，`server/ring.c:109`；safe 模式下 `len > REC_MAX_DATA` 的 record 會被視為鏈結損壞並跳到 `end`，不會送出），因此 `memcpy` 不會溢位。
  - `--unsafe=overrun`（R10）時 record 可能是被覆寫後的垃圾；型別不是 `REC_EVENT` 的一律當 DATA 送（註解寫在 `session.c:92`），畫面上會出現亂碼，這正是示範的效果。
  - 回傳 `bn_send()` 的結果：`0` 成功，`-1` 失敗（`errno` 保留，例如 `EAGAIN` 表示 `SO_SNDTIMEO` 逾時、`EPIPE`/`ECONNRESET` 表示對端已斷）。
- **關係**：被誰呼叫 → `pump()`；呼叫 → `bn_send()`（`lib/proto.c`）。

### `send_dropped`
*位置：`server/session.c:100`　簽章：`static int send_dropped(struct sess *s, uint64_t dropped, uint64_t new_off)`*

- **用途**：告訴這個 client「有 `dropped` 位元組的歷史已被覆蓋，串流從 `new_off` 繼續」，以 `BN_EVENT`（code `EV_DROPPED`）送出。
- **設計**：落後多少是每個 client 各自的事（取決於它自己的讀取偏移），所以這個事件 **不寫進共用 ring**，而是由該 child 在送出時當場合成；封包的 `ts_ns` 因此用 `now_ns()`，而不是某筆 record 的時間。`offset` 欄位放 `new_off`（跳到的新位置），與其他事件「offset = 之後的串流位置」的語意一致。文字為 `"dropped <大小>"`，大小以 `fmt_size()` 格式化（例如 `12.3K`），客戶端顯示給使用者看。
- **細節**：
  - `snprintf` 的緩衝是 96 位元組，大小字串 32 位元組，實際遠不會截斷；`n` 直接用 `snprintf` 回傳值當 `memcpy` 長度。
  - 同時記 `LOGW("client ... fell behind: %llu bytes overwritten")`。
  - 只有兩處會呼叫：`pump()` 發現自己被 ring 追過（R10 的安全路徑），以及 `handshake()` 斷線續傳但 `resume < tail` 時。非續傳的完整重播改用 `REPLAY_BEGIN.dropped_bytes` 回報，不經過這個函式。
  - 回傳 `bn_send()` 的結果（0 或 -1）。
- **關係**：被誰呼叫 → `pump()`、`handshake()`；呼叫 → `fmt_size()`、`now_ns()`（`lib/util.c`）、`bn_send()`。

### `fill_status`
*位置：`server/session.c:114`　簽章：`static void fill_status(struct sess *s, struct bn_status *st)`*

- **用途**：把目前的裝置與控制權狀態組成一個 `struct bn_status`，供 `pump()` 在一秒一次的 STATUS 封包使用。
- **設計**：**呼叫端必須已持有 `shm_lock`**，因為它直接讀 `g_shm` 的多個欄位，需要一致快照；因此 `pump()` 是在同一個臨界區內呼叫它。它是 per-child 函式，因為 `you_are_ctrl` 對每個 client 不同（`g_shm->ctrl_pid == s->me`）。先 `memset` 成 0，保證 `pad` 與 `ctrl_name` 尾端都是 0，這樣 `pump()` 之後可以安全地用 `memcmp` 比較兩個 STATUS 是否相同。
- **細節**：
  - 欄位：`dev_state`、`you_are_ctrl`、`baud`、`viewers`、`ctrl_idle_sec`（`ctrl_idle_sec(g_shm, now)`，沒有 controller 時為 0）、`rx_bytes`；多位元組欄位轉成大端。`ctrl_name` 僅在 `ctrl_pid != 0` 時才複製。
  - `viewers` 取自 `g_shm->viewers`，即 `slots_recount()` 算出的 `pid` 非 0 且 `joined` 的 slot 數，自己也算在內。
  - 不做任何 I/O。
- **關係**：被誰呼叫 → `pump()`；呼叫 → `ctrl_idle_sec()`（`server/control.c`）、`bench_strlcpy()`、`now_ns()`。

### `pump`
*位置：`server/session.c:132`　簽章：`static int pump(struct sess *s, uint64_t limit, int want_status)`*

- **用途**：把 ring 中 `[s->off, min(head, limit))` 範圍的 record 依序送給 client；可選擇順便送一個節流過的 STATUS。回傳 `0` 表示這一輪送完（追上了 head 或 limit），`-1` 表示送出失敗（`errno` 保留）。
- **設計**：
  - **鎖內只做 `memcpy`，網路寫入在鎖外**（`session.c:128-131` 的註解）。每輪迴圈先 `shm_lock`，在鎖內調整 `s->off`、用 `ring_copy()` 把最多 `sizeof copybuf = 64 KiB` 的整筆 record 複製進私有的 `copybuf`、更新 slot 的偏移、必要時 `fill_status()`，然後立刻 `shm_unlock`，之後才 `bn_send()`。這樣慢 client 的 `write()` 阻塞只卡住自己的 child，不會拖住 reader 和其他 child 搶鎖（README「fork() isolates slow clients」）；而且送出的是私有副本，reader 之後覆寫 ring 也不會把送到一半的資料弄壞。
  - **為什麼要 `for(;;)`**：一次最多只拷 64 KiB，ring 裡積壓更多時要分批；每批都重新取鎖，等於讓出鎖給別人。
  - **R10 落後處理**：若 `s->off < g_shm->tail`，代表這個 client 讀得太慢，舊資料已被 `ring_append` 逐筆淘汰；此時把 `s->off` 跳到 `tail`，記下 `dropped = tail - 原 off`，釋放鎖後先 `send_dropped(s, dropped, start)`，再繼續送資料，client 看到乾淨的資料加一個 DROPPED 事件。`--unsafe=overrun` 時略過這個檢查，`ring_copy()` 會從已被覆寫的位置讀垃圾「標頭」，只把長度夾到 1024 繼續，畫面上出現被覆寫的 record 標頭與舊位元組。
  - `s->off > g_shm->head` 時夾回 `head`，是防禦性檢查，避免偏移超過寫入位置。
  - `end = min(head, limit)`：live loop 傳 `UINT64_MAX`（送到目前最新）；`handshake()` 重播時傳 `snap_head`（取快照當下的 head），所以重播不會一路追到之後才寫入的 record，之後的資料交給 live loop，REPLAY_END 才有明確邊界。
  - **offset 的逐筆累加**：`o = start` 起算，每筆 record 先 `o += REC_SIZE(r->len)` 再呼叫 `send_record(..., o)`，因此封包帶的是 record 結尾偏移。因為 `copybuf` 為 8 位元組對齊，且每筆 record 長度對齊 8（`REC_SIZE`），強制轉型成 `struct ring_rec *` 讀取是對齊的。
  - **STATUS 節流**（僅 `want_status` 為真且只在第一輪）：`rx_bytes` 與 `ctrl_idle_sec` 隨時都在變，若直接 `memcmp` 整個結構，幾乎每次 pump 都不同、會狂送。因此先看距離上次送出是否 ≥ 1000 ms；否則把兩邊的 `rx_bytes`、`ctrl_idle_sec` 清成 0 再比，只有其他欄位（`dev_state`、`you_are_ctrl`、`baud`、`viewers`、`ctrl_name`）改變才立刻送。結果是：狀態變化立即通知，沒變化則每秒一次。送出後更新 `s->last_status` 與 `s->last_status_ms`。`last_status_ms` 初值為 0，所以 handshake 之後第一次 pump 通常立即送一個 STATUS。
  - **離開條件**：`n == 0 || s->off >= end` 就 `return 0`。`n == 0` 表示沒有東西可送（已追上，或 ring 內鏈結損壞時 `ring_copy` 直接把 `o` 推到 `end`）。
- **細節**：
  - slot 的 `offset` 欄位被設成這一批的 **起點** `start`（`session.c:147-148`），而不是送完後的位置，所以它落後一批；shutdown 時 master 等待所有 `joined` 的 slot 的 `offset >= target`（`server/main.c:228`），下一次 pump（最多約 20 ms 後）起點追上才會滿足。
  - 回傳 `-1` 的 `errno`：`EAGAIN`/`EWOULDBLOCK` 表示 `SO_SNDTIMEO`（60 s）到了，client 一直不讀；`session_main()` 依此給出不同的離場原因。
  - 全程沒有任何 log I/O 在鎖內。
- **關係**：被誰呼叫 → `handshake()`（重播，`limit = snap_head`、`want_status = 0`）、`session_main()`（live loop，`limit = UINT64_MAX`、`want_status = 1`）；呼叫 → `shm_lock`/`shm_unlock`、`ring_copy()`（`server/ring.c`）、`send_dropped()`、`send_record()`、`fill_status()`、`bn_send()`、`mono_ms()`。

### `handshake`
*位置：`server/session.c:189`　簽章：`static void handshake(struct sess *s, int conn_seq)`*

- **用途**：完成連線建立：在 5 秒內收到並驗證 HELLO，登記自己的 slot，回 WELCOME，然後依 HELLO 的內容做「歷史重播」或「斷線續傳」，並設定 `s->off` 為 live loop 的起點。任何失敗都以 `finish()` 結束，不會回到呼叫端。
- **設計**：
  1. **HELLO 期限（R5）**：先 `poll()` 等 socket 可讀，期限 `HELLO_TIMEOUT_MS = 5000`。用 `deadline - mono_ms()` 重算剩餘時間，`EINTR` 時繼續等，總時間才不會因為被訊號打斷而延長。逾時就回 `ERROR HANDSHAKE_TIMEOUT` 並離場。`poll` 只保證「有資料」，不保證整個 HELLO 都到了，所以 `session_main()` 還設了 `SO_RCVTIMEO = 5 s`：只送半個封包就停的 client，`bn_recv()` 會以 `EAGAIN` 回來，這裡回 `"HELLO incomplete"` 並離場。這兩層一起讓「連上來不說話」的 socket 最多佔用一個 child 5 秒（`demo_r5.sh`）。
  2. **接收與驗證**：用 `bn_recv()` 讀進 `char buf[4096]`。回傳 `0`（EOF）→ `"closed before HELLO"`；`-1` → 依 `errno` 離場；`-2` → `violation(s, perr)`。之後再檢查 `h.type == BN_HELLO` 且 `h.len >= sizeof(struct bn_hello)`，不符時以 `BAD_TYPE` 或 `BAD_LENGTH` 違規。
  3. **R3 示範點**：`buf` 是固定大小的堆疊陣列；`--unsafe=validate` 時 `bn_recv()` 不檢查 `h.len`，1 MiB 的長度會直接把 payload 讀進 4096 位元組的 `buf`，造成堆疊溢位（`stack smashing detected` 或 SIGSEGV，`demo_r3.sh`）。本函式的型別／長度檢查在此模式下也被略過，`memcpy(&hello, buf, sizeof hello)` 會讀到未初始化或不完整的內容。
  4. **名稱與參數**：把 `hello.name` 複製到 `s->name` 並 `sanitize_name()`（不可印出的位元組換成 `?`，去頭尾空白，空字串變 `"anon"`）；`replay_bytes`、`resume_offset`、`last_server_start_ns` 以 `be32toh`/`be64toh` 轉回主機序。
  5. **放寬逾時**：HELLO 收完後 `set_io_timeouts(fd, 60, 60)`，從 5 秒握手期限改成 60 秒閒置逾時（`IDLE_TIMEOUT_MS`），R5 關閉時略過。
  6. **一次取鎖做完所有需要一致性的事**：登記 slot（`pid`、`joined = 1`、`name`、`peer`、`joined_ns`），`slots_recount()` 更新 `viewers`（讓 WELCOME 的 viewer 數包含自己），組好 WELCOME 的各欄位，並決定重播起點。WELCOME 先填在區域變數 `w`，解鎖後才送，網路 I/O 不在鎖內；啟動 log 所需的 `viewers` 也在鎖內抄下來，解鎖後才印。
  7. **續傳 vs 完整重播**：
     - 條件 `hello.has_resume && client_start == g_shm->server_start_ns && resume <= g_shm->head` 同時成立才續傳。`server_start_ns` 比對是為了辨識 server 已重啟：重啟後 ring 偏移重新從 0 算，舊 `resume_offset` 沒有意義，所以退回完整重播（R12 示範的「server restarted」）；`resume > head` 也不可能合法。
     - 續傳：若 `resume < tail`（想接續的位置已被淘汰）→ `dropped = tail - resume`，從 `tail` 開始；否則 `from = ring_seek(resume)`，取 `resume` 之後的第一個 record 邊界。續傳 **不送** REPLAY_BEGIN/REPLAY_END，只有 `dropped > 0` 時送一個 DROPPED 事件，其餘由 live loop 接上。
     - 完整重播：`want = head - replay_bytes`（不夠就 0），小於 `tail` 時改為 `tail` 並記 `dropped`；`ring_seek(want)` 往後對齊到 record 邊界，避免從 record 中間開始送。
  8. **WELCOME 先於 REPLAY_BEGIN**，之後（非續傳時）送 `REPLAY_BEGIN {from_offset, dropped_bytes}`，`pump(s, snap_head, 0)` 把到快照 head 為止的歷史送完，最後送 `REPLAY_END {head = s->off}`。快照 head 之後新增的資料不屬於重播，由 live loop 送，這樣 client 能明確區分「歷史」與「即時」。
- **細節**：
  - 每一處送出失敗都以 `finish(s, "send failed")` 離場。
  - `poll()` 本身回傳負值且不是 `EINTR` 時，迴圈結束後 `r < 0`、不等於 0，會直接往 `bn_recv()` 走，由後者的結果決定下一步。
  - `s->slot < 0`（沒有分到 slot）時略過 slot 登記，但仍會 `slots_recount()` 與送 WELCOME。
  - **R7 的故障注入**（只有 `BENCH_FAULT_INJECTION` 編譯定義存在的 debug build）：當 `g_cfg.fault_die_in_lock_at == conn_seq` 時，WELCOME 送完後把 `RLIMIT_CORE` 設為 0（不產生 core）、記 `LOGW`、`shm_lock()` 後立刻 `abort()`，讓 child 死在持鎖狀態，示範 robust mutex 的 `EOWNERDEAD` 復原（`demo_r7.sh`，`BENCH_FAULT=die_in_lock[:N]`）。release build 沒有這段程式，只剩 `(void)conn_seq` 消除警告。
  - `--unsafe=timeout`（R5）時：不 `poll`、不設 socket 逾時，一個不送 HELLO 的連線會永遠佔著 child；加上 `--max-clients` 上限，32 個靜默連線就能讓其他人進不來。
  - `s->off` 在送 REPLAY_BEGIN 之前就被設成 `from`。
- **關係**：被誰呼叫 → `session_main()`；呼叫 → `bn_recv()`、`bn_send()`、`bn_send_error()`、`sanitize_name()`、`set_io_timeouts()`、`shm_lock`/`shm_unlock`、`slots_recount()`、`ring_seek()`、`send_dropped()`、`pump()`、`violation()`、`finish()`。

### `handle_packet`
*位置：`server/session.c:321`　簽章：`static int handle_packet(struct sess *s)`*

- **用途**：讀進並處理 **一個** client 封包（INPUT、TAKE、RELEASE、INFO_REQ、BAUD、PING、BYE）。回傳 `0` 表示繼續，`-1` 表示回應時寫 socket 失敗，呼叫端應結束 session；其他所有結束情形都在函式內直接 `finish()`。
- **設計**：
  - 只在 `poll()` 回報 socket 可讀時呼叫，所以 `bn_recv()` 一開始就有資料；`bn_recv()` 內部用 `readn()` 把封包湊滿（R4，TCP 可能把封包切成任意片段）。socket 上的 `SO_RCVTIMEO = 60 s` 保護的是「封包只到一半就停」的情況：`EAGAIN` → 回 `ERROR IDLE_TIMEOUT` 並離場。完全沉默的 client 靠 `session_main()` 的 `last_rx_ms` 檢查處理，因為那時 child 在 `poll()`，不在 `read()`。
  - 成功收到封包就更新 `s->last_rx_ms`，任何型別的封包（包含 PING）都算「還活著」。
  - `char buf[4096]` 同樣是 R3 的溢位目標（見 `handshake`）。
  - **權限檢查先在這裡做一次，讓 client 立刻得到錯誤回覆**：INPUT、BAUD 由 child 先查 `ctrl_pid == s->me`，不是 controller 就回 `ERROR NOT_CONTROLLER`（連線保持，不是違規）；通過後才經 pipe 轉給 reader，reader 會再用 pid 做第二次檢查。
  - 臨界區內只讀寫共用變數，回覆（`bn_send*`）和 log 一律在解鎖後。
  - 各分支：
    - **`BN_INPUT`**：`h.len > BN_MAX_INPUT (256)` 違規 `BAD_LENGTH`（R3 unsafe 則截成 256）。鎖內讀 `is_ctrl`、`dev_up`，並且 **只有 controller 且裝置在線** 才刷新 `ctrl_last_input_ns`，所以控制權的閒置計時只由真正送進裝置的輸入重置。非 controller → `NOT_CONTROLLER`；裝置離線 → `DEVICE_LOST`（"input dropped"）；否則 `pipe_send(PIPE_INPUT)` 並 `LOGT`。
    - **`BN_TAKE`**：鎖內呼叫 `ctrl_take_locked()`，它負責仲裁、檢查舊持有者是否已死（`kill(pid,0)`）、在 `ctrl_pid` 原本為 0 時把 CTRL_TAKEN 事件寫入 ring（舊持有者已死時另寫 CTRL_RELEASED）。不管成功與否都回 `BN_CTRL_RESULT`（含持有者名稱、閒置秒數、自動釋放剩餘秒數）；`LOGI` 記錄授予或拒絕。
    - **`BN_RELEASE`**：只有目前是 controller 才送 `PIPE_RELEASE "released"` 給 reader，由 reader 動手（與 INPUT 同一條 pipe，保持順序）；不是 controller 就靜默忽略，沒有回應封包。這個路徑不受 `--unsafe=ctrl-release` 影響（該開關只關閉自動釋放）。
    - **`BN_INFO_REQ`**：`sysinfo_build()` 內部自己取鎖，所以這裡不持鎖呼叫；輸出放在函式內的 `static char info[8192]`，避免佔 8 KiB 堆疊；結果以 `BN_INFO` 回覆。
    - **`BN_BAUD`**：payload 必須剛好 4 位元組，否則 `BAD_LENGTH` 違規（unsafe 時直接忽略、回 0）。轉成主機序後，鎖內檢查 controller 並刷新 `ctrl_last_input_ns`；非 controller → `NOT_CONTROLLER`；`baud_supported()` 不接受 → `ERROR BAD_REQUEST`（連線保持，這是 DECISIONS 新增的錯誤碼）；通過就把主機序 `uint32_t` 經 `PIPE_BAUD` 交給 reader 套用並寫入 BAUD_CHANGED 事件。
    - **`BN_PING`**：回 `BN_PONG`。若 payload ≥ 8 位元組，把 client 的時間戳原樣（不轉序）複製到 `client_ts_ns`，原樣回傳讓 client 量 RTT；`server_ts_ns` 為 `htobe64(now_ns())`。
    - **`BN_BYE`**：`finish(s, "bye")`，正常離場。
    - **其他型別（含 server→client 型別或重複的 HELLO）**：`violation(BAD_TYPE)`；`--unsafe=validate` 時只 `LOGD` 後忽略。
- **細節**：
  - `rc` 的處理：`0`（EOF，包含封包被切斷）→ `finish("connection closed")`；`-1` → 依 `errno` 離場；`-2` → `violation(s, perr)`。
  - 回傳值就是最後一次 `bn_send()`/`bn_send_error()` 的結果（`0` 或 `-1`）；INPUT、BAUD、RELEASE 成功轉送時回 `0`。
  - TAKE 的拒絕 log 用 `res.holder`，由 `ctrl_take_locked()` 在拒絕時填入。
- **關係**：被誰呼叫 → `session_main()`（live loop）；呼叫 → `bn_recv()`、`bn_send()`、`bn_send_error()`、`ctrl_take_locked()`（`server/control.c`）、`sysinfo_build()`（`server/sysinfo.c`）、`baud_supported()`（`server/reader.c`）、`pipe_send()`、`violation()`、`finish()`；轉送出去的訊息由 `server/reader.c` `handle_pipe_msg()` 處理。

### `session_main`
*位置：`server/session.c:423`　簽章：`void session_main(int fd, int slot, int conn_seq, const char *peer)`*

- **用途**：child 行程的入口與主迴圈，永不返回（宣告於 `server/server.h:137`）。負責行程初始化、呼叫 `handshake()`，然後進入 live loop，直到 `finish()` 把它結束。
- **設計**：
  - **初始化順序**：
    1. 建立 `struct sess`，`s.me = getpid()`，名稱先設為 `"?"`，`peer` 字串由 master 傳入。
    2. `bench_log_set_role("child:<pid>")` 讓 log 帶 child 標籤；`prctl(PR_SET_NAME, "benchd-child")` 讓 `ps`/`pstree` 好讀。
    3. `prctl(PR_SET_PDEATHSIG, SIGTERM)`：master 死了 child 也跟著死，避免孤兒繼續服務（DECISIONS）；`PDEATHSIG` 只在「設定之後」父行程死亡才觸發，所以緊接著用 `getppid() != g_shm->master_pid` 檢查設定前是否已被收養，是的話直接 `_exit(0)`。
    4. 訊號：`SIGINT`、`SIGUSR1` 忽略（關機由 master 統籌；log 等級由 master 改共用的 `log_level`）；`SIGCHLD`、`SIGTERM` 還原成 `SIG_DFL`，因為 master 安裝的 handler 會被 fork 繼承，child 不該執行它們。收到 `SIGTERM` 時 child 直接被終止；平常（非關機）被殺的 child 由 master 的 SIGCHLD 路徑清 slot 與控制權。
    5. 關掉不需要的 fd：`listen_fd` 與 input pipe 的讀端（`input_pipe[0]`），只保留寫端 `pipe_wr = input_pipe[1]`，這樣 reader 以外沒有人能讀這條 pipe。
    6. R5 開啟時設定 TCP keepalive（30 s idle、10 s 間隔、3 次）與 socket 逾時：接收 5 s（握手期限，`handshake()` 結束後改 60 s）、傳送 60 s（`HELLO_TIMEOUT_MS/1000`、`IDLE_TIMEOUT_MS/1000`）。
  - **live loop 每圈做四件事**：
    1. `poll(fd, 20 ms)`；`r < 0` 且不是 `EINTR` 才視為錯誤離場。**20 ms 的逾時就是 ring 的「輪詢週期」**：ring 沒有通知機制，所以每 20 ms 醒來檢查 `[off, head)`，新資料送出的延遲上限約 20 ms；有封包到達時 `poll` 立即返回，不必等滿。
    2. 處理封包：`r > 0` 時最多連續處理 **32** 個封包（每處理完一個用 `poll(…, 0)` 再看有沒有下一個），這個上限讓一個狂送封包的 client 不會餓死下面的串流與閒置檢查。
    3. 閒置檢查（R5）：`mono_ms() - last_rx_ms > IDLE_TIMEOUT_MS`（60 s 沒收到任何封包）→ 回 `ERROR IDLE_TIMEOUT`、記 log、離場。這個檢查是必要的，因為 socket 的 `SO_RCVTIMEO` 只對阻塞中的 `read` 生效，而主迴圈卡在 `poll`。client 平常靠 PING 或其他封包維持。
    4. `pump(&s, UINT64_MAX, 1)`：送出 ring 新增內容和（必要時）STATUS。失敗時 `errno == EAGAIN`（`SO_SNDTIMEO` 逾時）的離場原因寫成 `"send timeout (client not reading)"`，其餘用 `strerror(errno)`。
  - **一個慢 client 只卡自己**：`pump()` 在 `write()` 阻塞時只影響這個 child，其他 child 與 reader 不受影響，這是整個「fork per client」架構的目的。
- **細節**：
  - `--unsafe=timeout` 時：沒有 keepalive、沒有 socket 逾時、沒有 HELLO 期限、沒有閒置斷線。
  - 呼叫 `handshake()` 之後立刻 `s.last_rx_ms = mono_ms()`，讓閒置計時從握手結束算起，而不是從連線建立算起。
  - `handle_packet()` 回傳 `-1` 時以 `"send failed"` 結束。
  - 離開迴圈的唯一方式是 `finish()`（`_exit(0)`），所以函式沒有 `return`。
- **關係**：被誰呼叫 → `server/main.c` `handle_accept()`（fork 後的 child 分支，`main.c:188`）；呼叫 → `handshake()`、`handle_packet()`、`pump()`、`finish()`、`set_keepalive()`/`set_io_timeouts()`（`lib/net.c`）、`bench_log_set_role()`（`lib/log.c`）。

---

## `server/ring.c`

`server/ring.c` 實作放在 `MAP_SHARED` 共享記憶體裡的歷史環狀緩衝區（ring buffer）。reader 行程把 UART 收到的位元組（`REC_DATA`）以及各種事件（`REC_EVENT`，如 device lost、control taken）依序追加在同一條環上；master、reader、每個 per-client child 都會呼叫本檔的函式，所以這份程式碼會在三種行程中執行，但共享記憶體在 `fork()` 之前就已建立（`server/main.c:344-351`），大家看到的是同一塊資料。本檔對外提供 `ring_init`、`ring_append`、`ring_event`、`ring_seek`、`ring_copy`、`ring_repair` 六個函式（宣告在 `server/server.h:107-113`，標註「all require the lock held」），兩個 static 輔助函式 `ring_put`、`ring_get` 負責處理繞回（wrap-around）。本檔不自己加鎖，鎖由 `server/robust.c` 的 `shm_lock()` / `shm_unlock()` 提供；它依賴 `struct bench_shm`、`struct ring_rec`、`REC_SIZE()`、`REC_MAX_DATA`（`server/server.h`）以及 `now_ns()`（`lib/util.c`）、`UNSAFE(R_OVERRUN)`（`include/bench_util.h`）。

資料結構要點（供後面各函式引用）：

- 偏移（offset）是邏輯位址，只增不減（uint64_t）；實體位置是 `offset % cap`。`head` 是下一筆記錄的寫入點，`tail` 是最舊的有效記錄，有效資料為 `[tail, head)`。
- 每筆記錄為 `struct ring_rec`（16 位元組：`len` 4、`type` 2、`code` 2、`ts_ns` 8）加上 `len` 位元組的資料，總長由 `REC_SIZE(len)` 無條件進位到 8 的倍數，`len` 上限 `REC_MAX_DATA = 1024`，所以單筆記錄最大 1040 位元組。
- `cap` 由 `--history` 決定：預設 64K，小於 4096 會被拉到 4096，並進位為 8 的倍數（`server/main.c:284,321-323`）。因此單筆記錄遠小於 `cap`。
- 協定中的 DATA/EVENT `offset` 欄位是該記錄「結尾」的邏輯偏移（見 `docs/DECISIONS.md` Protocol 一節），這是 `server/session.c` 在 `ring_copy` 之後逐筆累加 `REC_SIZE` 算出來的，不存在環裡。

### `ring_put`
*位置：`server/ring.c:24`　簽章：`static void ring_put(struct bench_shm *s, uint64_t off, const void *src, size_t n)`*

- **用途**：把 `src` 的 `n` 位元組寫到環上邏輯偏移 `off` 的位置，必要時繞回環的開頭。
- **設計**：環是一塊固定大小的線性陣列，邏輯偏移取 `% cap` 得實體位置；一筆記錄可能剛好跨過陣列尾端，所以不能一次 `memcpy`。這裡先算出從 `pos` 到陣列尾端還剩多少（`first`），最多複製 `min(first, n)`，若還有剩就從 `s->ring` 開頭接著寫。把這個兩段式邏輯收在一個函式裡，`ring_append` 就能不管繞回，header 和 payload 都用它寫。
- **細節**：
  - `cap - pos` 的範圍是 1 到 `cap`，不會是 0，所以 `first` 至少為 1，不會有空的第一段。
  - 不檢查 `n <= cap`，也不檢查是否蓋掉有效資料；這是呼叫端（`ring_append`）的責任，它在寫之前先淘汰舊記錄讓 `head + size - tail <= cap`。
  - 不加鎖、不動 `head` / `tail`，只做位元組複製。
  - 因為 `REC_SIZE` 是 8 的倍數，header（16 位元組）可能被切成兩段（`pos = cap - 8` 時），這也由兩段式複製處理。
- **關係**：被誰呼叫 → `ring_append`（`server/ring.c:67,69`）；呼叫 → `memcpy`。

### `ring_get`
*位置：`server/ring.c:33`　簽章：`static void ring_get(const struct bench_shm *s, uint64_t off, void *dst, size_t n)`*

- **用途**：`ring_put` 的反向動作：從環上邏輯偏移 `off` 讀 `n` 位元組到 `dst`，必要時從環開頭接著讀。
- **設計**：與 `ring_put` 相同的 `pos`/`first` 運算，只是複製方向相反。`s` 宣告為 `const`，表示讀取不改共享狀態，也方便 `ring_copy`、`ring_seek`、`ring_repair` 共用。讀取 header 時一律用 `ring_get` 複製到區域變數 `struct ring_rec`，而不是直接用指標轉型，原因是 header 可能橫跨環尾（不連續），直接轉型會讀到環外的記憶體。
- **細節**：
  - 同樣不檢查 `off` 是否落在 `[tail, head)`。`--unsafe=overrun` 下呼叫端不先比對 `tail`，所以這裡可能讀到已被覆寫的位元組（見 `ring_copy`）。
  - 不加鎖；呼叫端須持有鎖，否則會讀到寫了一半的資料。
- **關係**：被誰呼叫 → `ring_append`（淘汰時讀舊 header，`server/ring.c:57`）、`ring_seek`（`:93`）、`ring_copy`（`:115,132`）、`ring_repair`（`:151`）；呼叫 → `memcpy`。

### `ring_init`
*位置：`server/ring.c:42`　簽章：`void ring_init(struct bench_shm *s, uint64_t cap)`*

- **用途**：設定環的容量並把 `head`、`tail` 歸零，使環為空。
- **設計**：只在 master 啟動時呼叫一次，位置在 `mmap()` 建立共享記憶體與 `shm_mutex_init()` 之後、任何 `fork()` 之前（`server/main.c:344-351`），此時只有一個行程，不需要鎖。共享記憶體是 `MAP_ANONYMOUS` 映射，內容本來就是 0，所以這裡不需要另外清空 `ring[]`。
- **細節**：
  - `cap` 傳入的是 `g_cfg.history`，已在 `server/main.c:321-323` 保證至少 4096 且是 8 的倍數；`ring_init` 本身不做驗證。
  - 整個共享區大小為 `sizeof(struct bench_shm) + cap`（`server/main.c:344`），`ring[]` 是結構尾端的彈性陣列（flexible array member）。
  - 重啟 benchd 會重新 `ring_init`，邏輯偏移從 0 開始；client 靠 `server_start_ns` 判斷 resume 偏移是否屬於同一次啟動（`server/session.c:266`）。
- **關係**：被誰呼叫 → `server/main.c:351`（`main`）。

### `ring_append`
*位置：`server/ring.c:49`　簽章：`void ring_append(struct bench_shm *s, uint16_t type, uint16_t code, const void *data, uint32_t len)`*

- **用途**：在 `head` 追加一筆記錄（header + 資料），空間不夠時從 `tail` 起淘汰最舊的整筆記錄。呼叫端必須持有共享鎖。
- **設計**：
  - 淘汰單位是「整筆記錄」：迴圈條件 `head + size - tail > cap` 表示寫入後已用空間會超過容量，就讀出 `tail` 處的 header、把 `tail` 前進 `REC_SIZE(old.len)`，直到放得下為止。這樣 `[tail, head)` 永遠由完整記錄組成，client 的 replay 才不會從記錄中間開始。因為 `size <= 1040 < cap`，迴圈一定會結束。
  - 淘汰時順便檢查鏈是否損壞：若舊 header 的 `len > REC_MAX_DATA`，或 `tail + osz > head`（記錄超出 `head`），代表環的內容已不可信，就把 `tail = head`（丟掉全部歷史）並跳出迴圈，而不是沿著垃圾走下去。這對應 `docs/DECISIONS.md` 的「Records evicted from a corrupt chain (impossible length) reset `tail = head` rather than walking garbage」。
  - 寫入順序是先寫 header、再寫資料，最後才 `s->head += size`（程式註解 `/* publish */`）。`head` 最後才前進，所以持鎖的行程若在中途死掉（R7 robust mutex 要處理的情況），`head` 之後最多留下一筆「看不見的」半成品，`[tail, head)` 仍然是完整記錄。淘汰階段只會把 `tail` 往前推到記錄邊界，同樣不會留下不一致。
  - 寫進去的 `ts_ns` 用 `now_ns()`（`CLOCK_REALTIME`），client 的 `:ts` 時間戳記就是用它。
- **細節**：
  - `len` 超過 `REC_MAX_DATA`（1024）會被截成 1024，多的資料靜默丟棄；reader 的讀取緩衝區剛好是 `REC_MAX_DATA` 大（`server/reader.c:232`），所以正常情況不會被截。
  - `len == 0` 時不呼叫第二次 `ring_put`（`if (len)`），只有 16 位元組 header 進位成 16。
  - 進位產生的 padding 位元組不會被寫入，保留環上原本（舊記錄）的內容；讀取端靠 `len` 取資料，不看 padding。
  - 淘汰迴圈不檢查 `old.type`，這一點由 `ring_repair` 補（它會驗證 type）。
  - 淘汰迴圈與 `--unsafe=overrun` 無關；R10 只影響讀的一方（`ring_copy` 與 `session.c` 的 `pump`），寫入端永遠安全地淘汰。
  - 此函式本身不會更新 `rx_bytes` 等統計，這些由 reader 在同一個臨界區內自己更新（`server/reader.c:246-248`）。
  - 沒有回傳值，不會失敗。
- **關係**：被誰呼叫 → `ring_event`（`server/ring.c:84`）、reader 行程的 `server/reader.c:246`（`REC_DATA`，從 tty 讀到的資料）；呼叫 → `ring_get`、`ring_put`、`now_ns`（`lib/util.c`）。

### `ring_event`
*位置：`server/ring.c:73`　簽章：`void ring_event(struct bench_shm *s, uint16_t code, const char *fmt, ...)`*

- **用途**：用 printf 風格格式化一段文字，當作 `REC_EVENT` 記錄追加到環上，`code` 是事件代碼（`EV_DEVICE_LOST`、`EV_CTRL_TAKEN`、`EV_CTRL_RELEASED`、`EV_BAUD_CHANGED`、`EV_SERVER_SHUTDOWN` 等，定義在 `include/bench_proto.h`）。呼叫端須持有共享鎖。
- **設計**：事件和 UART 資料放在同一條環、同一個順序，這是 benchd 不需要 child 之間 IPC 的關鍵：每個 child 沿著自己的偏移讀，就會在資料流中的正確位置看到「control taken」「device lost」，replay 與即時串流的順序一致。所以事件不走另一個通道，而是呼叫 `ring_append(REC_EVENT, ...)`。格式化到 256 位元組的堆疊緩衝區，再交給 `ring_append`，記錄長度遠小於 `REC_MAX_DATA`。
- **細節**：
  - `vsnprintf` 回傳負值（編碼錯誤）時 `n` 設為 0，產生一筆空文字的事件；回傳值 ≥ 256（被截斷）時 `n` 取 255，也就是只存實際寫入緩衝區的 255 個字元。
  - 存進環的文字不含結尾的 `'\0'`，長度由 `len` 決定。
  - 宣告帶 `__attribute__((format(printf, 3, 4)))`（`server/server.h:110`），編譯器會檢查格式字串與引數。
  - `EV_DROPPED` 不是用這個函式產生的：它是 session 在 client 落後被追過時直接組給該 client 的封包（`server/session.c` `send_dropped`），不進環。
  - 所有呼叫點都已持鎖：`server/control.c:46,58,75`（位於 `ctrl_take_locked` / `ctrl_release_locked`）、`server/reader.c:100,111,180`（device lost / back、baud 變更）、`server/main.c:218`（shutdown 事件）。
- **關係**：被誰呼叫 → `server/control.c`、`server/reader.c`、`server/main.c`（`shutdown_server`）；呼叫 → `ring_append`、`vsnprintf`。

### `ring_seek`
*位置：`server/ring.c:88`　簽章：`uint64_t ring_seek(struct bench_shm *s, uint64_t target)`*

- **用途**：回傳「大於或等於 `target` 的第一個記錄邊界」的邏輯偏移（找不到就回傳 `head`）。呼叫端須持有共享鎖。
- **設計**：環裡沒有索引，記錄是變長的，所以只能從 `tail` 一筆一筆跳過去（每次加 `REC_SIZE(r.len)`），直到 `off >= target` 或到達 `head`。這個函式存在的理由是 client 的起點不一定剛好是記錄邊界：新 client 的 replay 起點是「`head - replay` 位元組前」（任意位元組位置），必須往後對齊到下一筆記錄的開頭，否則 `ring_copy` 會把記錄中間當成 header。resume 的情況下 client 回報的是「最後看到的記錄結尾」，本身就是邊界，會原樣回傳。
- **細節**：
  - 若 `target <= tail`，迴圈不執行，回傳 `tail`。
  - 遇到 `len > REC_MAX_DATA`（鏈損壞）直接回傳 `head`，等於放棄歷史、只看新資料。
  - 最後 `off > head` 時夾到 `head`（`target` 超過 `head`，或最後一筆記錄的結尾越過 `head`）。
  - 複雜度與 `[tail, target)` 內的記錄數成正比，最壞約 64 KiB / 16 位元組 = 4096 筆（預設容量，且每筆至少 16 位元組），且是在持鎖狀態；只在 client 加入握手時呼叫一次，所以可以接受。
  - 不受 `--unsafe=overrun` 影響。
- **關係**：被誰呼叫 → `server/session.c:272`（resume）、`:280`（replay 起點）；呼叫 → `ring_get`。

### `ring_copy`
*位置：`server/ring.c:109`　簽章：`size_t ring_copy(struct bench_shm *s, uint64_t *off, uint64_t end, uint8_t *out, size_t outcap)`*

- **用途**：從 `*off` 開始，把完整的記錄（header + 資料 + padding，維持環內的版面）複製到 `out[]`，最多 `outcap` 位元組、最遠到 `end`，同時把 `*off` 前進到下一個尚未複製的記錄，回傳複製了多少位元組。呼叫端須持有共享鎖。
- **設計**：
  - child 把資料「搬出」環再於鎖外送網路：鎖只在這個 memcpy 期間持有（`server/session.c` `pump` 的註解：network writes happen with the lock released），所以慢速 client 不會卡住別人，這是 `fork()` 隔離慢 client 之外的另一層保護。`ring_copy` 一次最多搬 `outcap` 位元組（session 傳入 `sizeof copybuf` = 64 KiB），裝不下的記錄留給下一輪（`n + rs > outcap` 就 break，`pump` 會再呼叫）。
  - 只複製整筆記錄，所以 `*off` 永遠停在記錄邊界，之後的呼叫可以繼續接著讀；輸出保留環內的版面，呼叫端用 `REC_SIZE(r->len)` 逐筆往前走（`server/session.c:157-164`），不需要額外的長度表。
  - 鏈損壞時（`len > REC_MAX_DATA`）安全模式把 `*off` 設為 `end` 並結束，跳過剩下的資料，不再信任任何東西；這與 `docs/DECISIONS.md` 對損壞鏈的處理一致。
  - 複製後把輸出中的 `len` 改寫成（可能被夾過的）`len`（`((struct ring_rec *)(out + n))->len = len`），使輸出中每筆記錄的 `REC_SIZE(r->len)` 與這裡實際推進的 `rs` 相等；在安全模式下 `len` 沒被改過，這行等於無作用，它只在 R10 unsafe 才有意義。
- **細節**：
  - `--unsafe=overrun`（R10）：正常情況下 `pump` 會先檢查 `s->off < tail`（表示 client 被追過、舊資料已被覆寫），若是就把 `off` 拉到 `tail` 並在解鎖後送出 `DROPPED` 事件（偵測與調整 `server/session.c:138-141`，送出 `:153`）。unsafe 時這個檢查被關掉，`*off` 可能指向已被新資料覆蓋的位置，讀出來的「header」是垃圾。此時 `len > REC_MAX_DATA` 不跳過，而是夾成 1024 後照樣複製（`len = REC_MAX_DATA`），亂碼包括舊的 header 位元組就會顯示到畫面上，這就是 `docs/DECISIONS.md` 說的「clamps it to 1024 and carries on」。`type` 不是 `REC_EVENT` 的垃圾記錄在 `send_record` 裡一律當 DATA 送出。
  - `o + rs > end` 時：若這一輪還沒複製任何記錄（`n == 0`），把 `o = end` 以避免永遠無法前進（無窮迴圈）；已有輸出就保留並下一輪再說。程式註解說這只可能在 unsafe 時發生：在一致的鏈上，`end` 取自 `min(head, limit)`，而 `limit` 要麼是 `UINT64_MAX`，要麼是握手時記下的 `snap_head`（`server/session.c:310,467`），兩者都落在記錄邊界上。
  - `o >= end` 時迴圈不執行，回傳 0 且 `*off` 不變，表示沒有可搬的資料；回傳 0 也可能發生在 `n == 0` 時因 `o + rs > end` 而把 `*off` 直接設為 `end`（unsafe 的特殊情況）。`pump` 以 `n == 0 || s->off >= end` 作為結束條件（`server/session.c:182`）。
  - 輸出緩衝區 `copybuf` 有 `aligned(8)`（`server/session.c:39`），且 `n` 永遠是 8 的倍數，所以 `out + n` 轉成 `struct ring_rec *` 是對齊的。
- **關係**：被誰呼叫 → `server/session.c:146`（`pump`，握手後的 replay 與之後的即時串流都走這裡）；呼叫 → `ring_get`；讀取 `UNSAFE(R_OVERRUN)`。

### `ring_repair`
*位置：`server/ring.c:142`　簽章：`int ring_repair(struct bench_shm *s)`*

- **用途**：持鎖行程死亡後（`EOWNERDEAD`）重新檢查 `tail..head` 這條記錄鏈，不一致就把 `tail = head`（丟掉歷史），回傳 1；一切正常回傳 0。
- **設計**：`ring_append` 雖然以「最後才推進 `head`」降低損壞機率，但共享記憶體上的資料結構被一個死掉的行程碰過，就不能假設它完好；這個函式是保險：先做整體檢查（`tail > head`，或 `head - tail > cap`），再從 `tail` 沿著 `len` 逐筆走到 `head`，每筆都驗證 `len <= REC_MAX_DATA`、`type` 是 `REC_DATA` 或 `REC_EVENT`、記錄結尾不超過 `head`，且最後剛好落在 `head`（`while (off < head)` 配合每筆結尾 ≤ `head`）。任何一項不符就放棄歷史（`tail = head`），因為變長記錄沒有辦法從壞掉的地方重新同步。丟掉歷史比讓所有 client 收到垃圾好。
- **細節**：
  - 只檢查 `[tail, head)`，不檢查 `head` 之後的位元組，那是尚未發布的半成品，之後會被新記錄覆蓋。
  - 回傳值被 `shm_lock` 存進 `pending_repaired`，在 `shm_unlock` 解鎖之後再印警告（「ring was inconsistent: tail reset to head」或「ring consistent」），因為鎖內不做 I/O（見 `docs/DECISIONS.md` Threads and synchronisation）。
  - 只在 robust mutex 回報 `EOWNERDEAD` 時才會被呼叫；`--unsafe=robust-mutex`（R7）下 `shm_mutex_init` 不設 `PTHREAD_MUTEX_ROBUST`（`server/robust.c:34`），不會有 `EOWNERDEAD`，這個函式就不會被執行；持鎖行程死亡後，其他要取鎖的行程會一直阻塞在 `pthread_mutex_lock`（`docs/DECISIONS.md` 提到 reader 的 SIGTERM handler 為此 `alarm(2)`）。
  - 呼叫時鎖已由 `pthread_mutex_lock` 取得（雖然尚未 `pthread_mutex_consistent`），所以仍算是「持鎖」。
  - 與 `ring_append` 不同，這裡有檢查 `type`。
- **關係**：被誰呼叫 → `shm_lock`（`server/robust.c:45`）；呼叫 → `ring_get`。

---

## `server/control.c`

這個檔案實作「誰可以對 UART 打字」的仲裁（control，寫入權）。仲裁狀態只有 shared memory 裡的一組欄位：`ctrl_pid`、`ctrl_name`、`ctrl_last_input_ns`，以及啟動時由 `--idle-release` 設定的 `idle_release_sec`（`server/server.h:66-69`）。所有改動都必須在 `shm_lock()` 持有的 robust mutex 之下進行，所以檔內函式名稱帶 `_locked` 的都假設呼叫者已經上鎖；只有 `ctrl_idle_sec`、`ctrl_auto_release_in`（純讀取，函式本身不上鎖）與 `ctrl_log_note`（只寫 log，必須在解鎖後呼叫）不由自己取鎖；前兩者實際的呼叫端（`ctrl_take_locked`、`fill_status`）都在鎖內呼叫，欄位才讀得一致。程式碼會在不同行程中執行：`ctrl_take_locked` 跑在 per-client child（`server/session.c`），`ctrl_release_locked` 與 `ctrl_check_idle_locked` 跑在 reader 行程（`server/reader.c`），`ctrl_release_locked` 另外也跑在 master 行程的 `reap_children()`（`server/main.c:80`；SIGCHLD 只設旗標，由 master 主迴圈呼叫它）。`ctrl_log_note` 則由 reader 與 master 呼叫。TAKE 沒有排隊也沒有強制搶奪；釋放有四條路徑：明確的 RELEASE、閒置逾時、正常斷線（child 的 `finish()` 經 input pipe 通知 reader）、以及 child 當掉時由 master 的 `reap_children()` 直接釋放。每一次取得／釋放都會用 `ring_event()` 寫成 ring 裡的 EVENT 記錄，所以所有 viewer 看到的順序一致。本檔依賴 `server/server.h`（shm 結構與宣告）、`server/ring.c` 的 `ring_event`、`lib/util.c` 的 `now_ns` 與 `bench_strlcpy`、`include/bench_util.h` 的 `UNSAFE()` 巨集，以及 `include/bench_log.h` 的 `LOGI`。`--unsafe=ctrl-release`（R6）會關掉所有自動釋放：`finish()` 不送 `PIPE_RELEASE`、`reap_children()` 不釋放、`ctrl_take_locked` 不做 `kill(pid, 0)` 偵測、`ctrl_check_idle_locked` 不檢查閒置；只剩明確 RELEASE 能放掉控制權，controller 一旦當機，它的 dead pid 就永遠留在 `ctrl_pid`，沒有人能再打字。

### `ctrl_idle_sec`
*位置：`server/control.c:23`　簽章：`uint32_t ctrl_idle_sec(const struct bench_shm *s, uint64_t now)`*

- **用途**：回傳目前 controller 已經多少整秒沒有輸入；沒有 controller 時回傳 0。
- **設計**：閒置時間不另外存，而是由「現在時間」減去 `ctrl_last_input_ns` 即時算出，所以不需要任何計時器去更新欄位。`now` 由呼叫者傳入而不是在函式內呼叫 `now_ns()`，這樣同一個函式在同一次鎖內被多次使用（例如 `ctrl_take_locked` 同時填 `holder_idle_sec` 與 `auto_release_in_sec`）時會用同一個時間點，結果前後一致，也方便測試。
- **細節**：
  - `!s->ctrl_pid` 或 `now < s->ctrl_last_input_ns` 時回傳 0。後者是因為 `now_ns()` 取的是 `CLOCK_REALTIME`（`lib/util.c:15-20`），系統時間被往回調時，沒有這個保護會讓無號減法變成極大值，造成誤判閒置。
  - 計算為 `(now - ctrl_last_input_ns) / 1000000000ull`，單位是整秒，向下取整，強制轉成 `uint32_t`。
  - 回傳的是主機位元組序；要放進 `bn_ctrl_result` 時由呼叫者做 `htobe32`。
  - 與 `--unsafe` 無關：R6 開啟時它仍照常計算（STATUS 與 TAKE 被拒時仍會顯示閒置秒數），只是閒置釋放路徑被關掉，不再有人用它釋放控制權。（INFO 的 `ctrl.idle_sec` 在 `server/sysinfo.c:171-172` 自行以同樣公式計算，並沒有呼叫本函式。）
- **關係**：被誰呼叫 → `ctrl_auto_release_in`、`ctrl_take_locked`（本檔，只在拒絕分支）；`fill_status`（定義於 `server/session.c:114`，呼叫在 `server/session.c:122`，填 `bn_status.ctrl_idle_sec`；`fill_status` 本身在 `server/session.c:150` 於 `shm_lock` 內被呼叫）。呼叫 → 無。

### `ctrl_auto_release_in`
*位置：`server/control.c:30`　簽章：`uint32_t ctrl_auto_release_in(const struct bench_shm *s, uint64_t now)`*

- **用途**：算出距離閒置自動釋放還剩幾秒；若不會自動釋放則回傳 `UINT32_MAX`。
- **設計**：把「是否有自動釋放」與「還剩多久」收在同一個函式，讓 TAKE 的回應能直接告訴客戶端倒數時間（granted 時告知自己的倒數，denied 時告知對方的倒數）。用 `UINT32_MAX` 表示「永不」，與協定定義一致（`include/bench_proto.h:109` 的註解 `UINT32_MAX = never`），客戶端看到這個值就顯示 "no auto-release"（`client/session.c:284-285`、`client/subcmd.c:267-268`）。
- **細節**：
  - 回傳 `UINT32_MAX` 的三個條件：沒有 controller（`!ctrl_pid`）、R6 開啟（`UNSAFE(R_CTRL_RELEASE)`，因為閒置釋放路徑被關掉了，倒數沒有意義）、或 `idle_release_sec == 0`（`--idle-release 0` 代表永不釋放，見 `server/main.c:54`）。
  - 否則 `idle = ctrl_idle_sec(s, now)`；`idle >= idle_release_sec` 時回傳 0，否則回傳 `idle_release_sec - idle`。這裡的比較是 `>=`，而實際釋放條件在 `ctrl_check_idle_locked` 是嚴格 `>`，所以剛好到達門檻的那一秒會顯示 0，但還沒被放掉；差距最多是 reader 約每秒檢查一次的週期。
  - 回傳值是主機位元組序，呼叫者自行 `htobe32`。
- **關係**：被誰呼叫 → `ctrl_take_locked`（本檔，granted 與 denied 兩個分支）。呼叫 → `ctrl_idle_sec`（本檔）。

### `ctrl_take_locked`
*位置：`server/control.c:38`　簽章：`int ctrl_take_locked(struct bench_shm *s, pid_t me, const char *name, struct bn_ctrl_result *res)`*

- **用途**：處理一個 client 的 TAKE 請求：控制權閒置、持有者已死、或本來就是自己時授予並回傳 1，否則拒絕並回傳 0；同時填好要回給客戶端的 `bn_ctrl_result`。
- **設計**：整個判斷與修改都在呼叫者持有的鎖內完成，所以「檢查有沒有人持有」與「寫入自己」之間不會被別的 child 插進來，不會有兩個 child 同時拿到控制權。沒有排隊、沒有強制搶奪（見檔頭註解），被拒絕時只回報持有者名字、閒置秒數與自動釋放倒數，讓使用者自己決定要不要等。在拒絕前多做一次 `kill(pid, 0)` 偵測，是為了處理「持有者已死但還沒有人發現」的空窗：持有者若是 child 當掉，master 的 SIGCHLD reaper 會處理，但在 reaper 跑到之前（或 reaper 與這次 TAKE 競爭時），TAKE 自己就能把死掉的持有者清掉，不用等。清掉時寫的事件文字與一般斷線一致（`(disconnect)`）。
- **細節**：
  - 開頭 `now_ns()` 取一次時間並 `memset(res, 0, sizeof *res)`，因此 granted 時 `holder_idle_sec` 為 0、`pad` 為 0。
  - 死亡持有者檢查只在 `ctrl_pid != 0 && ctrl_pid != me && !UNSAFE(R_CTRL_RELEASE)` 時進行：`kill(ctrl_pid, 0) < 0 && errno == ESRCH` 才算死；若 `kill` 因 `EPERM` 失敗則視為還活著，不釋放。確認死亡後用 `ring_event(EV_CTRL_RELEASED, "control released by %s (disconnect)", ctrl_name)` 寫事件，再把 `ctrl_pid` 設 0、`ctrl_name[0]` 設 0。這條路徑只寫 ring 事件，不產生 `ctrl_note`，所以不會有對應的 `LOGI`。
  - 進入授予分支的條件是 `ctrl_pid == 0 || ctrl_pid == me`。`fresh = (ctrl_pid == 0)` 先記下；不論是否 fresh，都會更新 `ctrl_pid = me`、複製 `name` 到 `ctrl_name`（`bench_strlcpy`，上限 `sizeof ctrl_name` = `BN_NAME_LEN` = 32）、並把 `ctrl_last_input_ns` 重設為 now。因此「自己已經持有又再 TAKE」會被再次授予並重置閒置計時，但不會多寫一筆 `EV_CTRL_TAKEN`（只有 fresh 才寫 `"control taken by %s"`）。
  - granted 時 `res->granted = 1`，`holder` 填自己的名字，`auto_release_in_sec = htobe32(ctrl_auto_release_in(s, now))`（此時 `ctrl_last_input_ns == now`，所以通常是完整的 `idle_release_sec`，或 `UINT32_MAX`）。回傳 1。
  - denied 時 `holder` 填現任持有者名字，`holder_idle_sec = htobe32(ctrl_idle_sec(...))`，`auto_release_in_sec = htobe32(ctrl_auto_release_in(...))`。回傳 0。`granted` 由 `memset` 保持為 0，程式也明確寫了 `res->granted = 0`。
  - `--unsafe=ctrl-release`（R6）：跳過 `kill(pid, 0)` 偵測，所以持有者死掉後 `ctrl_pid` 還是舊的 dead pid，每次 TAKE 都被拒絕，而且 `auto_release_in_sec` 回報 `UINT32_MAX`。
  - 這個函式不做 I/O、不寫 log；呼叫者在解鎖之後才根據回傳值寫 log（符合 docs/DECISIONS.md「log 與其他 I/O 不在鎖內」）。
- **關係**：被誰呼叫 → `server/session.c:364`（處理 `BN_TAKE`，child 行程，之後 `bn_send(BN_CTRL_RESULT)`）。呼叫 → `now_ns`、`bench_strlcpy`（`lib/util.c`）、`ring_event`（`server/ring.c`）、`ctrl_auto_release_in`、`ctrl_idle_sec`（本檔）。

### `ctrl_release_locked`
*位置：`server/control.c:71`　簽章：`void ctrl_release_locked(struct bench_shm *s, const char *reason, struct ctrl_note *note)`*

- **用途**：釋放目前的控制權（不論持有者是誰），寫一筆 EVENT 到 ring，並把「誰、為什麼被釋放」記到 `note` 讓呼叫者在解鎖後寫 log。
- **設計**：四條釋放路徑最後都匯集到這個函式，所以事件文字格式（`control released by NAME (REASON)`）與欄位清除只有一份實作。它自己不寫 log，而是把資訊放進 `struct ctrl_note`（`server/server.h:124`），由呼叫者解鎖後交給 `ctrl_log_note`：log 可能是寫檔或 stderr 的 I/O，不該在持有跨行程鎖時做，否則慢速的 I/O 會擋住所有其他行程。`note` 可為 NULL，表示呼叫者不需要 log。
- **細節**：
  - `!s->ctrl_pid` 時直接 return，什麼都不做，所以對沒人持有的狀態呼叫是安全的。reader 與 master 兩處呼叫端各自先確認 `ctrl_pid` 就是離開的那個 pid（`server/reader.c:192`、`server/main.c:95`）才呼叫，因此同一個 controller 的離開被兩邊都處理時，後到的一邊不會呼叫到這裡。
  - 先 `ring_event(EV_CTRL_RELEASED, ..., s->ctrl_name, reason)` 再清欄位，因為事件文字要用到 `ctrl_name`。
  - `note` 非 NULL 時設 `happened = 1`，把 `ctrl_name` 複製到 `note->who`（大小 `BN_NAME_LEN` = 32），`reason` 複製到 `note->reason`（陣列大小 32，`bench_strlcpy` 會截斷）。
  - 最後 `ctrl_pid = 0`、`ctrl_name[0] = 0`。`ctrl_last_input_ns` 不清，因為沒有 controller 時 `ctrl_idle_sec` 會因 `!ctrl_pid` 回傳 0。
  - reason 字串實際值：reader 收到 `PIPE_RELEASE` 時使用 child 傳來的字串，明確 RELEASE 是 `"released"`（`server/session.c:377`），正常斷線是 `"disconnect"`（`finish()`，`server/session.c:72-73`），沒帶字串時預設 `"released"`（`server/reader.c:186`）；master 的 reaper 用 `"disconnect"`（`server/main.c:96`）；閒置檢查用 `"idle"`（本檔）。
  - 此函式本身不檢查 R6；各呼叫端自己決定：`server/main.c:95` 在 `UNSAFE(R_CTRL_RELEASE)` 時不呼叫，`finish()` 在 R6 時不送 `PIPE_RELEASE`，`ctrl_check_idle_locked` 在 R6 時直接返回。明確的 RELEASE 不受 R6 影響，仍會走到這裡。
- **關係**：被誰呼叫 → `ctrl_check_idle_locked`（本檔）；`handle_pipe_msg` 的 `PIPE_RELEASE` 分支（`server/reader.c:193`，reader 行程，先確認 `m->pid == ctrl_pid` 才釋放）；`reap_children`（`server/main.c:96`，master 行程，controller 當掉時）。呼叫 → `ring_event`（`server/ring.c`）、`bench_strlcpy`（`lib/util.c`）。

### `ctrl_check_idle_locked`
*位置：`server/control.c:85`　簽章：`void ctrl_check_idle_locked(struct bench_shm *s, uint64_t now, struct ctrl_note *note)`*

- **用途**：若目前的 controller 閒置超過 `idle_release_sec`，就以原因 `"idle"` 釋放控制權。
- **設計**：由 reader 行程的主迴圈每秒呼叫一次（`next_idle = now + 1000`，`server/reader.c:278-285`），而不是每個 child 各自設計時器：reader 是已經有事件迴圈的行程（`server/reader.c` 檔頭註解列有 "every second -> release control if the controller has been idle too long"），閒置檢查放在這裡不需要新增行程或訊號；而且 INPUT、BAUD、RELEASE 都經同一條 input pipe 由 reader 依序處理，閒置釋放也在同一個行程內序列化。`now` 由呼叫者傳入（`now_ns()`），與 `ctrl_idle_sec` 的理由相同。
- **細節**：
  - 提前 return 的情況：沒有 controller、R6 開啟、或 `idle_release_sec == 0`（永不釋放）。
  - 條件是 `now > ctrl_last_input_ns && now - ctrl_last_input_ns > idle_release_sec * 1000000000ull`（嚴格大於；先檢查 `now >` 以避免系統時間往回調造成無號溢位；乘法先轉成 `uint64_t` 避免 32 位元溢位）。
  - 所謂「輸入」包括 TAKE 成功（`ctrl_take_locked` 重設）、controller 送出 `BN_INPUT`（裝置為 UP 時）、以及 controller 送出 `BN_BAUD`；這幾處在 `server/session.c:350-351`、`395-397` 更新 `ctrl_last_input_ns`。
  - 解析度：reader 以 `next_idle = now + 1000`（毫秒）排程檢查，而主迴圈的 `poll` 逾時為 100 ms（`server/reader.c:236`），所以實際釋放時間會比設定值多出最多約 1.1 秒。
  - `--unsafe=ctrl-release`：整條閒置釋放路徑停用，函式直接返回。
- **關係**：被誰呼叫 → `server/reader.c:281`（reader 行程，主迴圈，鎖內呼叫，解鎖後再 `ctrl_log_note`）。呼叫 → `ctrl_release_locked`（本檔，原因字串 `"idle"`）。

### `ctrl_log_note`
*位置：`server/control.c:94`　簽章：`void ctrl_log_note(const struct ctrl_note *note)`*

- **用途**：如果 `note->happened` 為真，就寫一行 INFO log：`control released by <who> (<reason>)`。
- **設計**：它是 `ctrl_release_locked` 的「延後 log」另一半。釋放發生在鎖內，但 log 要在解鎖後輸出（docs/DECISIONS.md：「Logging and other I/O never happen inside the lock」）；因此釋放時只把資訊存進棧上的 `struct ctrl_note`，解鎖後再呼叫本函式。`note->happened` 預設為 0（呼叫端都以 `struct ctrl_note note = { 0 }` 初始化），所以在沒有發生釋放時（例如 `ctrl_release_locked` 因無人持有而 no-op、或條件不成立）呼叫它是安全的，什麼都不印。
- **細節**：
  - 使用 `LOGI`，是否真的輸出取決於 `include/bench_log.h` 的編譯期上限與執行期共享 log level（可被 SIGUSR1 切換）。
  - 本函式不接觸 shm，也不需要鎖，且只讀 `note`，所以可以在任何行程中呼叫。
  - 注意 `ctrl_take_locked` 在清除死亡持有者時只寫 ring 事件，不產生 note，所以那一種釋放不會有這行 log（TAKE 之後 child 會自己寫 `control granted/denied` 的 log）。
- **關係**：被誰呼叫 → `handle_pipe_msg` 的 `PIPE_RELEASE` 分支（`server/reader.c:195`）、reader 主迴圈的閒置檢查（`server/reader.c:283`）、`reap_children`（`server/main.c:102`）。呼叫 → `LOGI`（`include/bench_log.h`）。

---

## `server/robust.c`

這個檔案放 benchd 共享記憶體鎖的全部邏輯（R7），加上兩個走訪 `slots[]` 的小工具，以及 `--unsafe` 啟動橫幅。benchd 的 master、reader、每個 per-client child 都會連結這份程式碼：`shm_mutex_init` 與 `print_unsafe_banner` 只由 master 在 `main()` 裡、`fork()` 之前呼叫一次；`shm_lock` / `shm_unlock` 則由 master、reader、所有 child 在讀寫 `struct bench_shm` 時反覆使用（`server/main.c`、`server/reader.c`、`server/session.c`、`server/sysinfo.c` 中的 `shm_lock` / `shm_unlock` 呼叫成對出現）。這裡的 `pthread_mutex_t` 設為 `PTHREAD_PROCESS_SHARED` 並放在 `MAP_SHARED` 映射內，所以它鎖的是「行程」而不是執行緒；benchd 從不呼叫 `pthread_create`（見 docs/DECISIONS.md「Threads and synchronisation」）。檔案依賴 `server/server.h`（`struct bench_shm`、`BENCH_MAX_SLOTS`、`ring_repair` 原型）、`include/bench_util.h`（`UNSAFE()`、`R_ROBUST_MUTEX`、`bench_unsafe_mask`、`bench_unsafe_format`）與 `include/bench_log.h`（`LOGE` / `LOGW`）。

檔內有兩個 `static` 全域變數 `pending_recovery_log`（第 25 行）與 `pending_repaired`（第 26 行）。它們是每個行程各自一份的私有變數（不在共享記憶體中；`fork()` 後各行程各有副本），用來把「EOWNERDEAD 復原」這件事延後到解鎖之後才寫 log，原因是 docs/DECISIONS.md 規定臨界區內不做 I/O。

### `shm_mutex_init`
*位置：`server/robust.c:28`　簽章：`void shm_mutex_init(pthread_mutex_t *m)`*

- **用途**：初始化放在共享記憶體裡的互斥鎖 `g_shm->lock`，設成跨行程可用，並（預設）設成 robust。
- **設計**：鎖必須在 `mmap(MAP_SHARED)` 之後、第一次 `fork()` 之前初始化，這樣所有子行程繼承同一個映射時，鎖的狀態（unlocked、屬性）已經就緒。屬性物件 `pthread_mutexattr_t` 只是建立時用的設定單，所以初始化完立刻 `pthread_mutexattr_destroy`。`PTHREAD_PROCESS_SHARED` 是跨行程使用的必要條件，否則預設的 private 鎖放在共享記憶體裡行為未定義。`PTHREAD_MUTEX_ROBUST` 是 R7 的核心：持鎖行程若死亡（`SIGSEGV`、`abort()`、被 kill），下一個 `pthread_mutex_lock` 會得到 `EOWNERDEAD` 而不是永遠阻塞，讓 `shm_lock` 有機會修復共享狀態。
- **細節**：
  - 只有 `!UNSAFE(R_ROBUST_MUTEX)` 時才呼叫 `pthread_mutexattr_setrobust`；`--unsafe=robust-mutex` 下鎖仍是 process-shared，但是是普通鎖，持鎖者一死，所有之後要鎖的行程永遠卡住（`scripts/demo_r7.sh` 開頭註解：viewers 凍結、reader 無法 append、新 client 卡住）。判斷用的 `bench_unsafe_mask` 是 master 在第 329 行設好的。
  - 所有 `pthread_mutexattr_*` 與 `pthread_mutex_init` 的回傳值都沒有檢查；這些呼叫在正常參數下不會失敗。
  - 沒有回傳值；呼叫者傳入的是 `&g_shm->lock`。
- **關係**：被誰呼叫 → `server/main.c` 的 `main()`（第 350 行，緊接 `mmap` 之後、`ring_init` 之前）；呼叫 → libc 的 `pthread_mutexattr_*`、`pthread_mutex_init`，以及巨集 `UNSAFE()`。

### `shm_lock`
*位置：`server/robust.c:39`　簽章：`void shm_lock(struct bench_shm *s)`*

- **用途**：取得共享記憶體的全域鎖；若發現前任持鎖者死在臨界區內，先修復共享狀態再讓呼叫者繼續。
- **設計**：整個伺服器的共享狀態（ring、`slots[]`、控制權、統計）都由這一把鎖保護。持鎖者在更新到一半時死掉，狀態可能不一致，所以得到 `EOWNERDEAD` 時，呼叫者此刻已經持有鎖，必須在 `pthread_mutex_consistent` 之前把狀態檢查並修好，順序如下：
  1. `ring_repair(s)`（`server/ring.c:142`）先檢查 `tail > head` 或 `head - tail > cap`，成立就把 `tail = head` 並回傳 1；否則從 `tail` 走到 `head`，驗證每筆記錄的 `len`（不得超過 `REC_MAX_DATA`）、`type`（只能是 `REC_DATA` 或 `REC_EVENT`）以及是否越過 `head`，任一項不合就把 `tail = head`（丟棄歷史，換取不去走垃圾資料）並回傳 1；全部通過回傳 0。結果存進 `pending_repaired`。
  2. 若 `ctrl_pid` 非 0，用 `kill(ctrl_pid, 0)` 探測；回傳 -1 且 `errno == ESRCH` 表示該行程已不存在，就把 `ctrl_pid` 與 `ctrl_name[0]` 清掉。這樣死掉的控制者不會一直佔著控制權（與 R6 相同的精神）。只有 `ESRCH` 才清；其他錯誤（例如 `EPERM`）表示行程仍存在，不動。
  3. `slots_recount(s)` 重算 `viewers`，因為持鎖者可能死在更新 `slots[]` 與 `viewers` 之間。
  4. `s->lock_recoveries++`，這個計數器會出現在 INFO 的 `server.lock_recoveries`。
  5. `pthread_mutex_consistent(&s->lock)` 把鎖標記回一致。若不呼叫，之後 `pthread_mutex_unlock` 會讓鎖進入 `ENOTRECOVERABLE`，全伺服器之後都鎖不了。
  6. `pending_recovery_log = 1`，把 log 延到 `shm_unlock` 解鎖後才寫（臨界區內不做 I/O，見 docs/DECISIONS.md）。
- **細節**：
  - 函式本身無回傳值；`pthread_mutex_lock` 回傳 0 時什麼額外的事都不做，是熱路徑。
  - 回傳值既非 0 也非 `EOWNERDEAD`（例如 `ENOTRECOVERABLE`、`EINVAL`）時，`LOGE("pthread_mutex_lock: %s", strerror(rc))` 後直接 `abort()`。原因：沒有鎖就無法安全存取共享狀態，與其帶著不一致的狀態繼續，不如讓該行程死掉；若是 child，master 會從 `reap_children` 看到並清理。
  - 修復只在「呼叫者持有鎖」期間做，所以另一個行程不會看到修到一半的狀態。呼叫者隨後繼續它自己原本的臨界區工作。
  - `kill(ctrl_pid, 0)` 的 `errno` 判斷寫在 `&&` 右邊，在 `kill` 失敗後才讀，沒有被其他呼叫覆蓋的風險。
  - `--unsafe=robust-mutex` 下鎖不是 robust，`pthread_mutex_lock` 不會回傳 `EOWNERDEAD`，整個 `if` 分支不可能進入；持鎖者死亡後呼叫者就永遠阻塞。reader 的 SIGTERM handler 的 `alarm(2)`（`server/reader.c:40`）就是為了讓卡在這裡的 reader 仍能被結束。
  - 在 debug build 的 `BENCH_FAULT=die_in_lock[:N]` 下，第 N 個連線的 child 會 `shm_lock` 之後立刻 `abort()`（`server/session.c:291-296`），這就是 `scripts/demo_r7.sh` 製造 `EOWNERDEAD` 的方式（`BENCH_FAULT=die_in_lock:3`）。
  - 同一個行程若在臨界區內再次呼叫 `shm_lock` 會自我死結（預設 mutex 型別）；程式沒有這種呼叫，也不支援遞迴。
- **關係**：被誰呼叫 → `server/main.c`（`handle_accept`、`reap_children` 等）、`server/reader.c`、`server/session.c`（`handshake`、`finish` 等）、`server/sysinfo.c` 的 `sysinfo_build`；呼叫 → `ring_repair`（`server/ring.c`）、`slots_recount`（同檔）、`pthread_mutex_consistent`、`kill`、`LOGE`、`abort`。

### `shm_unlock`
*位置：`server/robust.c:60`　簽章：`void shm_unlock(struct bench_shm *s)`*

- **用途**：釋放全域鎖，並在剛才有發生 EOWNERDEAD 復原時，於鎖已釋放之後補寫一行警告 log。
- **設計**：log 寫在 `pthread_mutex_unlock` 之後，是為了遵守「臨界區內不做 I/O」：`LOGW` 會寫檔案或 stderr，可能因為終端機或磁碟慢而阻塞，若在持鎖時做，會讓所有行程陪著等。`pending_recovery_log` 是本行程私有的旗標，所以由當初觸發復原的那個行程自己負責印出訊息，不會重複也不會漏（其他行程有各自的副本，值為 0）。先把旗標歸零再印，避免之後的解鎖重複輸出。
- **細節**：
  - 訊息為 `EOWNERDEAD recovered: previous lock owner died inside the critical section`，後面接 ` (ring was inconsistent: tail reset to head)`（`pending_repaired` 非 0）或 ` (ring consistent)`。`scripts/demo_r7.sh` 結尾以 `show_server_log` 顯示 server log，開頭註解寫明 safe 模式會記錄 "EOWNERDEAD recovered"；該腳本本身並不 grep 這行訊息，成敗是看 viewer 是否仍收到資料與 `info` 是否在 3 秒內回應。
  - `pthread_mutex_unlock` 的回傳值被忽略。
  - `LOGW` 在 `LOG_COMPILE_LEVEL < 1` 的建置會被編譯掉；Makefile 的 release 用 `LOG_COMPILE_LEVEL=2`、debug 用 4，兩者都包含 WARN，所以預設建置都會輸出這行。
  - 如果復原行程在 `shm_lock` 之後、`shm_unlock` 之前就死了（例如在臨界區之後的程式碼中被終止），旗標隨行程消失，那次復原就不會有行程 log；但 `lock_recoveries` 已經在共享記憶體裡加過了。
- **關係**：被誰呼叫 → 與 `shm_lock` 成對，出現在 `server/main.c`、`server/reader.c`、`server/session.c`、`server/sysinfo.c` 的每個臨界區結尾；呼叫 → `pthread_mutex_unlock`、`LOGW`。

### `slots_recount`
*位置：`server/robust.c:70`　簽章：`uint32_t slots_recount(struct bench_shm *s)`*

- **用途**：掃描 `slots[]`，把「`pid` 非 0 且 `joined` 非 0」的槽位數量寫進 `s->viewers` 並回傳。
- **設計**：`viewers` 是衍生值，不用增減計數，而是每次從 `slots[]` 重算。這樣不會因為某個 child 異常死亡（沒機會減一）而讓計數漂移，也讓 `shm_lock` 的 EOWNERDEAD 復原可以直接呼叫它補救。`joined` 條件把「已被 master 保留但尚未完成 handshake」的槽位排除在外：master 在 `handle_accept` 會先把 `slots[slot].pid = -1` 當保留標記（`server/main.c`，`joined` 仍為 0），所以保留中的槽位不算 viewer，卻會被 `slots_in_use` 算進佔用數。
- **細節**：
  - 呼叫者必須持有鎖（`server.h` 的註解寫明 `lock held; updates s->viewers`）。
  - 迴圈範圍是 `BENCH_MAX_SLOTS`（1024），成本是 O(1024)，在客戶端數量小的情境下可忽略。
  - 回傳值與 `s->viewers` 相同，方便 `sysinfo_build` 直接用 `st.viewers = slots_recount(g_shm)`。
- **關係**：被誰呼叫 → `shm_lock`（復原路徑，本檔）、`server/main.c` 的 `reap_children`（child 結束後，第 100 行）、`server/session.c` 的 `finish`（第 69 行）與 `handshake`（第 252 行，加入後）、`server/sysinfo.c` 的 `sysinfo_build`（第 105 行）；呼叫 → 無。

### `slots_in_use`
*位置：`server/robust.c:80`　簽章：`int slots_in_use(struct bench_shm *s)`*

- **用途**：回傳 `slots[]` 中 `pid` 非 0 的槽位數，也就是目前已佔用（含保留中）的 client 數。
- **設計**：這個函式與 `slots_recount` 的差別是不看 `joined`：master 要判斷「是否已達 `--max-clients`」時，必須把剛 fork、尚未完成 handshake 的 child，以及 `pid = -1` 的保留槽位都算進去，否則一群還沒送 HELLO 的連線可以繞過上限（R9 `max-clients`）。它不修改共享狀態。
- **細節**：
  - 呼叫者必須持有鎖。
  - 與 `slots_recount` 一樣是 O(`BENCH_MAX_SLOTS`) 的線性掃描。
  - 在 `handle_accept` 中，`in_use` 先在鎖內取得，之後在鎖外才與 `g_cfg.max_clients` 比較；`--unsafe=max-clients` 時那個比較被略過，其餘不變。
- **關係**：被誰呼叫 → `server/main.c` 的 `handle_accept`（第 153 行，是唯一的呼叫點）；呼叫 → 無。

### `print_unsafe_banner`
*位置：`server/robust.c:89`　簽章：`void print_unsafe_banner(void)`*

- **用途**：若啟用了任何 `--unsafe` 項目，在 stderr 印出醒目的紅底警告橫幅，列出被停用的機制。
- **設計**：示範用的「不安全模式」不該被誤當成正常設定，所以啟動時讓它視覺上很明顯。橫幅直接 `fprintf(stderr, ...)` 而不走 `LOGx`，因為它是給操作者看的啟動訊息，不受 log level 或 `LOG_COMPILE_LEVEL` 影響。顏色碼只在 `isatty(STDERR_FILENO)` 為真時輸出，導向檔案或管線時不會混入 ESC 序列。
- **細節**：
  - `bench_unsafe_mask` 為 0 時直接 return，什麼都不印。
  - 名稱清單由 `bench_unsafe_format(bench_unsafe_mask, list, sizeof list)`（`lib/util.c:285`）產生，`list` 是 256 位元組的區域緩衝區；格式是以逗號分隔的名稱，例如 `zombie,robust-mutex`（依 R1 到 R12 的位元順序，不是輸入順序）；`bench_unsafe_format` 在遮罩為 0 時會輸出 `(none)`，但這裡已先 return，所以不會印出。
  - 色碼 `\033[1;97;41m`（粗體、亮白字、紅底）加在整個橫幅之前，`\033[0m` 加在最後一條橫線之後、換行之前。
  - 橫幅由 `#` 組成的上下框線與三行文字組成：`UNSAFE MODE: robustness mechanisms DISABLED:`、縮排後的名稱清單 `%s`、以及 `(for before/after demonstrations only)`。
  - 客戶端專用名稱（如 `term-restore`、`reconnect`）若出現在清單中也會被列出，因為遮罩是共用的（DECISIONS.md：「The server ignores client-only names」）。
- **關係**：被誰呼叫 → `server/main.c` 的 `main()`（第 336 行，在設好 `bench_unsafe_mask` 與 log 之後、設定 `SIGPIPE` 與 `mmap` 之前）；呼叫 → `bench_unsafe_format`（`lib/util.c`）、`isatty`、`fprintf`。

---

## `server/sysinfo.c`

這個檔案負責組出 `INFO` 封包（`BN_INFO`，type 0x87）的 payload：一段由多行 `key=value\n` 組成的純文字，內容分成 `host.*`（主機）、`server.*`（benchd 本身）、`dev.*`（序列裝置）、`hist.*`（ring buffer 歷史）、`ctrl.*`（控制權）五組。程式碼只在 per-client child 行程裡執行：child 的 `session.c` 收到 `BN_INFO_REQ` 時呼叫 `sysinfo_build()`（`server/session.c:382`），再把結果用 `bn_send()` 以 `BN_INFO` 送回該 client；master、reader 不會呼叫它。檔案只對外提供一個函式 `sysinfo_build()`（宣告在 `server/server.h:133`），其餘四個 `static` 函式是它的輔助工具。依賴：`server.h` 的共用記憶體 `g_shm` 與 `shm_lock()/shm_unlock()`、`slots_recount()`（`server/robust.c`），以及 `lib/` 的 `now_ns()`、`bench_strlcpy()`、`bench_unsafe_format()`、`bench_log_level_name()`。整個檔案的設計原則寫在檔頭註解：先在鎖內把共用狀態複製成區域快照，離開鎖之後才做 `/proc` 檔案 I/O 與字串格式化（對應 `docs/DECISIONS.md`「鎖內不做 I/O」的決定）。

### `add`
*位置：`server/sysinfo.c:17`（含 `format(printf, 4, 5)` 屬性宣告於 `:15`）　簽章：`static size_t add(char *buf, size_t cap, size_t o, const char *fmt, ...)`*

- **用途**：在 `buf` 的偏移 `o` 處以 printf 格式附加一段文字，回傳附加後的新偏移，供 `sysinfo_build()` 一行接一行串接 `key=value\n`。
- **設計**：這是一個「永遠不會寫出界」的 `snprintf` 累加器。呼叫端只需把回傳值當成下一次的 `o`，不必每一行都檢查剩餘空間；緩衝區滿了之後後續呼叫會自動變成 no-op。用 `vsnprintf(buf + o, cap - o, ...)` 而不是 `sprintf`，因此即使 `host.cpu_model` 之類來自外部檔案的字串很長，也只會被截斷、不會溢位。偏移用 `size_t` 回傳而不是用指標傳入，讓呼叫端寫成 `o = add(..., o, ...)` 的單純形式。宣告處的 `__attribute__((format(printf, 4, 5)))` 讓編譯器（搭配專案的 `-Wformat=2`）檢查每一行的格式字串與參數型別，例如 `%llu` 必須配 `unsigned long long`，這也是 `sysinfo_build()` 到處做 `(unsigned long long)` 轉型的原因。
- **細節**：
  - 若 `o >= cap`（已滿或偏移已經超出），直接回傳 `o`，不呼叫 `vsnprintf`。
  - `vsnprintf` 回傳負值（編碼錯誤）時回傳原本的 `o`，等於這一行被丟掉。
  - 若本次輸出會讓 `o + n >= cap`（被截斷），回傳 `cap - 1`，也就是把偏移固定在最後一個位元組之前（該位置已由 `vsnprintf` 寫入結尾 `'\0'`）。之後再呼叫時 `o = cap-1 < cap`，`vsnprintf` 只剩 1 byte 可寫，只會再寫一個 `'\0'`，回傳值仍是 `cap - 1`，所以不會前進也不會破壞已寫入的內容。
  - 因此回傳值最大是 `cap - 1`，`sysinfo_build()` 最終回傳的長度不含結尾的 `'\0'`；被截斷時最後一行可能不完整（沒有 `\n`）。實務上輸出約一千位元組，而呼叫端的緩衝區是 `static char info[8192]`（`server/session.c:381`，函式內的 static 變數，不在堆疊上），不會觸發截斷。
  - 不受任何 `--unsafe` 開關影響。
- **關係**：被誰呼叫 → `sysinfo_build()`（本檔，共 30 處）；呼叫 → libc 的 `vsnprintf`。

### `read_first_line`
*位置：`server/sysinfo.c:30`　簽章：`static int read_first_line(const char *path, char *out, size_t n)`*

- **用途**：開啟 `path`，讀第一行到 `out`（最多 `n-1` 個字元），去掉行尾換行字元；成功回傳 0，開檔或讀取失敗回傳 -1。
- **設計**：`/proc/uptime` 與 `/proc/loadavg` 都是「一行文字」的檔案，所以只需要 `fgets` 一次。以 `fopen/fgets/fclose` 這種最簡單的方式實作，不必自己處理 `read()` 的部分讀取與 EINTR；檔案很小，一次 `fgets` 就能讀完。失敗時回傳 -1 而且**不保證 `out` 內容**（`fgets` 失敗時內容未定義），所以呼叫端必須先把 `out` 放好預設值，再在失敗時沿用預設值。
- **細節**：
  - `fgets` 的長度參數是 `(int)n`；`fgets` 最多讀 `n-1` 字元並自己補 `'\0'`。
  - 換行用 `out[strcspn(out, "\n")] = 0` 去除；如果該行沒有換行，`strcspn` 回傳字串長度，等於把結尾 `'\0'` 再寫一次，無害。
  - 沒有檢查 `fclose` 回傳值；這是唯讀檔案，沒有需要檢查的寫入錯誤。
  - 所有路徑都會 `fclose`，不會漏掉 FILE 資源（child 是長生命週期行程，每次 INFO 請求都會呼叫兩次）。
  - 不受 `--unsafe` 開關影響。
- **關係**：被誰呼叫 → `sysinfo_build()`（本檔 `:121` 讀 `/proc/uptime`、`:122` 讀 `/proc/loadavg`）；呼叫 → libc `fopen/fgets/fclose`。

### `meminfo`
*位置：`server/sysinfo.c:44`　簽章：`static long meminfo(const char *key)`*

- **用途**：在 `/proc/meminfo` 中找出名稱為 `key` 的那一行（例如 `MemTotal`、`MemAvailable`），回傳其數值（單位 kB）；找不到或無法開檔回傳 -1。
- **設計**：`/proc/meminfo` 每行格式是 `Name:   value kB`。函式逐行用 `strncmp(line, key, kl)` 比對前綴，**再要求下一個字元是 `':'`**，這樣查 `MemTotal` 時不會誤中 `MemTotalXXX` 這種以 key 為前綴的其他欄位。找到後 `sscanf(line + kl + 1, "%ld", &v)` 解析數字（`%ld` 會自動略過冒號後的空白），然後 `break` 提早結束。用 -1 當作「不存在」的哨兵值，因為真實的記憶體量不可能是負數；`sysinfo_build()` 直接把這個值印出來，client 看到 `-1` 就知道這台機器沒有該欄位（例如很舊的核心沒有 `MemAvailable`）。
- **細節**：
  - 每呼叫一次就重新開啟並掃描檔案；`sysinfo_build()` 呼叫兩次（`MemTotal`、`MemAvailable`），因為 INFO 是人工觸發的低頻請求，不值得做快取。
  - `line` 為 256 bytes 的堆疊緩衝區，足夠容納 `/proc/meminfo` 的行。
  - `sscanf` 的回傳值沒有檢查；若數字解析失敗，`v` 維持 -1。
  - 單位由 `/proc/meminfo` 決定（kB），輸出的 key 名稱 `host.mem_total_kb` / `host.mem_avail_kb` 對應這個單位。
  - 不受 `--unsafe` 開關影響。
- **關係**：被誰呼叫 → `sysinfo_build()`（本檔 `:147`、`:148`）；呼叫 → libc `fopen/fgets/sscanf/fclose`。

### `cpu_model`
*位置：`server/sysinfo.c:61`　簽章：`static void cpu_model(char *out, size_t n)`*

- **用途**：從 `/proc/cpuinfo` 取出 CPU 型號字串寫入 `out`；取不到時結果是 `"unknown"`。
- **設計**：不同架構的 `/proc/cpuinfo` 欄位名稱不同：x86 用 `model name`，部分 ARM／嵌入式核心用 `Hardware`（benchd 的目標是 PYNQ 這類 ARM 板子，所以兩種都要接受）。函式先 `bench_strlcpy(out, "unknown", n)` 寫入預設值，之後無論開檔失敗或找不到欄位都不必再處理錯誤路徑——呼叫端永遠拿到一個合法字串。逐行掃描，**第一個**符合的行就採用並 `break`；SMP 機器的 `/proc/cpuinfo` 會為每顆 CPU 重複相同欄位，只取第一個即可。
- **細節**：
  - 比對用 `strncmp(line, "model name", 10)` 與 `strncmp(line, "Hardware", 8)`；只比對前綴，之後用 `strchr(line, ':')` 找冒號，冒號後略過空白與 tab，再用 `c[strcspn(c, "\n")] = 0` 去掉行尾換行。若該行沒有冒號，這一行被忽略，繼續讀下一行。
  - 使用 `bench_strlcpy` 複製，所以會被截斷在 `n-1` 個字元；呼叫端傳入的是 `char cpu[128]`。
  - `line` 為 512 bytes；超過 511 字元的行會被 `fgets` 分成多次讀出，接續的片段若恰好以 `model name` 或 `Hardware` 開頭才會被誤判；一般 `/proc/cpuinfo` 的行遠短於此，不會發生。
  - 若同一台機器同時有 `model name` 與 `Hardware`（某些 ARM 核心兩者都有），則以檔案中先出現者為準。
  - 不受 `--unsafe` 開關影響。
- **關係**：被誰呼叫 → `sysinfo_build()`（本檔 `:134`）；呼叫 → `bench_strlcpy`（`lib/util.c`）、libc `fopen/fgets/strchr/fclose`。

### `sysinfo_build`
*位置：`server/sysinfo.c:84`　簽章：`size_t sysinfo_build(char *buf, size_t cap)`*

- **用途**：把整個 INFO 回覆組成 `key=value\n` 文字寫入 `buf`（容量 `cap`），回傳寫入的位元組數（不含結尾 `'\0'`）。
- **設計**：分三步。
  1. **鎖內快照**（`:86-115`）：用一個區域匿名 struct `st` 在 `shm_lock(g_shm)` 與 `shm_unlock(g_shm)` 之間把所有要報告的共用記憶體欄位複製出來（裝置狀態與路徑、baud、rx/tx 位元組數、重連次數、最後收到資料的時間、控制權持有者與其最後輸入時間、idle 釋放秒數、viewer 數、max_clients、server 起始時間、ring 的 `cap/head/tail`、log 等級、`unsafe_mask`、鎖恢復次數、master pid）。這樣所有欄位來自同一個時間點，彼此一致（例如 `hist.used = head - tail` 不會因為兩次讀取之間 reader 又 append 而算出負值），而且鎖只被持有很短的時間：不做 `fopen`、`uname`、字串格式化或 log，避免 INFO 請求拖慢 reader 寫 ring 與其他 child（對應 `docs/DECISIONS.md`「鎖內不做 I/O」）。`dev_path`（128 bytes）、`ctrl_name`（`BN_NAME_LEN` = 32 bytes）是用 `memcpy` 整個陣列複製，之後在鎖外當成字串使用。viewer 數不是直接讀 `g_shm->viewers`，而是在鎖內呼叫 `slots_recount(g_shm)`（`:105`）重新數 slot 表中 `pid != 0 && joined` 的項目（`server/robust.c:70`；該函式同時會把結果寫回 `s->viewers`，所以這一步在鎖內對共用記憶體有寫入，呼叫端約定為「持鎖時呼叫」，見 `server/server.h:120`），因此 `server.clients` 是當下從 slot 表重算出來的值，而不是直接信任 `viewers` 欄位。
  2. **主機資訊**（`:117-138`）：`uname()`、`/proc/uptime`、`/proc/loadavg`、`cpu_model()`、`sysconf(_SC_NPROCESSORS_ONLN)`、`now_ns()`，以及 `bench_unsafe_format()` 把 `unsafe_mask` 轉成逗號分隔的名稱（沒有任何開關時為 `(none)`）。
  3. **依固定順序輸出**（`:140-173`）：連續呼叫 `add()` 輸出 key。鍵名採 `host.` / `server.` / `dev.` / `hist.` / `ctrl.` 前綴，client 端 `client/ui.c:313-314` 以這五個前綴分成 Host / Server / Device / History / Control 分組顯示；`scripts/stress.sh` 等腳本也用 `sed -n 's/^server.clients=//p'` 這類方式按 key 取值，所以 key 名稱是對外的介面。
- **細節**：
  - **輸出的 key**（依輸出順序）：`host.name`（`uname` 的 nodename）、`host.kernel`（`sysname release machine`）、`host.uptime_sec`（`%.0f`，由 `/proc/uptime` 第一個浮點數四捨五入）、`host.loadavg`、`host.cpu_model`、`host.cpu_count`、`host.mem_total_kb`、`host.mem_avail_kb`、`server.pid`（master pid）、`server.uptime_sec`、`server.version`（`BENCH_VERSION`，目前為 `"1.0.0"`）、`server.clients`、`server.max_clients`、`server.log_level`（以 `bench_log_level_name()` 轉成 ERROR/WARN/INFO/DEBUG/TRACE，超出範圍為 `?`）、`server.unsafe`、`server.lock_recoveries`、`dev.path`、`dev.state`、`dev.baud`、`dev.rx_bytes`、`dev.tx_bytes`、`dev.reconnects`、`dev.last_rx_ago_ms`、`hist.cap`、`hist.used`、`hist.head`、`ctrl.holder`、`ctrl.idle_sec`、`ctrl.idle_release_sec`。
  - `server.clients` 是「完成 handshake 的 client 數」，包含剛剛發出 INFO_REQ 的那一個（因為它自己已完成 handshake），見 `docs/DECISIONS.md`；`server.lock_recoveries` 是額外加的 key，用於 R7 示範。
  - `dev.state` 只有兩種輸出：`dev_state == DEV_UP` 印 `UP`，其他一律印 `LOST`。`dev.baud` 輸出格式是 `"%u 8N1"`（8N1 寫死在字串裡，reader 設定 termios 時固定為 8N1、無流量控制，見 `server/reader.c:70`；顯示值不隨實際 termios 變化）。
  - `dev.last_rx_ago_ms`：`last_rx_ns` 為 0（從未收過資料）輸出 `-1`；否則是 `(now - last_rx_ns) / 1000000`（毫秒）。此處**沒有**像 `ctrl.idle_sec` 那樣檢查 `now > last_rx_ns`，若系統時鐘（`now_ns()` 用 `CLOCK_REALTIME`）被往回調，無號減法會產生極大的數字。
  - `server.uptime_sec` 是 `(now - server_start_ns) / 1000000000`，同樣沒有檢查 `now >= start_ns`，時鐘往回調時無號減法會得到極大值。
  - `hist.used = head - tail`；`head`、`tail` 是只增不減的邏輯偏移，所以相減就是目前 ring 內有效資料量，不需處理環繞。`hist.cap` 是 ring 的位元組容量（`--history` 設定）。
  - `ctrl.holder`：`ctrl_pid` 為 0 時輸出 `(none)`，否則輸出 `ctrl_name`。`ctrl.idle_sec` 只有在有人持有控制權且 `now > ctrl_last` 時才計算 `(now - ctrl_last) / 1000000000ull` 秒（無號整數除法，向下取整），否則為 0。`ctrl.idle_release_sec` 是自動釋放閒置控制權的秒數設定；在 R6（`ctrl-release`）unsafe 模式下 idle 釋放被停用，但這個數字仍照設定值輸出（本函式不判斷 `UNSAFE()`）。
  - `/proc/loadavg` 內容如 `0.10 0.20 0.30 1/234 5678`；程式（`:126-133`）掃描字串，數到第 3 個空白就在該處截斷，只保留前三個平均值。讀取失敗時 `load` 維持預設的 `"?"`；`/proc/uptime` 讀取失敗時 `up` 維持 `"0"`，所以 `host.uptime_sec=0`。`cpu` 由 `cpu_model()` 保證有值。
  - `uname(&u)` 的回傳值沒有被檢查；若失敗，`u` 內容未定義，程式未做額外處理。
  - `server.unsafe` 顯示目前啟用的 `--unsafe=` 開關名稱（例如 `zombie,sigpipe`），這是 INFO 與 R1-R12 唯一直接的關聯；`sysinfo_build()` 自己不依任何開關改變行為，但它的第一步 `shm_lock()` 依賴 R7 的 robust mutex：安全模式下若先前持鎖的行程已死，`shm_lock()` 會得到 `EOWNERDEAD` 並修復共用狀態後繼續，所以 INFO 仍能回應；`--unsafe=robust-mutex` 時 mutex 不是 robust，持鎖者死亡後 `shm_lock()` 會永遠阻塞，這個 child 對 INFO_REQ 就不會回覆。
  - 回傳值是 `add()` 累計的偏移，若緩衝區不夠會被截斷在 `cap - 1`（見 `add`）；`session.c` 傳給它的 `cap` 是 `sizeof info`（= 8192，static 緩衝區），並以 `(uint32_t)n` 作為 `bn_send` 的 payload 長度，遠小於 `BN_MAX_PAYLOAD`（65536）。
  - 輸出以 `'\0'` 結尾（`vsnprintf` 保證），但 `bn_send` 只送 `n` 個位元組，不含結尾 `'\0'`。
- **關係**：被誰呼叫 → `server/session.c:382`（`BN_INFO_REQ` 分支，在 child 行程內，結果以 `BN_INFO` 回傳）；呼叫 → `shm_lock/shm_unlock/slots_recount`（`server/robust.c`）、`now_ns`、`bench_unsafe_format`（`lib/util.c`）、`bench_log_level_name`（`lib/log.c`）、本檔的 `add`、`read_first_line`、`meminfo`、`cpu_model`，以及 libc 的 `uname`、`sscanf`、`sysconf`。

---

## `client/main.c`

這是 `bench` 客戶端執行檔的進入點，由使用者在終端機（或 `scripts/` 內的腳本）直接啟動，屬於「bench client」行程，不是 `benchd` 的 master、reader 或 per-client child。檔案只做三件事：解析命令列選項、決定要跑哪個模式、把解析結果放進 `struct client_opts`（定義於 `client/client.h`）後交給後續模組。互動模式交給 `client/session.c` 的 `app_run()`，四個非互動子命令（`info`、`tail`、`watch`、`send`）交給 `client/subcmd.c` 的 `sub_info()`、`sub_tail()`、`sub_watch()`、`sub_send()`。它依賴 `libbench.so` 的 `parse_size()`、`parse_hostport()`、`sanitize_name()`、`bench_strlcpy()`、`bench_unsafe_parse()` 與日誌函式（`include/bench_util.h`、`include/bench_log.h`）。連線、協定、終端機處理都不在這個檔案裡。

### `usage`
*位置：`client/main.c:15`　簽章：`static void usage(FILE *f)`*

- **用途**：把 `bench` 的用法說明（四種呼叫形式、所有選項、`Ctrl-]` 熱鍵清單）印到呼叫端指定的 `FILE *`。
- **設計**：輸出串流由參數決定，是因為同一份文字有兩種用途。使用者要求說明（`-h`／`--help`）時是正常結果，應該寫到 stdout 並以 0 結束，方便 `bench -h | less`；選項錯誤或缺少位址時是錯誤訊息，應該寫到 stderr 並以 1 結束，不污染 stdout。整份說明是一個 `fprintf` 的字串常數，沒有任何格式轉換（`%`），所以不會有格式字串與參數不符的問題。
- **細節**：
  - 說明文字列出預設值：連接埠 9000（對應 `BN_DEFAULT_PORT`）、重播歷史 8K（對應 `main()` 內 `o->replay = 8 * 1024`）、名稱 `$USER@hostname`。
  - `--unsafe LIST` 的說明只列出用戶端認得的名稱：`term-restore`、`reconnect`、`readn`、`validate`、`all`。實際上 `bench_unsafe_parse()` 也接受 `sigpipe` 等其他名稱與 `R1`..`R12` 編號（見 `lib/util.c:259`），`main()` 也會用 `sigpipe`（R2）。這裡的清單只是說明文字，不是解析器的限制。
  - 提到 `bench tail` 的預設重播量時文字只寫 `[-n 16K]`，這是 `sub_tail()` 在未指定 `-n` 時使用的值（`client/subcmd.c:137`），不是 `main()` 的預設值 8K；`watch` 未指定 `-n` 時為 0（`client/subcmd.c:162`）。
  - 函式沒有回傳值，也不結束行程；結束行程由 `main()` 負責。
- **關係**：被誰呼叫 → `main()`（`client/main.c`，三處：`-h` 時傳 `stdout`；未知選項、缺少位址時傳 `stderr`）；呼叫 → 只有 libc 的 `fprintf`。

### `main`
*位置：`client/main.c:33`　簽章：`int main(int argc, char **argv)`*

- **用途**：解析選項與子命令，建立 `struct client_opts`，設定 SIGPIPE 與日誌，然後依模式回傳 `sub_*()` 或 `app_run()` 的結果當作行程結束碼。
- **設計**：
  - **選項表與 `getopt_long`**：短選項只有 `n:vh`。長選項 `--name`、`--no-reconnect`、`--unsafe`、`--duration`、`--out`、`--wait`、`--raw` 沒有對應的短字母，所以 `val` 欄位用 1 到 7 的小整數當私有代碼，`--help` 對應到 `'h'`，讓 `-h` 與 `--help` 共用同一個 `case`。GNU 的 `getopt_long` 會把非選項參數往後排（permutation），因此選項可以寫在主機位址之前或之後（例如 `bench send host "reset\n" --wait 500`），迴圈結束後 `optind` 指向第一個非選項參數。
  - **`struct app` 放在堆積並以 `calloc` 清零**：`struct app` 內含 `client_opts`、連線狀態、多個緩衝區（`cmd[128]`、`flash[200]`、`ibuf[64]` 等，定義於 `client/client.h`），清零後所有旗標與計數器都是 0，不需要逐欄初始化。選項直接寫進 `a->o`，因此互動模式可以直接把 `a` 交給 `app_run()`，子命令則只取 `&a->o`。程式從不 `free(a)`，因為行程在 `main()` 回傳時就結束。
  - **預設值**：`reconnect = 1`（預設會重連）、`replay = 8 * 1024`（加入時重播 8 KiB）、`replay_set = 0`。`replay_set` 的存在是為了讓子命令區分「使用者沒給 `-n`」與「使用者明確給了 `-n`」：`sub_tail()` 與 `sub_watch()` 各自有不同的預設值（`client/subcmd.c:137`、`:162`），只在 `replay_set` 為真時才採用使用者的值，否則用自己的預設值，而互動模式一律使用 `o->replay`（`client/session.c:172`）。
  - **`--unsafe` 先累積到區域變數，迴圈後才一次寫入全域**：`unsafe` 是區域的 `uint32_t`，`bench_unsafe_parse()` 以 OR 累加，所以可以重複給多次 `--unsafe`。選項全部解析成功後才執行 `bench_unsafe_mask = unsafe`（第 73 行），之後 `UNSAFE(r)` 巨集才在整個行程（包含 `libbench.so` 內的 `bn_recv` 等函式）生效。這與 `server/main.c:329` 的做法相同，只是客戶端沒有 `fork()` 需要繼承。
  - **R2 SIGPIPE**：預設 `signal(SIGPIPE, SIG_IGN)`，使得對已關閉的對端 `write()` 回傳 `EPIPE`，由 `conn_send()`（轉呼叫 `bn_send()`，見 `lib/proto.c:46`；實際寫出由 `lib/net.c:43` 的 `writen()` 內的 `write()` 完成）的呼叫端以回傳值判斷錯誤；`client/`、`lib/`、`server/` 都沒有使用 `MSG_NOSIGNAL`（只有 `tools/bench-evil.c` 用），所以這個行程層級的設定是客戶端唯一的保護。`--unsafe=sigpipe` 時明確設為 `SIG_DFL`，而不是什麼都不做：被忽略（`SIG_IGN`）的處置會跨 `exec` 繼承，若啟動 `bench` 的 Python 或其他啟動器本來就忽略 SIGPIPE，什麼都不做會讓 unsafe 示範悄悄變成 safe 行為（理由與 `docs/DECISIONS.md` 中伺服器端 R2 的說明相同）。
  - **子命令判定**：只有在第一個非選項參數剛好是 `info`、`tail`、`watch`、`send` 其中之一才當子命令，並把 `ai` 往後推進；否則它被當成主機位址。因此主機若真的叫 `info`（無連接埠）會被誤判，這是用法上的取捨：以字面相符換取不需要額外的 `--` 旗標。
  - **名稱預設值與消毒**：沒有給 `--name` 時用 `$USER@hostname` 組成，兩邊各截 15 字元（`%.15s@%.15s`），最長 31 字元，剛好放得進 `BN_NAME_LEN`（32，含結尾 NUL）。`gethostname` 失敗時保留初值 `"host"`；`$USER` 未設定時用 `"user"`。不論名稱來源是使用者或預設值，一律再經過 `sanitize_name()`，把不可列印字元換成 `?`、去除前後空白、空字串改成 `anon`，和伺服器收到 HELLO 後做的同一個消毒（`server/session.c:229`）保持一致，避免不同字元集在日誌或狀態列出現問題。
  - **日誌依模式分流**：子命令保留預設的 stderr 日誌（`bench_log_set_role("bench")` 設角色，`bench_log_init_level(o->verbose)` 設等級）。互動模式一旦進入原始終端機模式，任何寫到 stderr 的日誌都會破壞畫面，所以改開 `/tmp/bench-<pid>.log` 檔案並關掉 stderr 輸出。用 pid 命名讓多個 `bench` 同時執行時日誌不互相覆蓋。這段日誌設定放在解析位址之後、`app_run()` 之前。
- **細節**：
  - **回傳值**：`-h`／`--help` 回 0；未知選項、`--unsafe` 名稱不明、`-n` 值不合法、沒有位址、位址不合法、`send` 缺文字都回 1。其他情況回傳 `sub_*()` 或 `app_run()` 的值。`sub_send()` 在拒絕時回 2（`client/subcmd.c:280`，與 `usage()` 內「exit 2 if refused」說明一致），`sub_info()` 等失敗時回 1。
  - **`-n SIZE`**：用 `parse_size()` 解析（支援 `B`、`K`、`M`、`G` 與 `KiB`、`KB` 等寫法，見 `lib/util.c:57`），結果必須 `<= 0xFFFFFFFF`，因為 HELLO 封包的 `replay_bytes` 是 32 位元欄位（`client/session.c:37` 的 `htobe32`），超過的值會被截斷，所以在這裡先拒絕。`0` 是合法值，表示不重播。成功後才設 `replay_set = 1`。
  - **`--duration`** 用 `atof`，**`--wait`** 用 `strtoul(…, NULL, 10)`，兩者都不檢查錯誤：非數字會變成 0（`--wait` 的負數字串經 `strtoul` 會繞回成很大的值）。`duration <= 0` 在 `sub_watch()` 中等同於不限時（`client/subcmd.c:174`），`wait_ms == 0` 在 `sub_send()` 中表示不等待回應（`client/subcmd.c:288`）。
  - **`--name`** 用 `bench_strlcpy` 複製進 `o->name`（32 位元組），過長的名稱被截斷，不會溢位。`--out` 只存指標 `optarg`（指向 `argv` 內的字串），不複製，因為 `argv` 在行程存活期間一直有效。
  - **`-v`** 每出現一次 `verbose` 加 1，由 `bench_log_init_level()` 疊加在預設 INFO（或環境變數 `BENCH_LOG`）之上。
  - **位址**：`parse_hostport()` 支援 `host`、`host:port`、`[ipv6]:port`，失敗條件包含空主機、連接埠不在 1..65535、`[` 缺 `]`；未指定連接埠時用 `BN_DEFAULT_PORT`（9000）。主機緩衝區是 `o->host[256]`。位址後多餘的非選項參數，除了 `send` 的第一個文字參數外，都會被忽略。
  - **`send` 的文字參數**：取位址之後的 `argv[ai]`，只用第一個；跳脫序列（`\n`、`\r`、`\xHH` 等）由 `sub_send()` 呼叫 `unescape()`（定義於 `lib/util.c:119`）處理，不是 `main()`。
  - 日誌檔 `bench_log_open_file()` 的回傳值沒有檢查：開檔失敗時互動模式仍會繼續，只是沒有日誌（stderr 輸出也已關閉）。
  - 啟動日誌 `LOGI("bench %s starting, …")` 使用 `BENCH_VERSION`（`"1.0.0"`），只在互動模式寫入。
  - **`--unsafe` 在客戶端的效果**：`main()` 自己只用到 `sigpipe`（R2）。其餘由別處檢查 `UNSAFE()`：`term-restore`（R11，`client/term.c:136`）、`reconnect`（R12，`client/session.c:133`）、`readn` 與 `validate`（R4、R3，`lib/proto.c`）。伺服器專屬的名稱（如 `zombie`）在客戶端會被接受但沒有作用，這與 `docs/DECISIONS.md`「The server ignores client-only names and vice versa」一致。
- **關係**：被誰呼叫 → C 執行環境（行程進入點，`build/bin/bench`）；呼叫 → `usage()`、`getopt_long`、`bench_strlcpy`／`parse_size`／`bench_unsafe_parse`／`parse_hostport`／`sanitize_name`（`lib/util.c`）、`bench_log_set_role`／`bench_log_init_level`／`bench_log_open_file`／`bench_log_set_stderr`（`lib/log.c`）、`sub_info`／`sub_tail`／`sub_watch`／`sub_send`（`client/subcmd.c`）、`app_run`（`client/session.c:332`）。

---

## `client/session.c`

這個檔案是 `bench` 用戶端（bench client）的連線層與互動主迴圈，只在使用者執行的 `bench` 行程中運作（沒有 master、reader 或 per-client child，也沒有額外執行緒）。前半段（`conn_*`）是「連線層」：建立 TCP、送 HELLO、收封包、等 WELCOME、優雅關閉，互動模式與非互動子命令（`client/subcmd.c` 的 `info` / `tail` / `watch` / `send`）共用。後半段（`app_*`、`handle_*`、`app_run`）只屬於互動模式：單一 `poll()` 迴圈同時等 stdin、socket 與計時器，處理封包分派、輸入批次送出、PING 與 15 秒接收逾時、指數退避重連（R12），以及每秒重畫狀態列。本檔依賴 `include/bench_proto.h`（`bn_send` / `bn_recv` 與各 payload 結構）、`include/bench_net.h`（`tcp_connect_timeout`）、`include/bench_util.h`（`mono_ms`、`now_ns`、`bench_strlcpy`、`UNSAFE()`），以及 `client/term.c`、`client/ui.c`、`client/cmd.c` 提供的終端、繪圖與按鍵處理。

### `conn_connect`
*位置：`client/session.c:29`　簽章：`int conn_connect(struct conn *c, const struct client_opts *o, uint32_t replay, int timeout_ms)`*

- **用途**：連到 `o->host:o->port` 並送出 HELLO（含暱稱、要回放的位元組數，以及重連時的續傳資訊）。成功回傳 0，失敗回傳 -1。
- **設計**：把「TCP 連線 + HELLO」包成一個函式，讓互動模式與子命令走同一條路徑。續傳資訊由 `struct conn` 自己攜帶：只有 `c->have_pos`（已收過至少一筆帶 offset 的封包）與 `c->have_start`（已看過 WELCOME 的 `server_start_ns`）同時成立，才填 `has_resume = 1`、`resume_offset = c->pos`、`last_server_start_ns`。server 端用這兩個值判斷能否從上次位置接續，還是 server 已重啟、必須重新回放（判斷在 `server/session.c:266`）。兩者缺一就不續傳，因為只有 offset 而不知道 server 世代，offset 沒有意義。
- **細節**：
  - `tcp_connect_timeout()` 失敗時直接回 -1，`errno` 保留該函式設定的值（逾時為 `ETIMEDOUT`，`getaddrinfo` 失敗為 `EHOSTUNREACH`，其餘為 `connect` 的錯誤）；`try_connect()` 緊接著讀 `errno` 來顯示原因。`timeout_ms` 只限制非阻塞 `connect()` 的等待，DNS 解析本身不受它限制。
  - `struct bn_hello` 先 `memset` 為 0，名稱用 `bench_strlcpy` 複製進 `BN_NAME_LEN`（32）位元組的欄位，過長會被截斷；數值欄位一律轉大端（`htobe32` / `htobe64`）。
  - `c->last_rx_ms` 與 `c->last_ping_ms` 在送 HELLO 之前就設為目前的 `mono_ms()`，所以 `app_run()` 的 15 秒逾時從「連線建立」起算，握手階段 server 不回 WELCOME 也會被判逾時。
  - HELLO 送失敗時呼叫 `conn_close()` 後回 -1。
  - 呼叫端傳入的 timeout：互動模式 2000 ms（`try_connect`），子命令 3000 ms（`client/subcmd.c:35`）。
  - 子命令的 `struct conn` 先 `memset` 為 0，所以不會帶續傳資訊；互動模式的 `struct app` 由 `calloc` 配置，第一次連線同樣沒有續傳資訊。
- **關係**：被誰呼叫 → `try_connect`（本檔）、`client/subcmd.c` 的 `open_session`；呼叫 → `tcp_connect_timeout`（`lib/net.c`）、`bn_send`（`lib/proto.c`）、`conn_close`、`mono_ms`、`bench_strlcpy`。

### `conn_recv`
*位置：`client/session.c:53`　簽章：`int conn_recv(struct conn *c, struct bn_hdr *h, uint8_t *buf, size_t cap, int *perr)`*

- **用途**：收一個完整封包，並在成功時記錄「最後一次收到資料的時間」。回傳值與 `bn_recv` 相同：1 成功、0 對端正常關閉（EOF）、-1 I/O 錯誤（`errno`）、-2 協定錯誤（`*perr` 為 `BN_ERR_BAD_*`）。
- **設計**：`bn_recv` 屬於共用函式庫，不認識 `struct conn`；所以在用戶端這一層加上 `last_rx_ms` 的更新，讓 15 秒接收逾時的判斷只依賴「真的收到完整封包」，而不是「socket 可讀」。只在 `rc == 1` 時更新：EOF 或壞封包不應該讓連線看起來還活著。
- **細節**：
  - `--unsafe=readn` 時 `bn_recv` 只用一次 `recv()` 讀標頭，`--unsafe=validate` 時不檢查魔數／版本／長度／CRC，這兩個開關的效果都在 `bn_recv` 內，本函式不處理。
  - `buf` 容量 `cap` 由呼叫端決定；本檔都傳 `BN_MAX_PAYLOAD`（65536）的 static 緩衝區。
- **關係**：被誰呼叫 → `conn_wait_welcome`、`app_run`（本檔）、`client/subcmd.c` 的 `recv_timeout`；呼叫 → `bn_recv`（`lib/proto.c`）、`mono_ms`。

### `conn_send`
*位置：`client/session.c:61`　簽章：`int conn_send(struct conn *c, uint8_t type, const void *p, uint32_t len)`*

- **用途**：在這條連線上送一個封包，回傳 0 或 -1（`errno`）。
- **設計**：只是 `bn_send(c->fd, ...)` 的薄包裝，讓用戶端程式碼一律透過 `struct conn` 操作而不直接碰 fd。`bn_send` 把標頭與 payload 複製進同一個 static 緩衝區，再以 `writen` 一併寫出（`lib/proto.c:46-57`）。
- **細節**：
  - 不更新任何計時欄位；`last_ping_ms` 由 `app_run` 在送 PING 時自己更新。
  - 對端已關閉時，預設（`main.c:74-75` 忽略 SIGPIPE）回 -1 並設 `EPIPE`；若使用 `--unsafe=sigpipe`，`main.c:77` 把 SIGPIPE 改回預設動作，這裡的寫入可能直接殺掉 `bench`（R2 的用戶端版本）。
- **關係**：被誰呼叫 → `app_send`（本檔）、`client/subcmd.c`（`BN_INFO_REQ`、`BN_PING`、`BN_TAKE`、`BN_INPUT`、`BN_RELEASE`）；呼叫 → `bn_send`。

### `conn_bye`
*位置：`client/session.c:68`　簽章：`void conn_bye(struct conn *c)`*

- **用途**：優雅地結束連線：送 BYE、關閉寫端（`shutdown(SHUT_WR)`）、把對端還在送的資料讀掉直到 EOF，最後 `conn_close()`。
- **設計**：註解已說明原因——關閉一個接收佇列裡還有未讀資料的 socket 時，核心會送 RST 而不是 FIN。server 在收到 BYE 之前可能還有 DATA／STATUS 在路上，直接 `close()` 會讓對端看到 `ECONNRESET`、也讓 `captures/rst.pcap` 的對照案例出現。先 half-close 再排空，才會是乾淨的 FIN 交換（DECISIONS.md「Client」一節）。
- **細節**：
  - `c->fd < 0` 時直接返回（已關閉過，可重複呼叫）。
  - 排空迴圈有 1000 ms 的總期限（`end = mono_ms() + 1000`）。每圈 `poll(..., 100)`；`poll` 回傳 0 或 -1（逾時或 `EINTR`）就 `continue` 再試，直到期限到。`poll` 回傳可讀後 `read()` 回 0（EOF）或 -1 就 `break`；讀到的內容丟棄（`tmp[4096]`）。
  - 因此最壞情況會多等約 1 秒（server 一直不關閉時），不會無限阻塞。
  - `bn_send(BN_BYE)` 的回傳值被忽略：連線已壞時，後面的 `shutdown`／`read` 會很快失敗並結束。
- **關係**：被誰呼叫 → `app_run`（本檔，結束時）、`client/subcmd.c` 的 `sub_info`、`sub_tail`、`sub_watch`、`sub_send`；呼叫 → `bn_send`、`conn_close`、`mono_ms`。

### `conn_close`
*位置：`client/session.c:86`　簽章：`void conn_close(struct conn *c)`*

- **用途**：關閉 fd 並把 `c->fd` 設成 -1。
- **設計**：把 fd 設為 -1 讓函式可重複呼叫（idempotent），也讓 `app_run` 之後以 `c->fd` 是否為負值判斷連線狀態、`poll()` 不會誤用舊的 fd。
- **細節**：不送 BYE、不排空資料，因此用於連線已壞或握手失敗的情況；要優雅結束請用 `conn_bye`。不修改 `pos`、`server_start_ns`、`have_*`，這些續傳資訊刻意保留，供下次 `conn_connect` 使用。
- **關係**：被誰呼叫 → `conn_connect`（HELLO 失敗）、`conn_bye`、`app_disconnect`（本檔）、`client/subcmd.c` 的 `open_session` 與 `sub_info`。

### `conn_wait_welcome`
*位置：`client/session.c:93`　簽章：`int conn_wait_welcome(struct conn *c, struct bn_welcome *w, int timeout_ms)`*

- **用途**：同步等待 server 回的第一個封包，要求它是 WELCOME，並把內容複製到 `*w`。回傳 0 成功；-1 逾時／連線錯誤／封包不是 WELCOME；-2 server 回了 `BN_ERROR`（已把錯誤印到 stderr）。
- **設計**：給非互動子命令用，它們需要「連上 → 取得 WELCOME → 做事」的線性流程。先 `poll(timeout_ms)` 再 `conn_recv`，所以逾時由 poll 控制；`bn_recv` 本身阻塞，若先不 poll，沒有回應的 server 會讓 `bench` 永遠卡住。互動模式不使用它，而是把 WELCOME 當一般封包交給 `handle_welcome`，這樣重連與逾時都能在同一個事件迴圈處理。
- **細節**：
  - 緩衝區是 `static uint8_t buf[BN_MAX_PAYLOAD]`（64 KiB），避免佔用堆疊；因此函式不可重入（本程式沒有執行緒，沒有影響）。
  - `BN_ERROR`（例如 R9 的 `SERVER_BUSY`）：印出 `bench: server error <名稱>: <訊息>`，訊息用 `%.*s` 搭配長度印出，因為 payload 沒有 NUL 結尾；同時設 `errno = ECONNREFUSED` 並回 -2，讓呼叫端知道錯誤已經回報、不要再補印「no WELCOME」（見 `client/subcmd.c:41-42`）。
  - 第一個封包若不是 WELCOME 或長度小於 `struct bn_welcome`，一律回 -1（這裡不接受別的封包順序）。
  - 成功時寫入 `c->server_start_ns` 並設 `c->have_start = 1`，與 `handle_welcome` 的行為一致；不設 `have_pos`（該值由後續 DATA／REPLAY 封包設定）。
  - 呼叫端使用 5000 ms（`client/subcmd.c:39`）。
- **關係**：被誰呼叫 → `client/subcmd.c` 的 `open_session`；呼叫 → `conn_recv`、`bn_err_name`（`lib/proto.c`）。

### `on_winch`
*位置：`client/session.c:123`　簽章：`static void on_winch(int sig)`*

- **用途**：SIGWINCH（終端機視窗大小改變）的處理函式，只把旗標 `got_winch` 設為 1。
- **設計**：訊號處理函式只做 async-signal-safe 的事——寫一個 `volatile sig_atomic_t`。真正的重畫（`ui_resize`）會呼叫 `ioctl`、`ob_printf` 等非 async-signal-safe 的函式，所以交給主迴圈在 `poll` 返回後處理（`app_run` 的 `if (got_winch)`）。
- **細節**：`sigaction` 以 `SA_RESTART` 安裝（見 `app_run`）；但 Linux 上 `poll()` 不受 `SA_RESTART` 影響，仍會以 `EINTR` 返回，所以視窗改變會立刻喚醒迴圈，不必等 250 ms 的逾時。
- **關係**：被誰呼叫 → 核心在 SIGWINCH 時呼叫（由 `app_run` 以 `sigaction` 註冊）；呼叫 → 無。

### `app_disconnect`
*位置：`client/session.c:125`　簽章：`static void app_disconnect(struct app *a, const char *why)`*

- **用途**：處理「連線結束」的所有情況：關閉 socket、重設狀態，然後依設定決定是放棄（`quit = 2`）還是排定下一次重連時間。
- **設計**：所有失敗路徑（讀到 EOF、協定錯誤、15 秒逾時、寫入失敗、連線失敗）都匯集到這個函式，退避與狀態重設只寫一份。
  - 狀態重設：`conn_close`、`st = ST_DISCONNECTED`、`is_ctrl = 0`（控制權在 server 端已隨連線消失或將被釋放，不能再自認為控制者）、`ilen = 0`（丟掉還沒送出的按鍵，避免重連後把舊鍵送給新的控制者）。
  - 退避：`backoff_s[] = {1, 2, 4, 8, 16, 30}`；查表索引以 `backoff_idx < 5 ? backoff_idx : 5` 夾住，所以序列為 1、2、4、8、16、30、30…秒。`backoff_idx` 本身增加到 100 為止，防止無限增長溢位。`retry_at = mono_ms() + d * 1000ull`。成功收到 WELCOME 時 `handle_welcome` 才把 `backoff_idx` 歸零。
  - 只有 `was_live`（斷線前是 `ST_LIVE`）才呼叫 `ui_flash` 顯示黃色「connection lost … reconnecting」，避免連續重試失敗時狀態列一直閃；之後 `ui_status` 在 `ST_DISCONNECTED` 且曾握手成功時顯示「RECONNECTING (Ns)」倒數；從未成功握手過則只顯示「CONNECTING」（`client/ui.c:402-407`）。
- **細節**：
  - `--no-reconnect`（`a->o.reconnect == 0`）或 `--unsafe=reconnect`（`UNSAFE(R_RECONNECT)`，R12）：設 `quit = 2`、用 `ui_note` 印出「connection lost: …」就返回，`app_run` 結束時回傳 1。這就是 R12 不安全版本的「client exits」。
  - 對 R12 而言，安全版本與不安全版本的差別只在這個判斷；續傳（`resume_offset`）的資訊由 `conn_connect` 與 `handle_welcome` 管理。
  - `LOGI("disconnected: ...")` 在判斷之前記錄，不論是否重連都會記。
- **關係**：被誰呼叫 → `app_send`、`try_connect`、`app_run`（本檔）；呼叫 → `conn_close`、`ui_note`、`ui_flash`、`ui_status`（`client/ui.c`）、`mono_ms`。

### `app_send`
*位置：`client/session.c:148`　簽章：`int app_send(struct app *a, uint8_t type, const void *p, uint32_t len)`*

- **用途**：互動模式送封包的統一出口。成功回 0；已斷線回 -1（不產生任何動作）；寫入失敗時回 -1，並觸發 `app_disconnect`。
- **設計**：寫入失敗（例如 `EPIPE`、`ECONNRESET`）時立刻進入斷線／重連流程，呼叫端（`cmd.c`）不需要自己處理錯誤。`ST_DISCONNECTED` 時直接拒絕，因為此時 `a->c.fd` 為 -1。
- **細節**：
  - 狀態檢查只排除 `ST_DISCONNECTED`，`ST_HANDSHAKE` 仍可送；是否要求 `ST_LIVE` 由呼叫端決定（`cmd.c` 的 `send_simple` 與 `:baud` 自己檢查，`app_flush_input` 也檢查）。
  - 斷線原因字串直接使用 `strerror(errno)`，讀取時機緊接在 `conn_send` 之後。
- **關係**：被誰呼叫 → `app_flush_input`、`app_run`（PING）（本檔）、`client/cmd.c`（`send_simple`、`:baud`）；呼叫 → `conn_send`、`app_disconnect`。

### `app_flush_input`
*位置：`client/session.c:159`　簽章：`void app_flush_input(struct app *a)`*

- **用途**：把累積在 `a->ibuf` 的控制者按鍵以一個 `BN_INPUT` 封包送出，然後清空緩衝區。
- **設計**：`cmd.c` 為了不要每個按鍵都送一個封包，會把按鍵暫存在 `ibuf[64]`，第一個鍵進來時設 `iflush_at = now + 5 ms`，或緩衝滿 64 位元組時立刻呼叫本函式；`app_run` 也在 `now >= iflush_at` 時呼叫它。結果是 5 ms／64 位元組內的按鍵合併成一個 TCP 封包，貼上大段文字時仍然能維持順序且量小（64 位元組遠低於 `BN_MAX_INPUT = 256`）。
- **細節**：
  - 緩衝區為空直接返回。
  - 只有 `ST_LIVE` 才真的送；其他狀態下資料被丟棄（`ilen` 仍歸零），因為尚未握手完成時 server 不會接受 INPUT。
  - 送出失敗由 `app_send` 處理（觸發斷線）。
  - `:release` 與 `Ctrl-] r` 會先呼叫本函式再送 RELEASE，確保已打的字先於釋放送出（與 DECISIONS.md 中 server 端 INPUT 先於 RELEASE 的順序保證相呼應）；結束時 `app_run` 也先呼叫它再 `conn_bye`。
- **關係**：被誰呼叫 → `client/cmd.c`（兩處）、`app_run`（本檔，兩處）；呼叫 → `app_send`。

### `try_connect`
*位置：`client/session.c:168`　簽章：`static void try_connect(struct app *a)`*

- **用途**：在 `ST_DISCONNECTED` 且重試時間已到時，嘗試建立連線並送出 HELLO，成功就把狀態切到 `ST_HANDSHAKE`。
- **設計**：
  - 連線前先 `ui_status` + `ob_flush`，讓狀態列在阻塞的 `conn_connect`（最多 2000 ms，期間 UI 不會更新）之前就顯示 CONNECTING／RECONNECTING。
  - 回放大小用 `a->o.replay`（預設 8 KiB），續傳資訊由 `a->c` 內的 `pos` 與 `server_start_ns` 提供，因此第一次連線與重連使用同一段程式碼。
  - 失敗時分兩種：從未成功握手過（`!ever_welcomed`）且使用者指定 `--no-reconnect`，視為啟動失敗，印出 `bench: cannot connect to host:port: 原因` 到 stderr 並設 `quit = 2`；其餘情況交給 `app_disconnect` 排程退避。DECISIONS.md 也記載「第一次成功前同樣適用退避，顯示 CONNECTING」。
- **細節**：
  - `errno` 在 `conn_connect` 失敗後立刻存入 `e`，避免被後續 stdio／日誌呼叫改寫。
  - 失敗分支中先把 `a->st` 設成 `ST_HANDSHAKE` 再呼叫 `app_disconnect`；註解說這是為了讓退避被排程，但從程式碼看，`app_disconnect` 只用 `st` 計算 `was_live`（此時為假），隨後就把 `st` 設為 `ST_DISCONNECTED`，所以這一行對結果沒有影響。
  - 在 `--unsafe=reconnect` 下，若連線失敗，`app_disconnect` 會直接設 `quit = 2`。
  - 成功只代表 TCP 已連通且 HELLO 已送出，尚未收到 WELCOME；真正進入 `ST_LIVE` 在 `handle_welcome`。
- **關係**：被誰呼叫 → `app_run`；呼叫 → `ui_status`、`ob_flush`（`client/term.c`）、`conn_connect`、`app_disconnect`。

### `handle_welcome`
*位置：`client/session.c:186`　簽章：`static void handle_welcome(struct app *a, const uint8_t *buf, uint32_t len)`*

- **用途**：處理 WELCOME：判斷是首次連線、單純重連，還是 server 已重啟，更新本地狀態並切到 `ST_LIVE`。
- **設計**：以 `server_start_ns` 區分 server 世代。`restarted = ever_welcomed && have_start && start != 先前的 server_start_ns`。
  - 重連（同一個 server 世代）：顯示「reconnected」分隔線；續傳位置 `c.pos` 保留，server 會從該 offset 接續（若 offset 已被覆寫則送 DROPPED 事件）。
  - server 重啟：顯示「server restarted」，並把 `have_pos` 清 0，舊世代的 offset 在新 server 上沒有意義。實際上 HELLO 已經送出，server 已判斷世代不同並重新回放，後續 `REPLAY_BEGIN` 會重新設定 `c.pos`（見 `server/session.c:266-314`）。
  - 首次連線不顯示分隔線（`ever_welcomed == 0`）。
- **細節**：
  - 長度不足 `sizeof(struct bn_welcome)` 時直接返回，狀態維持 `ST_HANDSHAKE`。
  - 複製到本地：`dev_state`、`baud`（`be32toh`）、`viewers`、`dev_path`、`ctrl_name`（`bench_strlcpy`；它以 `strlen` 取長度，假設 server 填的字串有 NUL 結尾）。
  - `backoff_idx = 0`（重連成功後退避重新計算）、`ever_welcomed = 1`、`st = ST_LIVE`。
  - 沒有重設 `in_replay`；該欄位在 client 內只被寫入（`REPLAY_BEGIN` 設 1、`REPLAY_END` 設 0），沒有地方讀取。
  - 記錄 `LOGI("welcome: ...")`，內含 `server_version`。
- **關係**：被誰呼叫 → `handle_packet`（`BN_WELCOME`）；呼叫 → `ui_separator`（`client/ui.c`）、`bench_strlcpy`。

### `handle_packet`
*位置：`client/session.c:211`　簽章：`static void handle_packet(struct app *a, const struct bn_hdr *h, const uint8_t *buf)`*

- **用途**：依封包型別分派 server 傳來的封包，更新 `struct app` 與畫面。
- **設計**：一個 `switch`，每一種封包做最少的事：把大端欄位轉成主機序、更新狀態、呼叫 `ui_*` 繪圖。需要解析結構的 payload 先檢查長度（`h->len` 不足就 `break`；WELCOME 在 `handle_welcome` 內檢查），再 `memcpy` 到區域結構，避免對 `buf` 做未對齊的指標轉型（這些結構是 packed）。DATA 與 EVENT 都以 server 傳來的「記錄結束 offset」更新 `c.pos` 與 `have_pos`（DECISIONS.md：offset 是該記錄結尾），這是重連時 HELLO 帶的 `resume_offset`。各型別：
  - `BN_WELCOME`：交給 `handle_welcome`。
  - `BN_REPLAY_BEGIN`：`in_replay = 1`；若長度足夠，`c.pos = from_offset`（回放起點）、`have_pos = 1`。
  - `BN_REPLAY_END`：`in_replay = 0`；`c.pos = head`；畫一條「replay end · live」分隔線，使用者能分辨歷史與即時資料。
  - `BN_DATA`：更新 `c.pos`；`ui_data(...)` 以 server 時間戳 `ts_ns` 繪製（時間戳模式 `:ts` 使用）；若有開啟錄製（`a->logf`），把原始位元組（未經 ANSI 過濾）`fwrite` 到檔案。
  - `BN_EVENT`：更新 `c.pos`；文字最多保留 299 位元組（`text[300]`，超過就截斷並補 NUL，因為 payload 沒有 NUL 結尾）；`ui_event` 繪出事件；`EV_DEVICE_LOST` 設 `dev_state = DEV_LOST`，`EV_DEVICE_BACK` 設 `DEV_UP`，使狀態列立即反映（R8 的 DEVICE LOST 展示）。
  - `BN_CTRL_RESULT`：若 `granted`，`is_ctrl = 1`、記下持有者名稱並閃綠色提示；否則顯示紅色「held by X (idle Ns, auto-release in Ms)」。當 `auto_release_in_sec == UINT32_MAX` 時改顯示「no auto-release」，server 的 `ctrl_auto_release_in()` 在沒有控制者、`--unsafe=ctrl-release`（R6）或 `idle_release_sec` 為 0 時回報此值（`server/control.c:30-34`）。
  - `BN_INFO`：只有在使用者先要求過（`info_pending`）才呼叫 `ui_info_box`；不論如何都清掉旗標。未請求的 INFO 會被忽略。
  - `BN_STATUS`：整批覆寫 `dev_state`、`is_ctrl`、`baud`、`viewers`、`ctrl_idle`、`rx_bytes`、`ctrl_name`，再 `ui_status`。`is_ctrl` 以 server 為準，所以如果控制權在 server 端被釋放，狀態列會自動更正；`was_ctrl && !is_ctrl` 時清掉 `ilen`，避免把已失去控制權時打的字之後送出。
  - `BN_ERROR`：以 `ui_flash` 顯示 server 的文字；`BN_ERR_NOT_CONTROLLER` 用黃色警告，其他錯誤用紅色；並 `LOGW` 記錄錯誤名稱。
  - `BN_PONG`：不做事，因為 `conn_recv` 已更新 `last_rx_ms`，這就是 PONG 唯一的用途（保活）。
  - 其他型別：`LOGD` 記錄後忽略，讓協定日後增加型別時舊 client 不會壞掉。
- **細節**：
  - 沒有檢查目前的 `st`：任何狀態都處理所有型別。
  - `buf` 是 `app_run` 的 static 緩衝區，函式返回後內容即失效，所有需要保留的資料都已複製。
  - 本函式不會呼叫 `app_disconnect`，連線狀態不會因為處理一個封包而改變，因此 `app_run` 的批次迴圈在它之後不需要重新檢查 fd。
  - `BN_ERROR` 本身不會結束連線；若 server 因錯誤關閉連線（例如逾時），之後 `conn_recv` 回 0 才會觸發斷線。
- **關係**：被誰呼叫 → `app_run`；呼叫 → `handle_welcome`、`ui_data`、`ui_event`、`ui_separator`、`ui_flash`、`ui_info_box`、`ui_status`（皆在 `client/ui.c`）、`bn_err_name`、`bench_strlcpy`。

### `app_run`
*位置：`client/session.c:332`　簽章：`int app_run(struct app *a)`*

- **用途**：互動模式的入口與事件迴圈。初始化終端機與 UI，反覆處理 stdin、socket 與計時器，離開時優雅關閉並還原終端機。回傳程序結束碼：`quit == 2`（連線放棄）回 1，其他回 0；stdin 不是終端機時印訊息並回 1。
- **設計**：單行程單執行緒的 `poll()` 迴圈（專案不使用執行緒）。每圈依序：
  1. **重連**：`st == ST_DISCONNECTED` 且 `now >= retry_at` 時 `try_connect`（`retry_at` 初始為 0，所以第一圈就會連線）；`quit` 被設時立刻離開。
  2. **計算 poll 逾時**：預設 250 ms；有待送按鍵（`ilen`）時縮短到 `iflush_at`（5 ms 批次）；斷線時縮短到 `retry_at`，這樣退避時間到了就能準時重試而不是睡過頭。負值夾成 0。
  3. **等待事件**：`pollfd p[2]` = stdin 與 socket；斷線時只監看 stdin（`nfds = 1`），因為 fd 為 -1。`poll` 回 -1 且非 `EINTR` 才離開，`EINTR`（例如 SIGWINCH）只是進入下一步。
  4. **視窗改變**：`got_winch` 為真就清除並呼叫 `ui_resize`。
  5. **stdin**：可讀（或 HUP）時最多讀 256 位元組交給 `cmd_keys`；讀到 0 或錯誤（stdin 被關閉）就離開迴圈。
  6. **socket**：可讀／HUP／ERR 時，最多連續處理 256 個封包。每處理一個封包，再用 `poll(..., 0)` 檢查是否還有資料，沒有就跳出。這樣能一次消化大量 DATA（如回放）又不會永遠佔著迴圈而餓死 stdin 與計時器。`conn_recv` 回 1 就 `handle_packet`；否則呼叫 `app_disconnect`，原因字串分別為：0 → 「server closed the connection」，-2 → `bn_err_name(perr)`（協定錯誤），-1 → `strerror(errno)`。
  7. **計時器**：`now` 重新取得後，(a) `ilen` 且已到 `iflush_at` 就 `app_flush_input`；(b) 連線中（包含 `ST_HANDSHAKE`）若距上次收到封包超過 15000 ms 就以「no data from server for 15 s」斷線，否則若距上次 PING 已滿 15000 ms 就送 `BN_PING`（內含 `now_ns()` 時間戳，並先更新 `last_ping_ms`），server 的 PONG 會刷新 `last_rx_ms`；server 本身在 `pump` 中狀態改變或距上次 ≥ 1 秒時就會送 STATUS（`server/session.c:166-179`），所以正常連線不會因為安靜而觸發逾時，逾時代表對端真的沒有任何回應（例如網路中斷但 TCP 沒收到 RST）；(c) 狀態列每 1000 ms 重畫一次（閒置秒數、重連倒數等是隨時間變化的），或提示訊息（`flash`）到期時先清掉再重畫。
  8. **`ob_flush()`**：所有 UI 輸出累積在輸出緩衝區，每圈統一寫出一次，減少 `write()` 次數與畫面閃爍。
- **細節**：
  - 前置：`a->c.fd = -1`、`st = ST_DISCONNECTED`、`dev_state = DEV_UP`、`dev_path = "?"`；`term_init()` 失敗（stdin 不是 tty）時印出建議改用 `bench watch` 並回 1。`term_init` 內部依 `--unsafe=term-restore`（R11）決定是否安裝 `atexit` 與致命訊號還原處理（`client/term.c:136`）。
  - SIGWINCH 以 `SA_RESTART` 註冊 `on_winch`。
  - 離開後：若尚在連線（`st != ST_DISCONNECTED`），先 `app_flush_input`（`st` 非 `ST_LIVE` 時該函式只清空不送）再 `conn_bye`，保證 TCP 是 FIN 收尾而非 RST（DECISIONS.md「Client」）；關閉錄製檔 `logf`；最後呼叫 `term_restore()`（與 `atexit` 重複呼叫是安全的，因為只在需要時還原）。
  - 使用者 `q`（`quit = 1`）或 stdin EOF／poll 錯誤 → 結束碼 0；`--no-reconnect`、`--unsafe=reconnect` 或首次連線被拒且不重連 → `quit = 2` → 結束碼 1。
  - 因為 `bn_recv` 在標頭或 payload 讀到一半時會阻塞（socket 沒設 `SO_RCVTIMEO`），極端情況下封包只到一半會暫時卡住整個迴圈，直到剩餘資料到達或連線中斷。
- **關係**：被誰呼叫 → `client/main.c:122`（互動模式入口）；呼叫 → `term_init`、`term_restore`、`ob_flush`（`client/term.c`）、`ui_init`、`ui_resize`、`ui_status`（`client/ui.c`）、`cmd_keys`（`client/cmd.c`）、`try_connect`、`conn_recv`、`handle_packet`、`app_disconnect`、`app_flush_input`、`app_send`、`conn_bye`、`mono_ms`、`now_ns`。

---

## `client/term.c`

`client/term.c` 是互動式 `bench` 用戶端（`client/session.c` 的 `app_run()`）所用的終端機層，只在使用者執行 `bench <host>` 的那個 client 行程內執行，master、reader、per-client child 和 `bench info|tail|watch|send` 子指令都不會用到它（子指令不會呼叫 `term_init()`）。它不使用 ncurses，只靠 `termios` 與 ANSI escape sequence 完成三件事：把 stdin 切成 raw mode（保留 `OPOST|ONLCR`）、提供一個 256 KiB 的輸出緩衝（`ob_*` 系列）、以及用 scroll region 把最後一列留給狀態列。R11 `term-restore`（`--unsafe=term-restore`）的核心也在這裡：`term_init()` 預先建好還原用的 escape sequence，並註冊 `atexit()` 與致命訊號 handler，讓程式不論正常結束或被訊號打斷都能把終端機還原。本檔依賴 `client/client.h`（原型宣告）與 `include/bench_util.h`（`UNSAFE()` 巨集、`R_TERM_RESTORE`），使用者為 `client/ui.c`（畫面輸出）與 `client/session.c`（生命週期）。

檔案層級的靜態狀態：`saved`（進入 raw mode 前的 `struct termios`）、`raw_on`（目前是否處於 raw mode，也是 `term_restore()` 的冪等旗標）、`restore_seq[64]` / `restore_len`（預先建好的還原序列）、`obuf[256 * 1024]` / `olen`（輸出緩衝）。

### `ob_flush`
*位置：`client/term.c:32`　簽章：`void ob_flush(void)`*

- **用途**：把輸出緩衝 `obuf` 中累積的 `olen` 個位元組寫到 stdout，然後把緩衝清空。
- **設計**：整個 client 的畫面輸出（資料、狀態列、escape sequence）都先累積在記憶體，主迴圈每輪結束時才呼叫一次 `ob_flush()`（`client/session.c:422`），讓一輪內的多筆輸出變成少數幾次 `write()`，避免畫面閃爍，也避免每個小字串各產生一個系統呼叫。用迴圈是因為 `write()` 對 tty 可能只寫出一部分，必須用 `off` 續寫到全部寫完。
- **細節**：
  - `write()` 回傳 `<= 0`（錯誤或 0）就直接 `break`，不重試也不處理 `EINTR`；之後無論是否寫完都執行 `olen = 0`，剩餘資料會被丟棄，不會留在緩衝裡。`SIGWINCH` 的 handler 以 `SA_RESTART` 註冊（`client/session.c:345`），所以視窗縮放不會讓 `write()` 以 `EINTR` 失敗。
  - `olen == 0` 時迴圈不執行，呼叫是廉價的空操作。
  - 它會修改全域的 `olen`，且沒有任何鎖或訊號遮罩，因此不能在訊號 handler 中呼叫；`on_fatal()` 刻意不用它，改寫預先建好的 `restore_seq`。
- **關係**：被誰呼叫 → `ob_put()`（緩衝放不下時，`client/term.c:47`）、`term_restore()`、`client/session.c` 的 `try_connect()`（`:171`）、`app_run()`（`:348`、`:422`）；呼叫 → `write(2)`。

### `ob_put`
*位置：`client/term.c:44`　簽章：`void ob_put(const void *p, size_t n)`*

- **用途**：把 `n` 個位元組附加到輸出緩衝；是所有 `ob_*` 輸出函式與 `client/ui.c` 直接輸出原始位元組（例如 `ob_put(d + i - run, run)`）的基礎函式。
- **設計**：先判斷「加上這筆會不會超過 `sizeof obuf`」，會的話先 `ob_flush()` 把舊資料送出，這樣輸出順序不會亂。若單筆資料本身就大於整個緩衝（`n > sizeof obuf`），則不複製，flush 之後直接 `write()` 一次，免得緩衝放不下。把這個分支排在 flush 之後，保證大區塊一定排在先前累積的資料後面。
- **細節**：
  - `olen + n > sizeof obuf` 的判斷在 `n > sizeof obuf` 之前，因此大區塊的情況下一定會先 flush。
  - 直接寫的路徑只呼叫一次 `write()`，回傳值被丟棄（`(void)w`）：遇到部分寫入或錯誤時不會續寫。實際上 `p` 的來源是單一 DATA 封包的 payload（上限 `BN_MAX_PAYLOAD` = 65536），小於 256 KiB，這條路徑是防禦性的。
  - `memcpy` 之後 `olen += n`；`n == 0` 時什麼也不會改變。
  - 不做任何 escape 過濾；過濾由 `client/ui.c` 的 `esc_byte()` 負責。
- **關係**：被誰呼叫 → `ob_str()`、`ob_printf()`、`client/ui.c`（`:165-166`、`:204`、`:213`、`:226`）；呼叫 → `ob_flush()`、`write(2)`、`memcpy`。

### `ob_str`
*位置：`client/term.c:57`　簽章：`void ob_str(const char *s) { ob_put(s, strlen(s)); }`*

- **用途**：把一個以 NUL 結尾的字串附加到輸出緩衝。
- **設計**：單純的包裝，讓呼叫端對字面常數的 escape sequence 與換行寫 `ob_str("\033[2J")` 即可，不必自己算長度；變動內容才用 `ob_printf()`，來自裝置的原始位元組（可能含 NUL，長度已知）則走 `ob_put()`，不能用 `ob_str()`，否則遇到 NUL 就被截斷。
- **細節**：
  - 不包含結尾的 NUL；`s` 不可為 `NULL`（`strlen` 會當掉）。
  - 不做格式化，所以字串中的 `%` 不需要跳脫（這是與 `ob_printf()` 的差別）。
- **關係**：被誰呼叫 → `client/ui.c`（`:83` 清螢幕、`:104`、`:159`、`:164`、`:189-190`、`:215`、`:218`、`:237-241` 的 escape sequence、換行與分隔線）；呼叫 → `ob_put()`。其中 `:215` 送出單獨的 `"\n"`，依賴 raw mode 保留的 `ONLCR` 轉成 CR LF。

### `ob_printf`
*位置：`client/term.c:59`　簽章：`void ob_printf(const char *fmt, ...)`*

- **用途**：以 printf 格式格式化後附加到輸出緩衝，主要用來組出帶座標的 escape sequence（例如 `"\033[%d;1H"`）與狀態列內容。
- **設計**：先格式化到 1024 位元組的堆疊暫存區 `b`，再交給 `ob_put()`；這樣格式化結果不會被半途 flush 切開，也不需要動態配置。原型在 `client/client.h:113` 宣告了 `__attribute__((format(printf, 1, 2)))`，讓編譯器（搭配 `-Wformat=2`）檢查呼叫端的格式字串。
- **細節**：
  - `vsnprintf()` 回傳 `n`；`n <= 0`（格式化錯誤或空字串）時什麼都不輸出。
  - 若結果超過暫存區，`vsnprintf()` 回傳「應有長度」，程式把輸出長度截成 `sizeof b - 1`（1023 位元組），也就是超長輸出會被截斷，不會溢位。
  - 單次輸出以 1023 位元組為上限，呼叫端（座標移動、狀態列、說明框）都遠小於這個值。
- **關係**：被誰呼叫 → `term_set_region()`、`client/ui.c` 的 `ui_init()`、`ui_resize()`、狀態列與說明框繪製（`:85`、`:94`、`:96`、`:239`、`:397`、`:446` 等）；呼叫 → `vsnprintf`、`ob_put()`。

### `term_size`
*位置：`client/term.c:70`　簽章：`void term_size(int *rows, int *cols)`*

- **用途**：查詢目前終端機的列數與欄數，寫入 `*rows` / `*cols`。
- **設計**：用 `ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)` 取得視窗大小；若查詢失敗，或回報的值不合理，就退回 24x80 的傳統預設，確保後續的 scroll region 與狀態列計算一定有可用的數字。判斷合理性的門檻是 `ws_row > 2 && ws_col > 10`：scroll region 需要至少一列內容加一列狀態列（`rows - 1 >= 1`），欄位太窄也無法畫狀態列。
- **細節**：
  - 查詢對象是 stdout 而非 stdin；stdout 若被導向到檔案，`ioctl` 失敗，回傳 24x80。
  - 有些終端機（或剛建立的 pty）會回報 0x0，也會落入預設值。
  - 兩個輸出參數永遠都會被寫入，沒有回傳值。
- **關係**：被誰呼叫 → `term_init()`、`client/ui.c` 的 `ui_init()`（`:82`）與 `ui_resize()`（`:93`）；呼叫 → `ioctl(2)`。`SIGWINCH` 由 `client/session.c` 的 `on_winch()` 設旗標（`:123`），主迴圈偵測到旗標後呼叫 `ui_resize()`（`:376-378`），間接走到這裡。

### `build_restore`
*位置：`client/term.c:83`　簽章：`static void build_restore(int rows)`*

- **用途**：依目前的終端機高度 `rows`，預先組出「還原終端機」用的 escape sequence，存到 `restore_seq` 並記下長度 `restore_len`。
- **設計**：R11 要求在 `SIGSEGV` 等致命訊號 handler 裡還原終端機，而 handler 內只能呼叫 async-signal-safe 的函式；`snprintf()` 不在此列，`write()` 與 `tcsetattr()` 才是。因此把格式化提前到正常執行流程裡完成，handler 只需要 `write(restore_seq, restore_len)`。序列內容依序是：`\033[0m` 重設顏色與屬性、`\033[r` 重設 scroll region 為整個畫面、`\033[?25h` 顯示游標、`\033[%d;1H` 移到最後一列第一欄、`\033[K` 清掉該列（狀態列）、`\r\n` 換行讓 shell 的提示符號出現在乾淨的新行。
- **細節**：
  - 緩衝區 `restore_seq` 只有 64 位元組；固定部分共 23 位元組，加上 `rows` 的十進位位數（例如 `rows = 24` 時為 25 位元組），遠小於上限。
  - `restore_len` 是 `snprintf()` 的回傳值轉型，沒有檢查截斷。
  - 每次視窗大小改變後都要重建（`term_set_region()` 會呼叫），否則還原時會移到舊的最後一列。
  - 沒有訊號遮罩保護：若訊號恰好在主流程重寫 `restore_seq` 時到達，handler 可能讀到寫到一半的內容，這是此設計接受的視窗期。
- **關係**：被誰呼叫 → `term_init()`、`term_set_region()`；呼叫 → `snprintf`。使用者為 `term_restore()` 與 `on_fatal()`。

### `term_set_region`
*位置：`client/term.c:89`　簽章：`void term_set_region(int rows)`*

- **用途**：設定 ANSI scroll region 為第 1 到 `rows - 1` 列，讓第 `rows` 列（最後一列）成為不會捲動的狀態列；同時更新還原序列。
- **設計**：捲動區用 DECSTBM（`ESC [ top ; bottom r`）實現，這樣不需要 ncurses：UART 資料照常在上方捲動，狀態列固定在下方。DECSTBM 會把游標移到左上角（home），所以前後用 `ESC 7`（DECSC，存游標）與 `ESC 8`（DECRC，還原游標）包起來，使游標位置不被改變。呼叫 `build_restore(rows)` 是為了讓還原序列永遠對應「目前的」高度。
- **細節**：
  - 只寫入輸出緩衝，需之後 `ob_flush()` 才會生效。
  - 本函式不清螢幕、也不移動游標到底部；這些由呼叫端 `ui_init()`（先 `"\033[2J"`、之後 `"\033[%d;1H"`）與 `ui_resize()`（先擦掉舊的狀態列）處理。
  - region 的 top 固定為 1、bottom 為 `rows - 1`；`term_size()` 保證 `rows > 2`，所以 bottom 至少是 2，不會產生 top 大於 bottom 的無效範圍。
- **關係**：被誰呼叫 → `client/ui.c` 的 `ui_init()`（`:84`）與 `ui_resize()`（`:95`）；呼叫 → `ob_printf()`、`build_restore()`。

### `term_restore`
*位置：`client/term.c:95`　簽章：`void term_restore(void)`*

- **用途**：把終端機還原成進入前的狀態（屬性、scroll region、游標、termios）。可重複呼叫，只有第一次會真的做事。
- **設計**：這是 R11 的「正常退出路徑」。以 `raw_on` 作為冪等旗標，並且在做事之前就先設成 0，避免 `atexit` 與 `app_run()` 末尾各呼叫一次時重複還原，也避免還原到一半又被重入。順序有講究：先 `ob_flush()` 把已累積的輸出送完（否則之後才送出會在還原序列之後出現），再寫還原序列（回到整個畫面的捲動、顯示游標、清狀態列、換行），最後 `tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved)` 恢復 cooked mode；使用 `TCSAFLUSH` 會丟棄尚未讀取的輸入，避免殘留的按鍵字元漏到 shell。
- **細節**：
  - 若 `term_init()` 從未成功（`raw_on == 0`）就直接返回，所以非互動用途不會誤動終端機。
  - 寫入失敗與 `tcsetattr` 失敗都不檢查（`(void)w`）；此時已無補救辦法。
  - 它呼叫了 `ob_flush()`，因此不是 async-signal-safe，只用在一般（非訊號）上下文；訊號路徑用 `on_fatal()`。
  - `--unsafe=term-restore` 時 `atexit()` 不會註冊，但 `client/session.c:431` 在 `app_run()` 正常收尾時仍會直接呼叫它，所以「unsafe」影響的是當機與被訊號殺死的路徑，正常 `q` 離開仍然會還原。
- **關係**：被誰呼叫 → `atexit()`（由 `term_init()` 註冊）、`client/session.c` 的 `app_run()`（`:431`）；呼叫 → `ob_flush()`、`write(2)`、`tcsetattr(3)`。

### `on_fatal`
*位置：`client/term.c:106`　簽章：`static void on_fatal(int sig)`*

- **用途**：致命訊號（`SIGINT`、`SIGTERM`、`SIGHUP`、`SIGSEGV`、`SIGABRT`、`SIGBUS`、`SIGQUIT`）的 handler：先把終端機還原，再讓行程用原本的方式死去。
- **設計**：handler 內只呼叫 async-signal-safe 的 `write()`、`tcsetattr()`、`signal()`、`raise()`，並使用 `build_restore()` 預先建好的字串，而不是在 handler 內格式化。刻意不呼叫 `term_restore()`，因為後者會 `ob_flush()` 並操作緩衝，不適合在訊號上下文執行（程式可能正好在 `ob_put()` 中被打斷，或是記憶體已經損毀）。最後 `signal(sig, SIG_DFL)` 加 `raise(sig)`，是「die the way we were going to die」：保持原來的結束狀態（例如 shell 看到 `128 + sig`、`SIGSEGV` 仍能產生 core），而不是改成一般的 `exit()`。
- **細節**：
  - 只有 `raw_on` 為真時才還原，避免未進入 raw mode 就收到訊號時亂寫終端機；還原後設 `raw_on = 0`，使稍後的 `atexit` 不會重做。
  - 註冊時 `sa_flags` 為 0、`sa_mask` 為空，因此處理期間該訊號自己被阻擋；`raise()` 的訊號在 handler 返回後才投遞，此時處置已是 `SIG_DFL`，行程隨即終止。
  - raw mode 關閉了 `ISIG`，鍵盤上的 Ctrl-C 不會產生 `SIGINT`（被當成位元組送往裝置），所以 `SIGINT` 主要來自 `kill`。
  - 程式沒有使用 `sigaltstack()`，所以因堆疊溢位造成的 `SIGSEGV` 無法保證 handler 能執行；這是此設計未涵蓋的情況。`SIGPIPE` 也不在清單內，`client/main.c:74-77` 另行處理（R2）。
  - 只在 `!UNSAFE(R_TERM_RESTORE)` 時由 `term_init()` 安裝；`scripts/demo_r11.sh` 用 `kill -SEGV` 驗證。
- **關係**：被誰呼叫 → 由核心在訊號到達時呼叫（由 `term_init()` 的 `sigaction()` 註冊）；呼叫 → `write(2)`、`tcsetattr(3)`、`signal(2)`、`raise(3)`。

### `term_init`
*位置：`client/term.c:118`　簽章：`int term_init(void)`*

- **用途**：把 stdin 設成 raw mode，建立還原序列，並（除非 `--unsafe=term-restore`）安裝所有還原機制；成功回傳 0，失敗回傳 -1。
- **設計**：先 `tcgetattr` 存下原始設定到 `saved`，再在複本 `t` 上修改，以便日後逐位元還原。改法類似 `cfmakeraw()`，但刻意保留輸出處理：`c_iflag` 清除 `IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON`（Enter 送出 `\r`，Ctrl-S/Ctrl-Q 當成一般位元組送給裝置）；`c_lflag` 清除 `ECHO|ECHONL|ICANON|ISIG|IEXTEN`（由遠端裝置負責回顯，Ctrl-C/Ctrl-Z 變成資料，行編輯由遠端處理）；`c_cflag |= CS8`；`c_oflag |= OPOST|ONLCR`，這是 `docs/DECISIONS.md` 所述「raw mode 保留 `OPOST|ONLCR`」，讓 UART 傳來的單獨 `\n` 仍回到第 1 欄；`VMIN = 1`、`VTIME = 0` 讓 `read()` 至少等一個位元組。使用 `TCSAFLUSH` 同時丟掉先前輸入。`raw_on = 1` 和 `build_restore(rows)` 在設定成功之後、安裝 handler 之前完成，所以只要 handler 存在，就一定有可用的還原序列。
- **細節**：
  - `tcgetattr` 失敗（stdin 不是 tty）或 `tcsetattr` 失敗就回傳 -1，此時 `raw_on` 仍為 0；`client/session.c:341` 於是印出 "stdin is not a terminal (use `bench watch` for scripts)" 並回傳 1。
  - 安裝 `atexit(term_restore)` 與用 `sigaction()` 為 `SIGINT, SIGTERM, SIGHUP, SIGSEGV, SIGABRT, SIGBUS, SIGQUIT` 設定 `on_fatal`；檔案開頭註解的列表少了 `SIGBUS`、`SIGQUIT`，以程式碼為準。
  - `--unsafe=term-restore`（`UNSAFE(R_TERM_RESTORE)` 為真）時以上兩項都不安裝：終端機仍被切成 raw mode，但行程當機或被訊號殺死後 shell 會留在 raw mode（沒有回顯、沒有行編輯）。正常退出仍由 `app_run()` 末尾的 `term_restore()` 還原。
  - 此函式不處理 `SIGWINCH`（由 `client/session.c:345` 安裝），也不設定 scroll region 或清螢幕（由 `ui_init()` 負責）。
- **關係**：被誰呼叫 → `client/session.c` 的 `app_run()`（`:341`）；呼叫 → `tcgetattr`/`tcsetattr`、`term_size()`、`build_restore()`、`atexit`、`sigaction`。

---

## `client/ui.c`

`client/ui.c` 是互動式 `bench` 客戶端（`bench <host>`，不含 `info|tail|watch|send` 子命令）的繪圖層，只在 client 行程內執行；server（master / reader / per-client child）不會連結它。畫面分成兩塊：第 1 到第 rows-1 列是捲動區（UART 輸出、分隔線、info/help 方框），第 rows 列是狀態列。游標永遠停在捲動區內，狀態列用 `ESC 7` / `ESC 8`（儲存／還原游標）包住繪製，所以輸出不會寫到狀態列上。本檔提供 `ui_*` 系列函式（宣告在 `client/client.h`）給 `client/session.c`（收到封包時畫資料、事件、info 方框）與 `client/cmd.c`（按鍵／命令列時閃訊息、畫 help）呼叫，另外 `str_width()` 與 `info_box_lines()` 也被 `client/subcmd.c` 的 `bench info` 重用。所有輸出都經 `client/term.c` 的輸出緩衝（`ob_str` / `ob_put` / `ob_printf`），不直接 `write()`；真正送到終端機是 `session.c` 主迴圈裡的 `ob_flush()`。本檔相依 `client.h`（`struct app`、`FL_*`、`ESC_*` 狀態列舉）與 `bench_util.h`（`fmt_clock`、`fmt_hms`、`fmt_size`、`hexdump_line`、`mono_ms`）；檔內沒有任何 `UNSAFE()` 判斷，`--unsafe` 開關對本檔沒有直接作用，只有間接影響（見各函式）。

### `cp_width`
*位置：`client/ui.c:22`　簽章：`static int cp_width(uint32_t cp)`*

- **用途**：回傳一個 Unicode code point 在終端機佔的欄數，CJK 寬字元回 2，其餘回 1。
- **設計**：不引入 `wcwidth()` / locale 相依，直接用一張寫死的範圍表判斷東亞全形區；方框、分隔線與狀態列要靠欄數對齊，而訊息（例如 `read-only — Ctrl-] t 取得控制`）會混入中文，所以 `strlen` 不夠用。
- **細節**：
  - 回 2 的範圍：`0x1100-0x115F`（Hangul Jamo）、`0x2E80-0xA4CF`、`0xAC00-0xD7A3`（Hangul 音節）、`0xF900-0xFAFF`、`0xFE30-0xFE4F`、`0xFF00-0xFF60`、`0xFFE0-0xFFE6`、`0x20000-0x3FFFD`。
  - 其他一律回 1：不處理零寬／組合字元、emoji，也不處理「ambiguous width」字元；`─`、`│` 因此被當成 1 欄（DECISIONS 提到的 GNU screen + zh_TW locale 換行問題就是出在這個假設）。
  - 控制字元也回 1，不回 0。
- **關係**：被 `str_width`、`str_fit` 呼叫。

### `utf8_next`
*位置：`client/ui.c:32`　簽章：`static size_t utf8_next(const char *s, uint32_t *cp)`*

- **用途**：從 NUL 結尾字串 `s` 解出下一個 UTF-8 字元，寫入 `*cp`，回傳消耗的位元組數（1 到 4）。
- **設計**：手寫解碼器而不用 `mbrtowc`，結果不受 locale 影響。每個多位元組分支都同時檢查後續位元組「非 0」（`u[1]`、`u[2]`、`u[3]`），利用 `&&` 短路，保證遇到字串結尾 NUL 時不會讀過 NUL，不需要另外傳長度。
- **細節**：
  - 單位元組 `< 0x80`：原樣回傳。2/3/4 位元組由前導位元組 `110xxxxx` / `1110xxxx` / `11110xxx` 判斷，把各位元組的低 6 位元拼起來。
  - 只檢查後續位元組不是 0，不檢查它們是否為 `10xxxxxx`，也不拒絕 overlong 編碼；本檔只用來量寬度與裁切，不做安全驗證。
  - 其他情況（孤立的 continuation byte `0x80-0xBF`、`0xF8` 以上、被 NUL 截斷的序列）把 `*cp` 設為 `'?'` 並只消耗 1 位元組，確保呼叫端的迴圈一定前進。
- **關係**：被 `str_width`、`str_fit` 呼叫。

### `str_width`
*位置：`client/ui.c:49`　簽章：`int str_width(const char *s)`*

- **用途**：回傳 UTF-8 字串在終端機上的顯示欄數（CJK 算 2 欄）。
- **設計**：逐字元用 `utf8_next` 解碼、`cp_width` 累加。這是本檔唯一對外公開的寬度函式（宣告在 `client/client.h`），讓 `subcmd.c` 的非互動 `bench info` 也能用同樣的規則把方框對齊。
- **細節**：
  - 輸入必須是 NUL 結尾字串；沒有長度上限。
  - 不看跳脫序列：字串中若含 ESC 序列也會被當成一般字元計算，所以只能傳純文字。
- **關係**：被 `ui_separator`、`box_rule`、`ui_status`（本檔）與 `client/subcmd.c` 的 `sub_info` 呼叫；呼叫 → `utf8_next`、`cp_width`。

### `str_fit`
*位置：`client/ui.c:61`　簽章：`static int str_fit(char *out, size_t outsz, const char *s, int maxw)`*

- **用途**：把 `s` 複製到 `out`，最多佔 `maxw` 欄，回傳實際用掉的欄數；`out` 一定以 NUL 結尾。
- **設計**：以「字元」為單位裁切，不會把一個多位元組字元從中間切開（切開會在終端機產生亂碼）；同時用 `o + l + 1 > outsz` 保護輸出緩衝區，所以即使 `s` 很長也不會溢位。回傳實際欄數讓呼叫端能算出要補幾個空白，把整列填滿。
- **細節**：
  - 下一個字元放不下（`w + cw > maxw`，寬字元在只剩 1 欄時也放不下）就停，不會用空白補。
  - `maxw` 為負或 0 時，`out` 變成空字串、回傳 0。
  - 呼叫端傳入的 `outsz` 都是 512（`sizeof fit`），所以 `out[o] = 0` 一定在界內。
- **關係**：被 `box_line`、`ui_status` 呼叫；呼叫 → `utf8_next`、`cp_width`。

### `ui_init`
*位置：`client/ui.c:80`　簽章：`void ui_init(struct app *a)`*

- **用途**：進入互動模式時初始化畫面：量終端機大小、清螢幕、設定捲動區、把游標放到捲動區最底列並畫出狀態列。
- **設計**：先 `term_size()` 取得 `rows`/`cols`，再 `ESC[2J` 清畫面，`term_set_region(rows)` 把捲動區設為第 1 到第 rows-1 列（同時預先組好 crash/結束時用的還原序列，見 R11 `term-restore`）。游標移到第 rows-1 列第 1 欄，讓第一批輸出從底部往上捲，而不是從畫面頂端開始。`at_bol = 1` 表示現在位於行首，之後第一行不用先補換行。
- **細節**：
  - 只往輸出緩衝寫，不呼叫 `ob_flush()`；由呼叫端 `app_run` 在 `ui_init(a)` 之後立刻 `ob_flush()`。
  - `term_size()` 在取不到視窗大小（或 `rows <= 2` / `cols <= 10`）時退回 24×80。
- **關係**：被 `client/session.c` 的 `app_run` 呼叫（`term_init()` 成功並安裝 `SIGWINCH` handler 之後）；呼叫 → `term_size`、`term_set_region`、`ob_str`、`ob_printf`（`client/term.c`）、`ui_status`。

### `ui_resize`
*位置：`client/ui.c:90`　簽章：`void ui_resize(struct app *a)`*

- **用途**：終端機大小改變（`SIGWINCH`）後重新配置畫面：擦掉舊狀態列、套用新捲動區、在新的最後一列重畫狀態列。
- **設計**：訊號 handler 只設旗標 `got_winch`，真正的重繪在主迴圈裡由這個函式做，避免在 handler 裡呼叫非 async-signal-safe 的函式。先記下 `old_rows`，重新 `term_size()` 之後，用 `ESC 7` + 移到舊狀態列 + `ESC[K` + `ESC 8` 擦掉舊狀態列（視窗變高時，舊狀態列會落入新的捲動區，不擦會留下殘影）。再 `term_set_region()` 設新區域，並把游標放到新捲動區最底列。
- **細節**：
  - 不清整個螢幕，捲動區裡的既有內容保留給終端機自己處理。
  - 游標被移到新第 rows-1 列的第 1 欄並設 `at_bol = 1`，若原本停在一行未完成的文字中間，後續輸出會在該列從第 1 欄覆寫。
- **關係**：被 `client/session.c` 的 `app_run` 主迴圈在 `got_winch` 為真時呼叫；呼叫 → `term_size`、`term_set_region`、`ob_printf`、`ui_status`。

### `newline_if_needed`
*位置：`client/ui.c:101`　簽章：`static void newline_if_needed(struct app *a)`*

- **用途**：如果游標不在行首（`at_bol == 0`），先輸出 `"\r\n"` 並把 `at_bol` 設為 1。
- **設計**：裝置輸出常常停在一行中間（例如 `login: ` 提示字元）。分隔線、方框、備註若直接接在後面會黏在同一行，所以這些「整行」輸出在動手前都先呼叫它。`at_bol` 這個旗標就是為此而維護的。
- **細節**：已在行首時什麼都不做，所以連續畫多個方框之間不會多出空行。
- **關係**：被 `ui_data`（hex 分支）、`ui_separator`、`ui_note`、`draw_box` 呼叫；呼叫 → `ob_str`。

### `ts_prefix`
*位置：`client/ui.c:109`　簽章：`static void ts_prefix(uint64_t ts_ns)`*

- **用途**：輸出一個暗色的時間戳記前綴 `[HH:MM:SS.mmm] `。
- **設計**：時間來自參數 `ts_ns`（封包上由 server 蓋的 realtime 奈秒時間），不是 client 收到的當下時間，所以所有 client 看到的同一行時間相同，replay 的歷史資料也顯示當初收到的時間（DECISIONS：`:ts` 以該行第一個字元所在 record 的 server 時間為準）。以 `fmt_clock` 格式化（毫秒精度），用 `DIM` 與 `RESET` 包起來，跟 UART 文字視覺上分開。
- **細節**：
  - `fmt_clock` 內部用 `localtime_r`，所以顯示的是 client 本機時區。
  - 本函式不改 `at_bol`，由呼叫端自行處理。
- **關係**：被 `ui_data` 呼叫；呼叫 → `fmt_clock`（`lib/util.c`）、`ob_printf`。

### `esc_byte`
*位置：`client/ui.c:124`　簽章：`static void esc_byte(struct app *a, uint8_t ch)`*

- **用途**：ANSI 跳脫序列過濾器的狀態機，一次吃一個位元組，決定這個序列要原樣送給終端機還是丟掉。
- **設計**：真實裝置（例如 PYNQ 的 zsh 提示字元、`ls --color`）會送大量跳脫序列。全部放行的話，裝置可以用絕對定位把游標移到狀態列、用 `ESC[r` 重設捲動區、用查詢讓終端機回應（回應會被當成鍵盤輸入）。因此採白名單：只放行 SGR 顏色（`m`）、清除行（`K`）、相對游標移動（`A B C D G`）和插入／刪除／清除字元（`@ P X`）。狀態（`esc_state`、`esc_buf`、`esc_len`）存在 `struct app` 而不是區域變數，所以被切在兩個 DATA record 之間的序列仍能接續解析。
- **細節**：
  - 狀態（`client.h` 的列舉）：`ESC_NONE`（不在序列中，其實不會進來，`ui_data` 只在 `esc_state != 0` 時呼叫）、`ESC_GOT_ESC`（剛收到 `0x1b`）、`ESC_CSI`、`ESC_STRING`、`ESC_STRING_ESC`、`ESC_ONE_MORE`。
  - `ESC_GOT_ESC`：下一個位元組是 `[` → 進 `ESC_CSI` 並清 `esc_len`；是 `]`、`P`、`_`、`^`、`X`（OSC / DCS / APC / PM / SOS）→ 進 `ESC_STRING` 吞掉；是 `( ) * + # %`（字元集指定等）→ 進 `ESC_ONE_MORE`，再吞一個位元組；其他（`ESC 7`、`ESC 8`、`ESC c`、`ESC M`、`ESC =`，包含連續兩個 ESC）→ 直接回 `ESC_NONE`，等於整個序列被丟掉。
  - `ESC_STRING`：遇到 `0x07`（BEL）結束；遇到 `0x1b` 轉 `ESC_STRING_ESC`。`ESC_STRING_ESC`：下一個是 `\` 才結束（ST），否則回 `ESC_STRING` 繼續吞。字串沒有長度或逾時限制：未終止的 OSC 會一直吞到出現 BEL 或 `ESC \`。
  - `ESC_CSI`：`0x40-0x7e` 是 final byte。已緩衝的參數若以 `?`、`>`、`<`、`=` 開頭（私有模式，例如 `ESC[?2004h`）→ 丟掉。final 是 `J`（清除顯示）→ 改輸出 `ESC[K`（只清到行尾），保證不會清到狀態列，參數被忽略。final 不在 `"mKABCDGPX@"` 內（例如 `H`、`f`、`r`、`n`、`c`）→ 丟掉。通過者輸出 `ESC[` + 暫存的參數 + final。
  - 非 final byte：`< 0x20`（控制字元，該位元組本身也被丟掉）或暫存區已滿（`esc_buf` 為 32 位元組）→ 視為異常，放棄整個序列；否則存入 `esc_buf`（不補 NUL，用 `esc_len` 計長度）。放棄的設計理由是：上限讓壞資料或惡意裝置無法讓過濾器無限期停在 CSI 狀態而吞掉後續正常輸出（與 `ESC_STRING` 不同，CSI 有長度上限）。代價是若中斷序列的位元組恰好是新的 `ESC`，它會連同舊序列一起被丟掉，新序列的 `[` 之後的內容反而會當成一般文字顯示。
  - 過濾器只在文字模式使用；hex 模式不經過它。
- **關係**：被 `ui_data` 呼叫（僅當 `a->esc_state` 非 0）；呼叫 → `ob_str`、`ob_put`。

### `ui_data`
*位置：`client/ui.c:180`　簽章：`void ui_data(struct app *a, const uint8_t *d, size_t n, uint64_t ts_ns)`*

- **用途**：把一個 DATA record 的 UART 位元組畫到捲動區，支援三種模式：一般文字（經 ANSI 過濾）、`:ts` 時間戳記、`:hex` 十六進位傾印。
- **設計**：
  - hex 模式：先 `newline_if_needed`，每 16 位元組呼叫 `hexdump_line` 輸出一行（`line[96]` 足夠），`ts_mode` 開啟時每一行都加同一個 record 時間戳記，最後 `at_bol = 1` 並返回。因為每筆資料自成完整的行，這個模式不需要追蹤行首狀態，也不經過 ANSI 過濾，所以能看到每一個位元組（DECISIONS 建議用 `:hex` 檢視被過濾的內容）。
  - 文字模式：逐位元組掃描，把連續的「一般」位元組累積成 `run`，遇到需要特別處理的位元組才一次 `ob_put` 出去，避免一個位元組呼叫一次輸出函式。「一般」指 `>= 0x20` 且非 `0x7f`，加上 Tab 與 Backspace；`>= 0x80` 的位元組也算一般，所以 UTF-8 原樣通過。
  - 時間戳記：`ts_mode` 且 `at_bol` 時，遇到一般位元組或 ESC 就先 flush 已累積的 `run`、輸出 `ts_prefix`、把 `at_bol` 設 0。所以空行（只有 `\n`）不會被加時間戳記，一行的時間是「該行第一個有內容位元組所在 record」的時間；ESC 也觸發是為了讓以顏色序列開頭的行，前綴出現在顏色之前。
  - 控制字元：`\n` → 輸出 `"\n"`（終端機的 `OPOST|ONLCR` 會變成 CR LF，見 `term.c`）並設 `at_bol = 1`；`\r` → 原樣輸出且不改 `at_bol`（所以進度條類的 `\r` 覆寫不會重複蓋時間戳記）；`0x1b` → 進入 `ESC_GOT_ESC`、`esc_len = 0`；其他控制位元組（NUL、BEL、`0x7f` 等）直接丟掉。
  - 序列可跨 record：迴圈一開始如果 `a->esc_state` 非 0，位元組交給 `esc_byte` 並 `continue`。
- **細節**：
  - 迴圈結尾把剩下的 `run` 一次輸出。
  - 以 ESC 觸發的時間戳記，即使該序列之後被過濾器丟掉也已經輸出了，且 `at_bol` 已設 0。
  - 本函式不寫 log 檔；`session.c` 在呼叫 `ui_data` 之後，另外把原始位元組（未過濾）`fwrite` 到 `a->logf`。
  - `--unsafe=overrun`（R10）是 server 端開關（DECISIONS：naive reader 不比對 offset 與 `tail`，被覆寫的位元組含 record header 會到達畫面）；本函式沒有對應分支，只是照常顯示收到的位元組。safe 模式下 client 收到的是 `DROPPED` 事件，由 `ui_event` 畫成分隔線。
- **關係**：被 `client/session.c` 的 `handle_packet`（`BN_DATA` 分支）呼叫；呼叫 → `esc_byte`、`ts_prefix`、`newline_if_needed`、`hexdump_line`（`lib/util.c`）、`ob_put`、`ob_str`。

### `ui_separator`
*位置：`client/ui.c:229`　簽章：`void ui_separator(struct app *a, const char *text)`*

- **用途**：畫一條置中文字的暗色分隔線 `──── text ────`，例如 `replay end · live`、`reconnected`、`server restarted`。
- **設計**：先 `newline_if_needed` 以免接在半行文字之後。寬度取 `a->cols - 1`（不碰最後一欄，避免自動換行），扣掉文字寬度加兩個空白後，左右平分；除不盡的一欄給右邊。文字寬度用 `str_width` 算，所以中文標題也能置中。
- **細節**：
  - 左右各至少 2 條線（`left < 2` / `right < 2` 時補到 2）；文字太長時整行會超過 `cols - 1` 而換行，文字本身不會被截斷。
  - 用 `DIM` … `RESET` 包住並以 `"\r\n"` 結束，`at_bol = 1`。
  - 線條字元 `─` 以 1 欄計算（見 `cp_width`）。
- **關係**：被 `ui_event` 呼叫；被 `client/session.c` 呼叫（重連成功的 `reconnected` / `server restarted`、`BN_REPLAY_END` 的 `replay end · live`）；呼叫 → `newline_if_needed`、`str_width`、`ob_str`、`ob_printf`。

### `ui_note`
*位置：`client/ui.c:245`　簽章：`void ui_note(struct app *a, const char *fmt, ...)`*

- **用途**：在捲動區輸出一行 printf 格式的暗色備註文字。
- **設計**：先用 `vsnprintf` 格式化到 512 位元組的區域緩衝（過長會被截斷，不會溢位），再 `newline_if_needed`，以 `DIM` … `RESET` 輸出並換行、`at_bol = 1`。與 `ui_flash` 的差別：備註是捲動區的永久內容，不會消失；flash 是狀態列上的暫時訊息。
- **細節**：`client.h` 宣告了 `__attribute__((format(printf, 2, 3)))`，編譯器會檢查格式字串。
- **關係**：被 `client/session.c` 的 `app_disconnect` 呼叫（用於 `--no-reconnect` 或 `--unsafe=reconnect`（R12）時印 `connection lost: ...`，之後 `quit = 2` 結束）；呼叫 → `newline_if_needed`、`ob_printf`。

### `ui_event`
*位置：`client/ui.c:257`　簽章：`void ui_event(struct app *a, int code, const char *text, uint64_t ts_ns)`*

- **用途**：把 server 在 ring 裡插入的 EVENT record（裝置斷線／恢復、控制權變動、baud 變更、`DROPPED`、server 關閉）畫成一條分隔線。
- **設計**：事件和資料走同一個 ring，所以所有 client 在相同位置看到相同事件；這裡只負責呈現。`EV_DROPPED`（client 太慢、ring 被覆寫，R10）的文字格式為 `[text]`（server 送的文字像 `dropped 12.3K`）；其他事件為 `text hms`，例如 `device reconnected 20:44:13`，時間用 `fmt_hms`（秒精度，client 本機時區）取自事件上 server 蓋的時間。最後交給 `ui_separator`。
- **細節**：
  - `hms[16]`、`b[300]`；`session.c` 已把事件文字截到 299 位元組以內，`snprintf` 本身也會截斷，不會溢位。
  - 本函式不更新 `dev_state`；`session.c` 在 `ui_event` 之後才依 `EV_DEVICE_LOST` / `EV_DEVICE_BACK` 修改 `a->dev_state`（所以狀態列要等下一次 `ui_status` 才會反映）。
- **關係**：被 `client/session.c` 的 `handle_packet`（`BN_EVENT` 分支）呼叫；呼叫 → `fmt_hms`、`ui_separator`。

### `box_line`
*位置：`client/ui.c:273`　簽章：`static void box_line(int inner, const char *text)`*

- **用途**：輸出方框的一行內容 `│ text<padding> │`，並補空白對齊右邊框。
- **設計**：文字用 `str_fit` 裁成 `inner - 2` 欄（過寬就截斷，避免破壞邊框），再用 `%*s` 補 `inner - 2 - w` 個空白，其中 `w` 是實際欄數。補空白的算法是用 `str_width`/`str_fit` 的欄數，而不是位元組數，所以中英混合也能對齊。
- **細節**：注意參數 `inner` 的含義和 `box_rule` 不同：`draw_box` 傳入的是 `inner + 2`（內容寬度加上兩側的空白），因此內容區實際寬度是 `draw_box` 的 `inner`，整行總寬為 `inner + 4`，與上下框線等寬。
- **關係**：被 `draw_box` 呼叫；呼叫 → `str_fit`、`ob_printf`。

### `box_rule`
*位置：`client/ui.c:280`　簽章：`static void box_rule(int inner, const char *l, const char *title, const char *r)`*

- **用途**：輸出方框的上框線或下框線：左角 `l`、可選的標題、水平線、右角 `r`，並換行。
- **設計**：有標題時先輸出 `─ title `，已用寬度 `used = str_width(title) + 3`（前面的 `─ ` 佔 2 欄、後面的空白佔 1 欄）；再用 `─` 補到 `inner + 2` 欄。因此整行寬度 = 兩個角 + `inner + 2` = `inner + 4`，與 `box_line` 的總寬一致。
- **細節**：
  - 下框線傳 `title = NULL`（或空字串）就只畫水平線。
  - 標題比框寬時不會被截斷，只是不再補 `─`。
- **關係**：被 `draw_box` 呼叫；呼叫 → `str_width`、`ob_str`、`ob_printf`。

### `draw_box`
*位置：`client/ui.c:294`　簽章：`static void draw_box(struct app *a, const char *title, char lines[][256], int n)`*

- **用途**：在捲動區畫一個完整的方框：上框線（含標題）、`n` 行內容、下框線。
- **設計**：內容寬度 `inner = cols - 4`，但限制在 20 到 70 欄之間：太寬的終端機上長方框不好讀，太窄時至少保持可用的最小寬度。先 `newline_if_needed`，畫完設 `at_bol = 1`。
- **細節**：`lines` 是呼叫端準備好的 `char[][256]`，每行都是純文字（不含跳脫序列），過長由 `box_line` 截斷。
- **關係**：被 `ui_info_box`、`ui_help` 呼叫；呼叫 → `newline_if_needed`、`box_rule`、`box_line`。

### `info_box_lines`
*位置：`client/ui.c:310`　簽章：`int info_box_lines(const char *kv, size_t len, int width, char lines[][256], int max)`*

- **用途**：把 server 的 INFO 文字（每行 `key=value\n`，key 以 `host.`、`server.`、`dev.`、`hist.`、`ctrl.` 開頭）轉成分段的縮排文字行，回傳行數（最多 `max`）。
- **設計**：固定的段落表依序是 `host.`→`Host`、`server.`→`Server`、`dev.`→`Device`、`hist.`→`History`、`ctrl.`→`Control`。對每個段落掃整份 `kv`，因此畫面上的段落順序固定，段內保持 server 送出的順序。輸入用 `memchr` 配合 `len` 逐行切，而不是 `strtok`/`strchr`，因為封包 payload 不保證以 NUL 結尾，也不能被改動。這個函式不碰輸出緩衝，只回傳文字行，所以互動 UI（方框）與非互動的 `bench info`（用 `printf` 自己畫框）可以共用。
- **細節**：
  - 每個段落第一次出現有效行時，若已經有內容，先插入一個空行，再輸出段落標題（例如 `Host`）。
  - 資料行格式 `"  %-16.*s %.*s"`：兩個空格縮排、去掉前綴的 key 左對齊補到至少 16 字元（超過不截斷）、一個空白、`=` 後的值。value 超過 256 位元組的緩衝時由 `snprintf` 截斷。
  - 沒有 `=` 的行、不屬於任何段落的行會被略過。
  - 參數 `width` 目前未使用（`(void)width`）。
  - 迴圈條件都有 `n < max`；寫標題行（空行後）時沒有再檢查一次，但 server 的 INFO（`server/sysinfo.c`）固定 29 行 `key=value`，加 5 個標題與 4 個空行共 38 行，遠小於呼叫端的 `max = 64`，邊界不會被觸發。
- **關係**：被 `ui_info_box`（本檔）與 `client/subcmd.c` 的 `sub_info`（寬度參數傳 80，被忽略）呼叫；INFO 文字由 `server/sysinfo.c` 產生。

### `ui_info_box`
*位置：`client/ui.c:345`　簽章：`void ui_info_box(struct app *a, const char *kv, size_t len)`*

- **用途**：收到 INFO 回應後，在捲動區畫出標題為 `System info` 的系統資訊方框。
- **設計**：薄包裝：開一個 `char lines[64][256]`（區域變數，約 16 KB，放在 stack 上），用 `info_box_lines` 轉換，再交給 `draw_box`。
- **細節**：`kv` 是封包 payload 指標與長度，不要求 NUL 結尾。
- **關係**：被 `client/session.c` 的 `handle_packet`（`BN_INFO` 分支，僅當 `info_pending` 為真，也就是使用者剛送過 `info` 命令）呼叫；呼叫 → `info_box_lines`、`draw_box`。

### `ui_help`
*位置：`client/ui.c:352`　簽章：`void ui_help(struct app *a)`*

- **用途**：畫出標題為 `Keys` 的按鍵與命令說明方框（`Ctrl-] t/r/i/s/h/l/?/q`、`:baud`、`:` 命令列、`Ctrl-] Ctrl-]`）。
- **設計**：說明文字是函式內的 `static const char *h[]`（11 行），列舉出每個快捷鍵對應的 `:` 命令名稱，與 `cmd.c` 的 `cmd_exec` 與 `escape_key` 一致。複製到 `char lines[16][256]` 後交給 `draw_box`，這樣和 info 方框走同一套排版與截斷。
- **細節**：文字內容寫死，不隨狀態改變；中文提示不在這裡，只有英文與符號。
- **關係**：被 `client/cmd.c` 的 `cmd_exec`（`help` 命令，即 `Ctrl-] ?`）呼叫；呼叫 → `draw_box`。

### `ui_flash`
*位置：`client/ui.c:376`　簽章：`void ui_flash(struct app *a, int color, const char *fmt, ...)`*

- **用途**：在狀態列上暫時顯示一則訊息（取代平常的 `ctrl:/viewers/rx` 資訊），並立即重畫狀態列。
- **設計**：訊息不寫進捲動區，因為這些回饋（`timestamps on`、`you have control` …）不應污染 UART 紀錄。訊息文字存進 `a->flash`（200 位元組，`vsnprintf` 截斷）、顏色存 `a->flash_color`（`FL_INFO`=0、`FL_OK`=1、`FL_WARN`=2、`FL_ERR`=3）。到期時間 `flash_until = mono_ms() + 4000`（`FL_ERR`、`FL_WARN`，要讓人來得及讀）或 `+ 2500`（`FL_INFO`、`FL_OK`）。用單調時鐘 `mono_ms()`，不受系統時間調整影響。
- **細節**：
  - 到期的清除不在本檔：`session.c` 主迴圈檢查 `now >= flash_until` 時把 `flash[0]` 清零並呼叫 `ui_status`，每秒也會順便重畫一次。
  - 新的 flash 直接覆蓋舊的，沒有佇列。
  - `client.h` 宣告了 printf 格式檢查屬性。
- **關係**：被 `client/cmd.c`（`cmd_exec`、`escape_key`、`log_toggle`、`cmd_keys` 等）與 `client/session.c`（連線中斷警告、`BN_CTRL_RESULT`（取得控制權的結果）、server 錯誤）呼叫；呼叫 → `mono_ms`（`lib/util.c`）、`ui_status`。

### `ui_status`
*位置：`client/ui.c:387`　簽章：`void ui_status(struct app *a)`*

- **用途**：重畫最底列的狀態列：左邊是色塊徽章，右邊是裝置、baud、控制權、觀看人數、已收位元組數與旗標，或是暫時的 flash 訊息；命令列模式時改成顯示正在輸入的命令。
- **設計**：
  - 繪製一律是 `ESC 7`（存游標）→ 移到第 `a->rows` 列 → 輸出整列 → `ESC 8`（還原游標），所以不論呼叫時機，捲動區的游標位置都不受影響，這也是狀態列能和串流輸出共存的關鍵。
  - 一開始先 `a->last_status_draw = mono_ms()`，主迴圈據此每秒（`>= 1000 ms`）重畫，讓 `idle` 秒數與重連倒數會動。
  - 命令列模式（`cmd_mode`）：顯示 `:` 加上 `a->cmd` 的前 `cmd_len` 個位元組（`cmd` 在按 Enter 前沒有 NUL 結尾，所以用 `%.*s`），背景 256 色 236，後面接一個反白空格當游標，用 `str_fit` 裁成 `cols - 2` 欄後補空白，畫完直接返回，不顯示徽章。
  - 徽章依序判斷，先成立者勝出，順序反映「哪個資訊對使用者最要緊」：(1) 連線不是 `ST_LIVE`（黃底）時，畫面上的資料和控制狀態都已不可信，所以蓋過一切：`st == ST_HANDSHAKE` 或從未收過 WELCOME（`!ever_welcomed`）時顯示 `CONNECTING`（`ever_welcomed` 為假）或 `RECONNECTING`（為真），不帶秒數；只有「已 welcome 過、目前 `ST_DISCONNECTED`」才顯示 ` RECONNECTING (Ns) `，`N = ceil((retry_at - now) / 1000)`，`retry_at` 已過則為 0，倒數取自 `app_disconnect` 設的 backoff。(2) 連線正常但 `dev_state != DEV_UP` → 紅底 `DEV LOST`，排在 `CTRL` 之前，因為裝置掉了即使握有控制權，輸入也送不出去，使用者必須先看到這件事。(3) `is_ctrl` → 綠底 `CTRL`。(4) 其餘 → 灰底 `VIEW`（唯讀）。
  - 右側內容：若有未過期的 flash（`flash[0]` 非空且 `now < flash_until`），顯示 ` ` + flash 文字，顏色依類型：`FL_ERR` 亮紅、`FL_WARN` 亮黃、`FL_OK` 亮綠、其他亮青。否則顯示 ` │ <dev_path 或 ?> <baud> 8N1 │ <ctrl> │ <N> viewer(s) │ rx <大小>`，其中 `<ctrl>` 為 `ctrl: none`（無人持有）、`ctrl: you (idle Ns)`（自己持有）或 `ctrl: <name> (idle Ns)`，大小由 `fmt_size` 格式化（例如 `12.3K`）；有旗標時再接 ` │` 與 ` TS`、` HEX`、` REC`（`ts_mode`、`hex_mode`、`logf` 非空）。
  - 最後用 `str_fit` 把右側裁成 `cols - 1 - 徽章寬度` 欄，再補空白到同樣寬度，使整列背景連續；刻意不寫最後一欄，避免在最底列觸發自動換行造成整個畫面上捲。
- **細節**：
  - 徽章與右側的數值全部取自 `struct app`（由 `session.c` 在收到 `WELCOME`、`STATUS` 封包時更新），本函式只讀取不修改（除了 `last_status_draw`）。
  - `--unsafe=reconnect`（R12）或 `--no-reconnect` 時，斷線後 client 直接結束，所以不會看到 `RECONNECTING` 徽章。
  - 終端機極窄（欄寬小於徽章寬度加 1）時，`str_fit` 的上限為負，右側變空字串，`%*s` 收到負寬度，等同左對齊補 `|寬度|` 個空白；一般大小的終端機不會遇到。
- **關係**：被 `ui_init`、`ui_resize`、`ui_flash`（本檔）以及 `client/session.c`（`app_disconnect`、`try_connect`（每次連線嘗試前）、`BN_STATUS`、主迴圈每秒重畫與 flash 到期）和 `client/cmd.c`（進出命令列、Enter 後）呼叫；呼叫 → `mono_ms`、`fmt_size`、`str_width`、`str_fit`、`ob_printf`。

---

## `client/cmd.c`

這個檔案是互動式 `bench` 用戶端的鍵盤處理層，只在執行 `bench <host>`（互動模式）的 bench client 行程中執行；`benchd` 的 master、reader、per-client child 以及 `bench info|tail|watch|send` 子命令都不會用到它。`session.c` 的 `app_run()` 主迴圈每次從 stdin `read()` 到位元組，就整批交給 `cmd_keys()`；這個函式把 `Ctrl-]`（`ESC_KEY`，0x1D）當成跳脫鍵，其後的一個鍵選擇指令，`:` 則進入底部狀態列上的命令列模式，其餘按鍵在持有控制權時被放進 `a->ibuf` 批次送出。檔案對外提供 `cmd_keys()`、`cmd_exec()`、`log_toggle()` 三個函式（宣告在 `client/client.h`）。它依賴 `client.h` 的 `struct app`、`ui.c` 的 `ui_flash()`/`ui_status()`/`ui_help()`、`session.c` 的 `app_send()`/`app_flush_input()`，以及 `lib/util.c` 的 `bench_strlcpy()`/`mono_ms()`。本檔案沒有 `--unsafe=<switch>` 分支，所有函式在 safe 與 unsafe 模式下行為相同。

### `log_toggle`
*位置：`client/cmd.c:16`　簽章：`void log_toggle(struct app *a, const char *file)`*

- **用途**：切換「把 UART 資料錄到本機檔案」的功能。已在錄且沒給檔名時停止錄製；否則（重新）開始錄製到指定檔名，沒指定檔名則用時間戳命名的預設檔。
- **設計**：開關與「開新檔」共用同一個函式，因此 `Ctrl-] l`（`escape_key` 以 `NULL` 呼叫）、`:log`（`arg` 為空字串）與 `:log <file>` 三種入口走同一條路徑，狀態只放在 `a->logf` 與 `a->logname`。判斷順序刻意是先處理「停止」：只有 `a->logf` 非 NULL 且 `file` 為 NULL 或空字串時才停止並 `return`，這樣單純的 `:log` 能當開關用。若已在錄、又給了新檔名，則先 `fclose()` 舊檔再開新檔（等於切換檔案，不會顯示 "recording stopped"）。檔案以 `"ab"` 開啟（附加、二進位），所以對同一檔名重複開始不會覆蓋已存在的內容，並且寫入的是原始位元組、不做換行轉換。實際寫入資料不在這裡，而是 `session.c` 在處理 DATA 封包時對 `a->logf` 做 `fwrite()`（`client/session.c:246-247`）；`ui.c` 的 `ui_status()` 在 `a->logf` 非 NULL 時於旗標加上 `REC`。
- **細節**：
  - 沒給檔名時用 `time(NULL)` + `localtime_r()` + `strftime()` 產生 `bench-%Y%m%d-%H%M%S.log`（本地時區，相對於目前工作目錄）。同一秒內重新開始會得到同一檔名，因為是附加模式，內容會接在後面。
  - 指定的檔名用 `bench_strlcpy()` 複製進 `a->logname`（大小 256），超長會被截斷，不會溢位。
  - `fopen()` 失敗時 `a->logf` 維持 NULL，以 `FL_ERR` 閃現 `cannot open <name>: <strerror(errno)>`；`a->logname` 此時保存的是嘗試開啟的名稱。成功時以 `FL_OK` 閃現 `recording UART data to <name>`。
  - 停止時閃現的檔名來自 `a->logname`，所以顯示的是剛剛錄製的那個檔。
  - 沒有 `fflush()`：資料由 stdio 緩衝，正常結束時 `app_run()` 結尾（`client/session.c:429-430`）會 `fclose()`。
  - 無 `--unsafe` 分支。
- **關係**：被誰呼叫 → `cmd_exec()`（`:log`）、`escape_key()`（`Ctrl-] l`）；呼叫 → `bench_strlcpy()`（`lib/util.c`）、`ui_flash()`（`client/ui.c`），以及 libc 的 `fopen`/`fclose`/`strftime`。

### `send_simple`
*位置：`client/cmd.c:43`　簽章：`static void send_simple(struct app *a, uint8_t type)`*

- **用途**：送出一個沒有 payload 的請求封包（`BN_TAKE`、`BN_RELEASE`、`BN_INFO_REQ`），但只在連線處於 `ST_LIVE` 時才送。
- **設計**：這三個指令的封包都是空 payload，只差封包型別，所以抽成一個 helper；`ST_LIVE` 檢查集中在這裡，避免各指令分別判斷。只在 `ST_LIVE`（已收到 WELCOME、握手完成）才送是因為握手前或斷線時送出沒有意義：`ST_DISCONNECTED` 時 `app_send()` 本身就會回 -1，而 `ST_HANDSHAKE` 時尚未收到 WELCOME（`a->st` 在 `client/session.c:205` 才設為 `ST_LIVE`），此時 TAKE/RELEASE/INFO 還沒有意義，所以直接在本地提示使用者。
- **細節**：
  - 非 `ST_LIVE` 時以 `FL_WARN` 閃現 `not connected` 後直接返回，不送任何東西。
  - 沒有回傳值；`app_send()` 若送出失敗（`conn_send()` 回 < 0）會自行呼叫 `app_disconnect()`（預設排程退避重連；`--no-reconnect` 或 `--unsafe=reconnect` 時改為設 `a->quit = 2` 放棄），這裡不處理。
  - 實際結果（是否取得控制權等）是非同步的：伺服器用 `BN_CTRL_RESULT`/`BN_STATUS`/`BN_ERROR` 回應，由 `session.c` 的 `handle_packet()` 更新 `a->is_ctrl` 並閃現訊息。
- **關係**：被誰呼叫 → `cmd_exec()` 的 `take`、`release`、`info` 三個分支；呼叫 → `app_send()`（`client/session.c`）、`ui_flash()`。

### `cmd_exec`
*位置：`client/cmd.c:52`　簽章：`void cmd_exec(struct app *a, const char *line)`*

- **用途**：解析並執行一行指令（不含開頭的 `:` 也可以），指令包括 `take`、`release`、`info`、`ts`、`hex`、`log [file]`、`baud <rate>`、`help`/`?`、`quit`/`q`。是「`:` 命令列」與「`Ctrl-]` 單鍵捷徑」共用的執行入口。
- **設計**：單鍵捷徑（`escape_key`）不自己實作功能，而是組出指令字串再呼叫 `cmd_exec()`，讓兩種輸入方式的行為保證一致，邏輯只有一份。解析手法是：略過開頭的空白與 `:`，用 `strcspn(line, " ")` 取出第一個詞複製到 32 位元組的 `word`，其後略過空白剩下的即為 `arg`；之後用 `strcmp` 逐一比對，沒有用函式表，因為指令只有九個（外加 `?`、`q` 兩個別名），直接的 if/else 鏈最易讀。權限檢查放在客戶端只是為了立即給使用者回饋：真正的授權在伺服器（`server/session.c` 對 `BN_BAUD` 會檢查 `ctrl_pid` 並回 `BN_ERR_NOT_CONTROLLER`），所以客戶端的 `is_ctrl` 檢查是便利性而非安全邊界。
- **細節**：
  - 詞長度超過 31 時被截斷成 31 個字元（`wl = sizeof word - 1`），因此超長的詞會落到「unknown command」分支；空詞（空行或只有 `:`/空白）直接 `return`，不顯示錯誤。分隔符只認空格 `' '`。
  - `take`：只呼叫 `send_simple(BN_TAKE)`；後面的 `arg` 被忽略。
  - `release`：若 `!a->is_ctrl` 先以 `FL_INFO` 閃現 `you do not hold control`，但仍會繼續往下做。接著無條件呼叫 `app_flush_input()`，再送 `BN_RELEASE`。先 flush 是為了讓還在批次緩衝區（`ibuf`，最多等 5 ms）的按鍵先於 RELEASE 上線，同一條 TCP 連線上順序固定，伺服器端 child 再透過 input pipe 轉給 reader，由 reader 依序處理（見 DECISIONS 的 RELEASE 與 INPUT 順序）；伺服器收到 RELEASE 時才檢查 controller 身分並轉給 reader（非 controller 的 RELEASE 被靜默忽略），因此先送出的 INPUT 不會被這個 RELEASE 搶先。非 `ST_LIVE` 時 `send_simple()` 會閃現 `not connected`（覆蓋前一個提示）。
  - `info`：先設 `a->info_pending = 1` 再送 `BN_INFO_REQ`。`session.c` 收到 `BN_INFO` 時只有 `info_pending` 為真才顯示資訊框並清除旗標，所以不是自己要求的 INFO 封包不會彈出方塊。注意即使 `send_simple()` 因未連線而沒送，旗標仍被設為 1。
  - `ts`、`hex`：只翻轉 `a->ts_mode`/`a->hex_mode` 並以 `FL_INFO` 提示 on/off，渲染由 `ui.c` 完成。
  - `log`：把 `arg` 交給 `log_toggle()`；`arg` 為空字串時就是開關行為。
  - `baud`：用 `strtoul(arg, &end, 10)` 解析十進位；`arg` 為空、後面有非數字字元（`*end`，包含尾端多餘的空白）或值為 0 時，以 `FL_ERR` 顯示 `usage: :baud <rate>`。通過後才檢查 `!a->is_ctrl`（`FL_WARN` 提示先按 `Ctrl-] t`）；通過且 `a->st == ST_LIVE` 時，把值轉成 `uint32_t` 並用 `htobe32()` 轉大端，以 `app_send(BN_BAUD, &nb, 4)` 送出，payload 長度剛好 4，符合伺服器端 `h.len != sizeof nb` 的檢查。`strtoul` 的結果直接強轉 `uint32_t`，過大的值會被截斷、前置負號會被 `strtoul` 轉成很大的無號數再截斷；客戶端不檢查範圍；若通過前兩項檢查但 `a->st != ST_LIVE`，則什麼都不送也不提示。是否為支援的鮑率由伺服器 `baud_supported()` 判斷，不支援時回 `BN_ERR_BAD_REQUEST`，客戶端以 `server: ...` 的 `FL_ERR` 閃現（`session.c` 的 `BN_ERROR` 分支）。改變後的鮑率透過之後的 `BN_STATUS` 更新到 `a->baud`，這裡不自行修改。
  - `help`/`?`：呼叫 `ui_help()`；`quit`/`q`：只設 `a->quit = 1`，由 `app_run()` 的 `while (!a->quit)` 離開迴圈後（若 `a->st != ST_DISCONNECTED`）做 `app_flush_input()`、`conn_bye()`（BYE、半關閉、drain，見 DECISIONS 的 Client 一節），關閉 `a->logf`，再 `term_restore()`，因此 quit 為 1 時 `app_run()` 回傳 0（`quit == 2` 才回 1）。
  - 其他詞：`FL_ERR` 閃現 `unknown command '<word>' — :help`。
  - 沒有使用到任何 `--unsafe` 開關。
- **關係**：被誰呼叫 → `cmdline_key()`（按 Enter 時）、`escape_key()`（單鍵捷徑 `t r i s h ?` 以字串轉呼叫）；呼叫 → `send_simple()`、`log_toggle()`、`app_flush_input()`、`app_send()`、`ui_flash()`、`ui_help()`。

### `escape_key`
*位置：`client/cmd.c:107`　簽章：`static void escape_key(struct app *a, uint8_t k)`*

- **用途**：處理按下 `Ctrl-]` 之後的下一個鍵 `k`，把它對應到動作：`t` take、`r` release、`i` info、`s` 時間戳、`h` hex、`l` 錄製、`?` 說明、`q` 離開、`:` 進入命令列、再按一次 `Ctrl-]` 送出字面上的 0x1D。
- **設計**：用 `switch` 一個字元對一個動作；大部分直接轉成字串呼叫 `cmd_exec()`，讓單鍵與冒號指令共用實作（`l` 例外，直接呼叫 `log_toggle(a, NULL)`，`q` 直接設 `a->quit = 1`，因為這兩個不需要字串解析）。字面 `Ctrl-]` 的存在是因為跳脫鍵本身被攔截了，若使用者真的要把 0x1D 送到序列埠，只能用「連按兩次」這個慣例來逃逸。按鍵比對是區分大小寫的，只認小寫字母。
- **細節**：
  - `:`：設 `a->cmd_mode = 1`、`a->cmd_len = 0`，並呼叫 `ui_status()` 立即把狀態列改畫成命令列（`ui.c` 的 `ui_status()` 在 `cmd_mode` 時顯示 `:` 加目前輸入）。注意不清 `a->cmd` 內容，因為長度歸零已足夠。
  - `ESC_KEY` 分支：只有 `a->is_ctrl && a->st == ST_LIVE` 且 `a->ilen < sizeof a->ibuf`（64）時才把 0x1D 追加到 `a->ibuf`；否則靜默丟棄（唯讀者按它不會有任何提示，緩衝區已滿也不 flush）。此分支不設定 `a->iflush_at`（正常輸入路徑在 `ilen == 0` 時才設定），所以實際送出時間取決於 `iflush_at` 當時的舊值：若緩衝區原本就有資料，沿用該批次的 deadline；若緩衝區原本是空的，舊值（`calloc` 後的 0，或之前某次批次的 deadline）通常已經過去，`app_run()` 在同一輪迴圈尾端（`client/session.c:406`）的 `a->ilen && now >= a->iflush_at` 檢查就會 `app_flush_input()`，因此 0x1D 會被很快送出而不是等 5 ms。另外，追加後 `ilen` 可能剛好等於 64 而不 flush；若同一批 `cmd_keys()` 中緊接著還有一般按鍵，`cmd_keys()` 的一般輸入路徑會在 `ilen == 64` 時執行 `a->ibuf[a->ilen++]`，寫到 `ibuf[64]`（超出陣列一個位元組；此為依程式碼推得的邊界情形，未實際執行驗證）。
  - 未知的鍵：以 `FL_WARN` 閃現 `unknown key after Ctrl-] — Ctrl-] ? for help`；該鍵被吃掉，不會送往 UART。
  - 無論如何，呼叫端 `cmd_keys()` 在進入本函式前已先清掉 `a->esc_pending`，所以跳脫狀態只維持一個鍵。
- **關係**：被誰呼叫 → `cmd_keys()`；呼叫 → `cmd_exec()`、`log_toggle()`、`ui_status()`。

### `cmdline_key`
*位置：`client/cmd.c:134`　簽章：`static void cmdline_key(struct app *a, uint8_t k)`*

- **用途**：在命令列模式（`a->cmd_mode` 為真）下處理單一個按鍵：編輯 `a->cmd` 緩衝區、Enter 執行、取消。
- **設計**：命令列是一個固定大小 `char cmd[128]` 加長度 `cmd_len` 的簡單行編輯器，不需要游標移動，所以只支援在尾端輸入與刪除，足以應付 `:baud 9600`、`:log file` 這類短指令。每個按鍵處理完最後都呼叫 `ui_status()` 重畫狀態列，讓使用者即時看到輸入。因為按鍵是逐個到達的（`cmd_keys()` 迴圈），不用考慮一次到達多個位元組的狀態拆分問題。
- **細節**：
  - `\r` 或 `\n`（Enter）：先清 `cmd_mode`、在 `cmd[cmd_len]` 補上 `'\0'`、`ui_status()` 還原狀態列，然後才 `cmd_exec(a, a->cmd)`。必須先清 `cmd_mode` 再執行，原因是 `ui_flash()` 內部呼叫 `ui_status()`（`client/ui.c:384`），而 `ui_status()` 在 `cmd_mode` 為真時只畫命令列就 `return`（`client/ui.c:394-400`）；若順序顛倒，指令產生的 `FL_ERR`/`FL_OK` 訊息雖已寫入 `a->flash`，卻不會被畫出來，使用者看不到 `unknown command` 或 `usage` 提示。
  - `0x1b`（Esc）或 `0x03`（Ctrl-C）：取消，只清 `cmd_mode`（緩衝區內容留著但下次進入時 `cmd_len` 會歸零）。
  - `0x7f` 或 `0x08`（Backspace）：`cmd_len > 0` 時減一；已經是空的則離開命令列模式，與 vi/shell 的習慣一致。
  - `0x15`（Ctrl-U）：清空整行（`cmd_len = 0`）。
  - 可顯示 ASCII（`0x20` 到 `0x7e`）且 `cmd_len < sizeof a->cmd - 1`（即最多 127 個字元，保留結尾 NUL 的位置）時追加；超過長度的字元被靜默丟棄。其他控制字元（包括 `Ctrl-]`）與非 ASCII 位元組（>= 0x80，例如 UTF-8 的多位元組字元）都被忽略，不會進到緩衝區，也不會送給 UART。
  - 在命令列模式中，所有按鍵都不會送往序列埠，也不需要持有控制權。
  - 離開函式前一律 `ui_status()`；在 Enter 分支這是第二次重畫（執行完指令後讓狀態列反映最新狀態）。
- **關係**：被誰呼叫 → `cmd_keys()`（`a->cmd_mode` 為真時）；呼叫 → `cmd_exec()`、`ui_status()`。

### `cmd_keys`
*位置：`client/cmd.c:156`　簽章：`void cmd_keys(struct app *a, const uint8_t *k, size_t n)`*

- **用途**：處理從 stdin 讀到的一批按鍵位元組：依序判斷是在命令列模式、跳脫鍵之後、跳脫鍵本身，還是一般輸入；一般輸入在持有控制權時被累積到 `a->ibuf` 準備送往 UART，否則顯示提示並丟棄。
- **設計**：這是一個以 `struct app` 欄位為狀態的小型狀態機（`cmd_mode`、`esc_pending`），每個位元組逐一判斷，因此即使一次 `read()` 回來多個位元組（貼上、或 `Ctrl-]` 與下一個鍵在同一批），或跳脫鍵與其後的鍵被拆在兩次 `read()`，結果都相同。判斷優先順序是：命令列模式 > 等待指令鍵 > `Ctrl-]` > 一般字元，這個順序保證命令列中輸入的字元不會被當成跳脫鍵。一般字元採批次送出而不是每鍵一個封包：第一個字元進入空的緩衝區時把 `a->iflush_at` 設為 `mono_ms() + 5`，`app_run()` 的 `poll()` 逾時會以此為上限，到時呼叫 `app_flush_input()`，所以連續快速的輸入（貼上）合併成一個 `BN_INPUT`，減少小封包數量，而單鍵最多只延遲 5 ms；緩衝滿 64 位元組（`sizeof a->ibuf`）就立即 flush，避免超過緩衝區。5 ms 與 64 位元組的上限與 `client/session.c` 的說明一致。
- **細節**：
  - 不在 `ST_LIVE` 時（斷線或握手中）：每個按鍵以 `FL_WARN` 提示 `not connected — key not sent` 並丟棄；`Ctrl-]` 與命令列在斷線時仍可使用（例如能 `:quit` 或 `:help`）。
  - 非控制者（`!a->is_ctrl`）：每個按鍵以 `FL_WARN` 提示 `read-only — Ctrl-] t 取得控制`（提示文字含中文）並丟棄，不會送到伺服器。這是客戶端的善意提示；伺服器端對非 controller 的 `BN_INPUT` 也會回 `BN_ERR_NOT_CONTROLLER`（`server/session.c` 的 `BN_INPUT` 分支），所以這個檢查不是唯一防線。
  - `ilen == 0` 才設 `iflush_at`：後續字元不會延後 deadline，所以 5 ms 是從批次中第一個字元起算的上限，而不是每鍵重新計時。
  - 緩衝區滿時 `app_flush_input()` 會送出 `BN_INPUT` 並把 `ilen` 歸零；若連線不是 `ST_LIVE` 則僅清空。
  - 若批次中途控制權被收回（`BN_STATUS` 使 `is_ctrl` 變 0），`session.c` 會把 `ilen` 清零，尚未送出的字元被丟掉，不會在失去控制權後還送出。
  - 此函式沒有回傳值，也不 `read()` stdin；讀取由 `app_run()` 完成（`client/session.c:382` 以 256 位元組的緩衝區 `read()`，`client/session.c:385` 呼叫本函式），因此單次 `n` 最多 256。
  - 與 `--unsafe` 開關無關。
- **關係**：被誰呼叫 → `app_run()`（`client/session.c`，互動模式主迴圈）；呼叫 → `cmdline_key()`、`escape_key()`、`app_flush_input()`（`client/session.c`）、`ui_flash()`、`mono_ms()`（`lib/util.c`）。

---

## `client/subcmd.c`

`client/subcmd.c` 實作 `bench` 客戶端的四個非互動子命令：`info`、`tail`、`watch`、`send`，供 shell 腳本與壓力測試使用（`scripts/stress.sh` 用 `bench watch`，`scripts/lib.sh` 的 `info()` 用 `bench info --raw`，`scripts/capture.sh` 用 tail/info/send）。這份程式碼跑在「bench client」行程：`client/main.c` 解析完選項後依子命令名稱直接呼叫 `sub_info` / `sub_tail` / `sub_watch` / `sub_send`（`client/main.c:106-113`），不進入 `app_run` 的 TUI，也不進 raw mode，更不重連。檔案只有四個對外函式（宣告於 `client/client.h`）與五個 static 輔助函式；連線、收發、BYE 都借用 `client/session.c` 的 `conn_*`（`conn_connect`、`conn_wait_welcome`、`conn_recv`、`conn_send`、`conn_bye`、`conn_close`），封包編解碼與 CRC 驗證在 libbench 的 `bn_send` / `bn_recv`，因此 `--unsafe=readn`、`--unsafe=validate` 對這些子命令同樣有效（見 `lib/proto.c:77`）。所有子命令共用一個檔案層級的 `static uint8_t buf[BN_MAX_PAYLOAD]`（64 KiB）接收緩衝區與一個 `out_bol` 旗標；因為單行程單執行緒，共用靜態變數沒有競爭問題，但也代表「解析封包 payload 必須在下一次 `recv_timeout` 之前完成」。結束碼約定：`info`、`watch`、`send` 在連線失敗或逾時等錯誤時回 1（`tail` 只有連線失敗才回 1，等不到封包仍回 0）；`send` 在控制權被拒絕或 TAKE 無回應時回 2；`main.c` 在 `send` 缺少文字參數時直接回 1（`client/main.c:109-112`）。

### `out_data`
*位置：`client/subcmd.c:23`　簽章：`static void out_data(const uint8_t *d, size_t n)`*

- **用途**：把一段 UART 位元組寫到 stdout，並記住「最後寫出的位元組是不是換行」。
- **設計**：`print_event` 要在事件分隔線前決定是否先補一個換行，否則分隔線會接在尚未結束的 UART 行後面。這個資訊只有寫資料的人知道，所以用檔案層級的 `out_bol`（beginning-of-line，初始 1）記錄，並集中在這一個函式更新，避免各子命令各自維護。只用 `fwrite` 而不 `fflush`，讓 stdio 自己緩衝；需要及時輸出的呼叫端自行 `fflush`（`sub_watch` 在每個收到的封包處理完後 `fflush(stdout)`，`sub_tail` 在迴圈結束後 `fflush` 一次）。
- **細節**：
  - `n == 0` 直接返回，不改動 `out_bol`（否則會讀 `d[-1]`）。
  - `out_bol = d[n - 1] == '\n'`：只看最後一個位元組，與 UART 慣用 `\r\n` 相容（結尾是 `\n`）；若資料以 `\r` 結尾則視為尚未換行。
  - 沒有檢查 `fwrite` 的回傳值；stdout 被關閉或管線破裂時（SIGPIPE 已由 `main.c` 依 R2 設為忽略）錯誤被靜默吞掉。
  - 注意 `sub_send` 的等待階段與 `sub_info` 不經過這個函式，直接 `fwrite`，所以不影響 `out_bol`。
- **關係**：被誰呼叫 → `sub_tail`（`client/subcmd.c:147`）、`sub_watch`（`client/subcmd.c:212`）；呼叫 → libc `fwrite`。

### `open_session`
*位置：`client/subcmd.c:31`　簽章：`static int open_session(struct conn *c, struct client_opts *o, uint32_t replay, struct bn_welcome *w)`*

- **用途**：四個子命令共用的「連線 + HELLO + 等 WELCOME」流程；成功回 0，失敗回 -1 並已印出錯誤、已關閉 socket。
- **設計**：先 `memset(c, 0, ...)` 再把 `c->fd` 設為 -1。`memset` 讓 `have_pos` / `have_start` 為 0，所以 `conn_connect` 組出的 HELLO 不帶 resume 資訊，伺服器一定走「依 `replay_bytes` 重播歷史」的路徑，而非續傳（續傳是互動模式 R12 的機制，子命令是一次性連線不需要）。fd 設成 -1 是因為 0 是合法的 fd（stdin），`conn_close` 以 `fd >= 0` 判斷是否已開啟，初始化為 -1 才不會誤關 stdin。連線逾時 3000 ms、等 WELCOME 5000 ms 是寫死的：子命令要「快速失敗」讓腳本可判斷，不像互動模式有重連退避。
- **細節**：
  - `conn_connect` 失敗時印 `bench: cannot connect to host:port: <strerror(errno)>`，回 -1（`errno` 由 `tcp_connect_timeout` 設定，例如 `getaddrinfo` 失敗為 `EHOSTUNREACH`、連線逾時為 `ETIMEDOUT`，或 `connect` 回報的 `SO_ERROR`；若失敗發生在送 HELLO 的 `bn_send`，則是它留下的 `errno`）。
  - `conn_wait_welcome` 回傳值三種：0 成功；-1 逾時/封包不對，這裡印 `bench: no WELCOME from server`；-2 表示伺服器回了 ERROR 封包（例如 `SERVER_BUSY`，R9），錯誤文字已由 `conn_wait_welcome` 自己印到 stderr，所以這裡只在 `rc == -1` 時補印訊息，避免重複。兩種失敗都先 `conn_close(c)` 再回 -1（不送 BYE，因為還沒有成功的會話）。
  - `replay` 以 `uint32_t` 傳入，對應 HELLO 的 `replay_bytes`；`main.c` 已限制 `-n` 不超過 0xFFFFFFFF。
  - `o->name` 會被 `bench_strlcpy` 放進 HELLO 的 32 位元組 `name` 欄位，成為伺服器日誌（`client <peer> (<name>) joined`）與控制權持有者名稱所用的客戶端名稱（`watch --name botNN`）。
  - `w` 取得 WELCOME 內容，但四個呼叫端都沒有再使用它（只是 `conn_wait_welcome` 需要輸出參數）。
- **關係**：被誰呼叫 → `sub_info`（`client/subcmd.c:92`）、`sub_tail`（138）、`sub_watch`（163）、`sub_send`（249）；呼叫 → `conn_connect`、`conn_wait_welcome`、`conn_close`（皆在 `client/session.c`）。

### `recv_timeout`
*位置：`client/subcmd.c:50`　簽章：`static int recv_timeout(struct conn *c, struct bn_hdr *h, int ms)`*

- **用途**：最多等 `ms` 毫秒收一個完整封包，封包 payload 放進檔案層級的 `buf`。回傳 1 = 收到一個封包、0 = 逾時、-1 = 連線關閉或錯誤。
- **設計**：`conn_recv` 底層是阻塞式 `readn`（`--unsafe=readn` 時為單次 `recv`），沒有逾時，所以先用 `poll` 等 socket 可讀，才讓子命令的主迴圈有機會檢查 deadline、送 PING。`poll` 有訊號只代表「第一個位元組到了」；之後 `conn_recv` 會阻塞到整個封包讀完（客戶端沒有設定 `SO_RCVTIMEO`；`set_io_timeouts` 只有 `server/session.c` 呼叫），這在伺服器一次 `write()` 送出整個封包（`bn_send` 的設計）時不是問題。
- **細節**：
  - `poll` 回 0 → 回 0（逾時）。`poll` 回 -1 時，`EINTR` 當作逾時回 0（由呼叫端的迴圈自然重試），其他 errno 回 -1。
  - 呼叫 `conn_recv` 的 `perr` 區域變數只是為了滿足介面，內容不被讀取；`bn_recv` 回 0（EOF）、-1（I/O 錯誤）、-2（BAD_MAGIC/VERSION/LENGTH/CRC）全部被折成 -1，所以呼叫端無法分辨「伺服器關閉」與「協定錯誤」（`sub_watch` 因此一律印 "connection closed by server"）。
  - 只有回傳 1 時 `conn_recv` 才會更新 `c->last_rx_ms`，`sub_watch` 的 15 秒無資料偵測就靠它。
  - 注意 `sub_tail` 把 0 和 -1 一起當作結束（`rc <= 0`），所以 `EINTR` 也會讓 tail 提早結束；但此程式沒有安裝任何會中斷 `poll` 的訊號處理器。
  - `--unsafe=readn`（單次 `recv`）與 `--unsafe=validate`（R3，不檢查 magic、version、長度與 CRC）由 `bn_recv` 處理；`buf` 大小等於 `BN_MAX_PAYLOAD`，合法長度的封包一定放得下。
- **關係**：被誰呼叫 → `sub_info`（`client/subcmd.c:98`）、`sub_tail`（143）、`sub_watch`（188）、`sub_send`（256、291）；呼叫 → `poll`、`conn_recv`（`client/session.c:53`，再轉到 `bn_recv` `lib/proto.c:77`）。

### `print_error_pkt`
*位置：`client/subcmd.c:63`　簽章：`static void print_error_pkt(const struct bn_hdr *h)`*

- **用途**：把剛收到的 `BN_ERROR` 封包（在 `buf` 裡）以 `bench: server error <NAME>: <text>` 印到 stderr。
- **設計**：ERROR payload 是 `struct bn_error_hdr`（2 位元組 code + 2 位元組 pad）後接不含結尾 NUL 的文字，所以用 `%.*s` 配合 `h->len - sizeof e` 限定長度，不依賴 NUL。先 `memcpy` 到區域變數 `e` 再 `be16toh`，而不是把 `buf` 直接轉型成指標去讀 packed 的線上格式。
- **細節**：
  - `h->len < sizeof e` 時靜默返回（畸形封包不報錯）。
  - 讀的是全域 `buf`，所以必須在 `recv_timeout` 之後、下一次收包之前呼叫。
  - 輸出的文字不是以 NUL 結尾的字串，而是 `%.*s` 限長輸出，內容原樣印出，不做過濾。
  - `bn_err_name` 把 code 轉成字串（`BAD_LENGTH`、`NOT_CONTROLLER`、`DEVICE_LOST`…）；此函式只印訊息，不改變結束碼，結束碼由呼叫端決定（`sub_send` 設 status = 1，`sub_watch` 不改）。
- **關係**：被誰呼叫 → `sub_watch`（`client/subcmd.c:229`）、`sub_send`（297）；呼叫 → `bn_err_name`（`lib/proto.c:164`）。

### `print_event`
*位置：`client/subcmd.c:73`　簽章：`static void print_event(FILE *f, const struct bn_hdr *h, int color)`*

- **用途**：把 `BN_EVENT` 封包畫成一行分隔線 `──── <事件文字> HH:MM:SS ────`，使 UART 資料中穿插的事件（控制權取得/釋放、裝置遺失、DROPPED…）在文字輸出裡可見。
- **設計**：事件與資料在伺服器的環形緩衝裡是同一條有序串流（見 README 架構），客戶端依序處理就能讓分隔線出現在正確的位置。為了不讓分隔線黏在沒結束的一行後面，先看 `out_bol`：若上一個寫到 stdout 的位元組不是換行，就先補 `\n`，輸出後把 `out_bol` 設為 1。`color` 為真時用 `\033[2m`（dim）包住並以 `\033[0m` 還原，只在 stdout 是 tty 時使用，導向檔案時不會混入跳脫碼。
- **細節**：
  - 事件文字在 `struct bn_event_hdr`（offset 8 + ts_ns 8 + code 2 + pad 2 = 20 位元組）之後，長度 `h->len - sizeof e`，同樣用 `%.*s`；時間用 `fmt_hms(be64toh(e.ts_ns), ...)` 轉成本機時區的 `HH:MM:SS`（伺服器時間戳）。
  - `h->len < sizeof e` 時靜默返回。
  - `f` 參數形式上可換檔案，但 `out_bol` 追蹤的是 stdout，現有呼叫端也都傳 `stdout`；事件只印到 stdout，不寫入 `sub_watch` 的 `--out` 檔，所以 `--out` 檔是純 UART 位元組。
  - 讀取全域 `buf`，需在收包後立即呼叫。
- **關係**：被誰呼叫 → `sub_tail`（`client/subcmd.c:149`）、`sub_watch`（227）；呼叫 → `fmt_hms`（`lib/util.c:48`）。

### `sub_info`
*位置：`client/subcmd.c:88`　簽章：`int sub_info(struct client_opts *o)`*

- **用途**：實作 `bench info`：連線、送 `BN_INFO_REQ`、等 `BN_INFO` 回覆，輸出系統資訊後結束。stdout 是 tty 且沒有 `--raw` 時畫成方框，否則原樣輸出 `key=value\n` 文字。回 0 成功，1 失敗。
- **設計**：腳本需要穩定可解析的格式（`scripts/lib.sh` 的 `info_key` 用 `sed` 取 `key=value`），人類在終端機則要好讀，所以依 `isatty(STDOUT_FILENO)` 與 `--raw` 切換。方框寬度要依「顯示欄寬」而不是位元組數計算：標題與內容可能含 UTF-8/CJK，所以用 `str_width`（CJK 算 2 欄），並以 `%*s` 對 `inner - str_width(line)` 補空白，使右側 `│` 對齊。`inner` 初值為 `str_width(title) + 3`（保證標題列至少有橫線），再取所有內容行的最大欄寬。外框總寬為 `inner + 4`：上緣 `┌─ ` + 標題 + 空白 + 橫線 + `┐`，內容列 `│ ` + 內容 + 補白 + ` │`，下緣 `└` + `inner+2` 條橫線 + `┘`，三者等寬。
  成功後用 `conn_bye`（BYE + `shutdown(SHUT_WR)` + 排空直到 EOF，最多等 1000 ms，見 `client/session.c:68`），讓 TCP 以乾淨的 FIN 結束而非 RST（DECISIONS 的 Client 一節）；`captures/rst.pcap` 對照的就是這個差別。
- **細節**：
  - `open_session(..., replay = 0, ...)`：不要歷史，但伺服器仍會送 REPLAY_BEGIN / REPLAY_END / STATUS 等封包；迴圈用 `h.type != BN_INFO` 跳過它們。
  - 總 deadline 5000 ms（`mono_ms() + 5000`），每次 `recv_timeout` 500 ms；`rc < 0`（連線關閉）跳出迴圈，`rc == 0` 繼續等。
  - 失敗路徑（逾時或連線被關）印 `bench: no INFO reply`，用 `conn_close` 直接關閉（不送 BYE，因為連線可能已壞）並回 1。
  - 方框：`char lines[64][256]`（16 KiB 堆疊）傳給 `info_box_lines(buf, h.len, 80, lines, 64)`，最多 64 行，超出被截斷；寬度參數 80 實際被 `info_box_lines` 忽略。INFO payload 是 `sysinfo_build` 產生的 `key=value\n` 文字，長度以 `bn_hdr.len` 為準，不含額外的結尾 NUL，所以一定帶長度。標題為 `System info: <host>:<port>`，`host` 以 `%.100s` 限長。
  - raw 模式直接 `fwrite(buf, 1, h.len, stdout)`，內容包含 `server.clients` 等鍵；`server.clients` 會把提出請求的這條連線也算進去（DECISIONS）。
  - 伺服器端的 `INFO_REQ` 處理在 `server/session.c` 的 `handle_packet`（回傳 `sysinfo_build` 的結果）。
- **關係**：被誰呼叫 → `main`（`client/main.c:106`）；呼叫 → `open_session`、`conn_send`、`recv_timeout`、`info_box_lines` 與 `str_width`（`client/ui.c:310`、`49`）、`conn_bye`、`conn_close`。

### `sub_tail`
*位置：`client/subcmd.c:133`　簽章：`int sub_tail(struct client_opts *o)`*

- **用途**：實作 `bench tail`：請求伺服器重播最近的歷史（預設 16 KiB，`-n` 可改），印完就結束。
- **設計**：不另設協定訊息，直接利用 HELLO 的 `replay_bytes`：連線時要求重播 n 位元組，伺服器在 WELCOME 後送 REPLAY_BEGIN、若干 DATA/EVENT、最後 REPLAY_END。客戶端以 REPLAY_END 作為「歷史已結束」的終止條件，所以不需要 `--duration`，也不會變成即時串流。預設值要用 `o->replay_set` 判斷：`main.c` 把 `o->replay` 預設為 8 KiB（互動模式用），但 tail 的預設是 16 KiB，因此只有使用者明確下 `-n` 時才採用 `o->replay`（`-n 0` 也是明確指定，只得到空輸出）。
  DATA 與 EVENT 依到達順序輸出，使事件分隔線出現在與互動模式相同的位置；資料用 `out_data` 以維持 `out_bol`。
- **細節**：
  - 每個封包最多等 5000 ms；`recv_timeout` 回 0 或 -1 都結束迴圈（`rc <= 0`），也就是逾時或連線被關閉時 **不是錯誤**：仍 `fflush`、`conn_bye`、回 0。只有 `open_session` 失敗才回 1。
  - DATA 需 `h.len >= sizeof(struct bn_data_hdr)`（offset 8 + ts_ns 8 = 16 位元組），內容從 `buf + 16` 開始。
  - `color = isatty(STDOUT_FILENO)` 只影響 `print_event` 的 dim 跳脫碼。
  - 重播量可能被環形緩衝截短：要求的起點早於環形緩衝仍保留的 `tail` 時，伺服器從 `tail` 開始並在 REPLAY_BEGIN 填 `dropped_bytes`（`server/session.c:307`）；tail 子命令不顯示這個值（REPLAY_BEGIN 被忽略），只是輸出較少。
  - 其他封包型別（WELCOME 之後的 STATUS、REPLAY_BEGIN）被忽略。
- **關係**：被誰呼叫 → `main`（`client/main.c:107`）；呼叫 → `open_session`、`recv_timeout`、`out_data`、`print_event`、`conn_bye`。

### `sub_watch`
*位置：`client/subcmd.c:158`　簽章：`int sub_watch(struct client_opts *o)`*

- **用途**：實作 `bench watch`：以唯讀觀察者身分接收即時串流，可指定持續時間（`--duration` 秒）、名稱（`--name`）、把資料寫入檔案（`--out`），結束時在 stderr 印一行統計。壓力測試用它模擬 20 個同時的觀察者（`scripts/stress.sh`）。
- **設計**：
  - 只讀、不進 raw mode、不處理鍵盤，所以不需要 TUI；歷史重播量預設 0（`o->replay_set ? o->replay : 0`），只看連線後的新資料，`-n` 可改。
  - 計數器的意義：`records` 與 `bytes` 只算 DATA；`first_off` / `last_off` 是 DATA 與 EVENT 的 `offset`（協定定義為「該筆記錄結尾的邏輯偏移」，DECISIONS），讓壓力測試可比對各觀察者看到的偏移範圍；`dropped` 計 `EV_DROPPED` 事件次數（客戶端落後被覆寫時由伺服器注入，R10）。EVENT 也更新偏移，使只收到事件的串流也有合理的範圍。
  - 活性維持：伺服器子行程在握手後有 60 秒接收逾時（`IDLE_TIMEOUT_MS`，R5），安靜的觀察者若什麼都不送會被切掉，所以每 15 秒送一次 `BN_PING`（`last_ping` 計時）；伺服器回 PONG，客戶端的 `last_rx_ms` 因此更新。反過來客戶端自己也偵測伺服器是否靜默：在 `recv_timeout` 逾時（`rc == 0`）時若距上次收到封包超過 15000 ms，印 `no data from server for 15 s` 並以狀態 1 結束。
  - 主迴圈每次最多等 `min(剩餘時間, 1000)` ms，所以每秒至少醒一次來檢查結束時間與 PING 計時；`end` 在沒有 `--duration` 時為 `UINT64_MAX`（永遠執行，直到連線被關或被訊號殺掉；沒有訊號處理器，所以被 SIGINT 殺掉時不會送 BYE 也不會印統計）。
  - `--out` 檔每筆 DATA 都 `fflush`，使檔案內容隨時可被其他程序比對（stress 以 sha256 比較 20 份檔案）；只存原始 UART 位元組，不含事件分隔線。stdout 在每處理完一個封包後 `fflush`（逾時 `continue` 的那幾圈不會執行到），讓管線下游即時看到輸出。
- **細節**：
  - `--out` 檔在 `open_session` **之後**才開；`fopen` 失敗時印錯誤並回 1，但此路徑沒有呼叫 `conn_bye` / `conn_close`，連線留給行程結束時由核心關閉。
  - `rc < 0` → 印 `bench: connection closed by server`，status = 1（也涵蓋協定錯誤，見 `recv_timeout`）。
  - `BN_ERROR` 封包用 `print_error_pkt` 印出但不結束迴圈；伺服器因違規而斷線時，下一輪 `recv_timeout` 回 -1 才結束。STATUS、PONG、REPLAY_* 都被忽略。
  - DATA：`dl = h.len - sizeof d`，`bytes += dl`、`records++`，同時寫 stdout（經 `out_data`）與 `--out`。
  - 正常結束（到達 `--duration`）回 0；`conn_bye` 之後關閉輸出檔，最後印 `watch: name=… records=… bytes=… first_offset=… last_offset=… dropped_events=…` 到 stderr；沒收到任何偏移時 `first_offset` 印 0。統計用 `unsigned long long` 轉型以符合 `%llu`。
  - 在 `--unsafe=readn` / `validate` 下行為取決於 `bn_recv`：分段到達的封包可能被誤判而讓 watch 以 "connection closed by server" 結束。
- **關係**：被誰呼叫 → `main`（`client/main.c:108`）；呼叫 → `open_session`、`recv_timeout`、`conn_send`（PING）、`out_data`、`print_event`、`print_error_pkt`、`conn_bye`、`now_ns` / `mono_ms`（`lib/util.c`）。

### `sub_send`
*位置：`client/subcmd.c:243`　簽章：`int sub_send(struct client_opts *o, const char *text)`*

- **用途**：實作 `bench send`：把 `text`（支援 C 風格跳脫）送進 UART。流程為取得控制權 → 送 INPUT → 釋放控制權，可選擇 `--wait MS` 再讀回裝置回應。結束碼：0 成功，1 連線失敗或等待期間收到伺服器錯誤，2 控制權被拒絕或 TAKE 沒有回應。
- **設計**：
  - `unescape(text, data, sizeof data)` 在連線之前就完成（`\n \r \t \\ \xHH \e \0`），讓 `bench send host 'reset\n'` 能在 shell 引號內表示控制字元；`data` 是 4096 位元組的堆疊緩衝，超過的部分被靜默截斷，`n` 是跳脫後的實際長度（可含 NUL 位元組，所以不能用 `strlen`）。
  - 送出前必須 TAKE：伺服器只轉送控制者的 INPUT，其他人會得到 `NOT_CONTROLLER`。`BN_CTRL_RESULT` 的 `granted` 以 `granted` 變數三態記錄：-1 尚未回答、0 被拒、1 取得。
  - 被拒絕時 payload 帶有持有者名稱、閒置秒數與距離自動釋放的秒數，都印給使用者（`be32toh` 轉換）；`auto_release_in_sec == UINT32_MAX` 意為「不會自動釋放」，由伺服器的 `ctrl_auto_release_in`（`server/control.c:30`）在目前無持有者、`--unsafe=ctrl-release`（R6 關閉）或閒置釋放時間為 0 時回傳，這裡印 "no auto-release"。`holder` 欄位固定 32 位元組，欄位本身沒有保證結尾，所以複製到 33 位元組的區域陣列再印；伺服器端以 `bench_strlcpy` 填入 `holder`（最多 31 字元加 NUL，`server/control.c:60,65`），因此實務上有 NUL。注意 `bench_strlcpy` 內部先對來源做 `strlen`，若遇到不含 NUL 的 32 位元組欄位仍會越界讀取，這裡的 33 位元組緩衝並不能防住這種情況。
  - INPUT 依 `BN_MAX_INPUT`（256）切塊：伺服器對 `len > 256` 的 INPUT 視為違規（`BAD_LENGTH`，除非 `--unsafe=validate`），所以最多 4096/256 = 16 個封包。
  - 接著送 `BN_RELEASE`。伺服器把 INPUT 與 RELEASE 都透過同一條輸入管線交給 reader 依序處理（DECISIONS Server 一節），所以「TAKE, INPUT, RELEASE」不會讓輸入被丟掉；這也是為何釋放在客戶端 INPUT 之後立刻送，不需等待。
- **細節**：
  - 等待 TAKE 的結果最多 5000 ms，每次 `recv_timeout` 500 ms；期間收到的其他封包（REPLAY_END、STATUS、EVENT 等）被略過。連線在回答前被關閉（`rc < 0`）也會落到 "no answer to TAKE"，回傳 2。
  - 被拒（`granted == 0`）與無回答都走同一個出口：`conn_bye` 後回 2；此時不送任何 INPUT。
  - `text` 為空時 `n == 0`，不送 INPUT，但仍會 TAKE 再 RELEASE。
  - 若裝置已遺失，伺服器會對 INPUT 回 `DEVICE_LOST` 錯誤並丟棄輸入；沒有 `--wait` 時客戶端不會讀它，仍印 `sent N bytes` 並回 0。
  - `--wait MS`（`o->wait_ms`，0 表示不等）：RELEASE 後再讀最多 MS 毫秒，DATA 的內容直接 `fwrite` 到 stdout（用 `open_session` 的 replay = 0，所以只有送出之後的新資料；事件不印、不更新 `out_bol`），`BN_ERROR` 以 `print_error_pkt` 印出並使 status = 1。
  - 等待迴圈每圈重新計算 `end - mono_ms()` 當 `poll` 逾時；理論上若時鐘恰好在 while 判斷與這個減法之間越過 `end`，無號減法再轉 `int` 會變成負值，`poll` 就會無限等待。這個窗口只有微秒級，實際上等同於不會發生，但不是被程式碼保護的。
  - 最後 `conn_bye`，在 stderr 印 `bench: sent N bytes`（N 為跳脫後的位元組數，即使輸入實際沒有被裝置接受也如此）。
- **關係**：被誰呼叫 → `main`（`client/main.c:113`，`text` 為 `argv` 中子命令後的字串）；呼叫 → `unescape`（`lib/util.c:119`）、`open_session`、`conn_send`（TAKE / INPUT / RELEASE）、`recv_timeout`、`print_error_pkt`、`bench_strlcpy`、`conn_bye`；對應的伺服器端為 `server/session.c` `handle_packet` 的 `BN_TAKE` / `BN_INPUT` / `BN_RELEASE` 分支與 `server/control.c` `ctrl_take_locked`。

---

## `tools/benchsim.c`

`benchsim` 是一個獨立執行的工具程式（單一行程，不 fork、不用執行緒），用來在沒有實體開發板時假裝成一台序列裝置。它用 `posix_openpt()` 建立 pseudo terminal，把 slave 端以符號連結公開（預設 `/tmp/benchsim0`，`-L` 可改），再依 UART 的實際速率（每 byte 10 bit，預設 115200 baud）重播開機 log；log 中 `#delay <ms>` 行表示暫停。播放結束（或播放途中收到任何按鍵）後，它變成一個簡化的假 U-Boot 提示字元，支援 `version` / `printenv` / `reset` / `help`。`benchd` 的 reader 把 `/tmp/benchsim0` 當成一般 tty 開啟，腳本（透過 `scripts/lib.sh` 的 `start_sim` 啟動，呼叫者包括 `scripts/test.sh`、各 `demo_rN.sh`、`scripts/stress.sh`、`scripts/capture.sh`）用它取代真實硬體；殺掉 benchsim 會關閉 pty master，benchd 的 reader 因此讀到 EIO，這就是 R8（`dev-reopen`）「裝置被拔掉」的示範方式。它只依賴 libc 與 `libbench.so` 的 `bench_log.h`（`LOGx`、`bench_log_set_role`、`bench_log_init_level`）和 `bench_util.h`（`mono_ns`），不讀 `--unsafe` 開關。

整個程式是一個事件迴圈加幾個檔案層級的全域變數：`segs[]/nsegs`（解析後的 log 片段）、`outq`（待送出的位元組佇列）、`next_due_ns`（下一次允許寫出的時間）、`byte_ns`（每 byte 的線上時間）、`chunk_bytes`（每次 write 的大小）、`mode`（`M_PLAY` 播放 / `M_PAUSE` 迴圈間隔 / `M_PROMPT` 提示字元）、`seg_idx`、`delay_until_ns`、`line[256]`（提示字元模式下的輸入行）。`produce()` 把 log 填進佇列、`drain()` 按波特率節奏寫出、`handle_input()` 處理主機送來的按鍵，三者都由 `main()` 的迴圈驅動。

### `on_signal`
*位置：`tools/benchsim.c:145`　簽章：`static void on_signal(int sig)`*

- **用途**：SIGINT / SIGTERM / SIGHUP 的處理函式，只把訊號編號存進 `stop_sig`，讓主迴圈結束。
- **設計**：訊號處理函式裡只做一件事，就是對 `volatile sig_atomic_t` 賦值，這是 async-signal-safe 的最小作法；真正的清理（移除符號連結、記 log）留到主迴圈外由 `main()` 做，避免在 handler 內呼叫 `unlink()`/`LOGI()`（stdio 與 log 都不是 async-signal-safe）。
- **細節**：
  - `stop_sig` 宣告在 `tools/benchsim.c:128`，型別 `volatile sig_atomic_t`；`main()` 的 `while (!stop_sig)` 讀它，結束時以 `(int)stop_sig` 印出訊號編號。
  - 以 `sigaction()` 安裝且 `sa_flags` 為 0（沒有 `SA_RESTART`）；訊號到達時阻塞中的 `poll()` 以 EINTR 返回（`poll()` 本來就不會被 `SA_RESTART` 自動重啟），`main()` 沒有檢查 `poll()` 的回傳值就繞回迴圈，立刻重新檢查 `stop_sig`。若訊號剛好落在檢查與 `poll()` 之間，最久等一個 poll 逾時（上限 50 ms）才會發現。
  - SIGKILL 無法被攔截，此時符號連結會留在原地，下次啟動時由 `unlink(opt_link)` 清掉。
- **關係**：被誰呼叫 → 由核心在收到訊號時呼叫，於 `main()`（`tools/benchsim.c:450-453`）註冊；呼叫 → 無。

### `q_put`
*位置：`tools/benchsim.c:147`　簽章：`static void q_put(const char *p, size_t n)`*

- **用途**：把 `n` 個位元組附加到輸出佇列 `outq` 尾端，必要時擴充緩衝區。
- **設計**：`outq` 是一條簡單的動態位元組陣列（`outq`、`outq_len`、`outq_cap`），因為佇列只有「尾端附加、頭端消耗」兩種操作且資料量很小（一個 write chunk 加最多一行 log，或提示字元的回應），用 `realloc` 加 `memmove` 比環狀緩衝區簡單。容量不足時擴成 `(outq_len + n) * 2 + 1024`，倍增讓攤銷成本固定，`+ 1024` 避免從 0 開始反覆小幅擴充。
- **細節**：
  - `realloc` 失敗時 `perror("realloc")` 後 `exit(1)`；這是一個模擬工具，沒有可降級的路徑，所以直接結束（舊指標此時已無用，不會再使用）。
  - 呼叫端傳 `n == 0` 時只做一次 `memcpy` 長度 0 與加 0，無副作用。
  - 不做任何速率控制，節奏完全由 `drain()` 與 `next_due_ns` 負責。
- **關係**：被誰呼叫 → `q_str`、`produce`（排入 log 片段）、`handle_input`（只用在回顯單一可顯示位元組 `q_put((char *)&c, 1)`；退格序列與 CRLF 的回送是經由 `q_str`）；呼叫 → `realloc`、`memcpy`、`perror`、`exit`。

### `q_str`
*位置：`tools/benchsim.c:160`　簽章：`static void q_str(const char *s)`*

- **用途**：把以 NUL 結尾的字串排入輸出佇列，`q_put(s, strlen(s))` 的薄包裝。
- **設計**：提示字元與命令回應都是字面字串，用這個包裝讓呼叫端不必自己算長度。
- **細節**：不含結尾的 NUL；字串中的換行由呼叫端自己寫成 `\r\n`（模擬 UART 的 CRLF）。
- **關係**：被誰呼叫 → `prompt`、`run_command`、`handle_input`；呼叫 → `q_put`。

### `add_seg_text`
*位置：`tools/benchsim.c:162`　簽章：`static void add_seg_text(const char *p, size_t n)`*

- **用途**：在 `segs[]` 尾端新增一個「文字片段」，內容是 log 的一行，並在行尾補上 `\r\n`。
- **設計**：log 檔以 LF 儲存（DECISIONS.md 的 benchsim 一節：「lines are stored with LF because benchsim appends CRLF」），真實 U-Boot/Linux 從 UART 送出的是 CRLF，所以在這裡統一補 `\r\n`，使 benchd 與 bench 看到的位元組與真機一致。每行獨立成一個片段，是為了讓 `produce()` 能以「整行」為單位排入佇列，並讓 `#delay` 只會落在行與行之間。
- **細節**：
  - `segs` 以 `realloc((nsegs + 1) * sizeof *segs)` 逐一成長；文字緩衝區 `malloc(n + 2)`，前 `n` 個位元組是原文，之後是 `'\r'`、`'\n'`，`len = n + 2`。沒有 NUL 結尾，長度一律靠 `len`。
  - 空行（`n == 0`）會產生只含 `\r\n` 的片段，所以 log 內的空行會被忠實重現。
  - `is_delay` 設為 0；`delay_ms` 欄位不會被設定也不會被讀取。
  - `realloc` / `malloc` 的回傳值沒有檢查（失敗時會在下一行解參考 NULL）；工具的輸入是開發者自己的 log 檔，沒有針對記憶體耗盡做處理。
- **關係**：被誰呼叫 → `parse_script`；呼叫 → `realloc`、`malloc`、`memcpy`。

### `add_seg_delay`
*位置：`tools/benchsim.c:174`　簽章：`static void add_seg_delay(unsigned ms)`*

- **用途**：在 `segs[]` 尾端新增一個「延遲片段」，代表輸出要暫停 `ms` 毫秒。
- **設計**：把 `#delay` 也存成一個片段，而不是在解析時就換算成絕對時間，是因為真正的暫停起點要等「前面排入的文字都在模擬線路上送完」才算（見 `produce`），這只有在播放時才知道。
- **細節**：`is_delay = 1`、`delay_ms = ms`、`text = NULL`、`len = 0`。`text` 為 NULL 讓 `load_log()` 在放棄 placeholder 時對所有片段 `free(segs[i].text)` 是安全的（`free(NULL)` 無動作）。同樣沒有檢查 `realloc` 失敗。
- **關係**：被誰呼叫 → `parse_script`；呼叫 → `realloc`。

### `parse_script`
*位置：`tools/benchsim.c:185`　簽章：`static int parse_script(const char *buf, size_t len)`*

- **用途**：把一段 log 文字拆成逐行片段（文字或 `#delay`），回傳是否發現 `#placeholder` 行（1 是、0 否）。
- **設計**：以 `memchr` 找 `'\n'` 逐行掃描，不依賴 NUL 結尾，所以可以直接處理整塊檔案內容，也可處理內建字串。判斷順序是先看 `#placeholder`、再看 `#delay `、其餘一律當成文字，這樣檔案中不會有任何位元組被丟棄（連空行都保留）。偵測到 placeholder 立即 `return 1`，讓呼叫端可以整份捨棄、改用內建文字（`sim/onkyo_boot.log` 目前就是 placeholder，見 DECISIONS.md）。
- **細節**：
  - 行尾若是 `\r`（CRLF 檔案）會先減掉，所以 CRLF 與 LF 的 log 檔都能用，之後由 `add_seg_text` 統一補 `\r\n`。
  - `#placeholder` 要求行長至少 12 且以這 12 個字元開頭；`#delay ` 要求行長至少 7（含空格），數字用 `atoi(p + 7)` 取得。`#delay` 後沒有空格的行（例如 `#delay`）不符合，會被當成普通文字送出。
  - 最後一行沒有 `\n` 結尾也會被處理（`nl` 為 NULL 時行長取到 `end`）。檔案以 `\n` 結尾時，不會多出一個空的片段。
  - 發現 placeholder 前已加入的片段不會在這裡清掉；由 `load_log()` 負責釋放並把 `nsegs` 歸零。
  - 注意：從檔案載入時 `load_log()` 配置的緩衝區是 `malloc(sz + 1)`，最後那 1 個 byte 沒有被設為 NUL（未初始化），而 `atoi` 需要遇到非數字字元才會停；若檔案最後一行是沒有換行的 `#delay N`，`atoi` 會去讀這個未初始化的 byte（若恰為數字則可能繼續讀出緩衝區）。內建字串（`builtin_log` 有結尾 NUL）與以換行結尾的檔案不受影響。
- **關係**：被誰呼叫 → `load_log`（載入檔案內容與內建 `builtin_log` 各一次）；呼叫 → `add_seg_text`、`add_seg_delay`、`memchr`、`atoi`。

### `load_log`
*位置：`tools/benchsim.c:204`　簽章：`static void load_log(void)`*

- **用途**：決定要播放什麼：讀 `-l` 指定的檔案，若沒給、打不開、或內容仍是 placeholder，就改用內建的通用 U-Boot / Linux 開機文字 `builtin_log`。
- **設計**：任何失敗都降級而不是結束，因為這個工具的目的是讓示範永遠能跑起來；降級時用 `LOGW` 明確說出原因（沒給 `-l`、`cannot open ... (strerror)`、或「仍是 placeholder」），避免使用者誤以為播的是真實擷取。載入成功時 `LOGI` 印出片段數後直接 return。檔案以 `"rb"` 開、`fseek/ftell` 取得大小後一次 `fread` 進記憶體，因為 log 檔很小，一次讀完比邊讀邊解析簡單。
- **細節**：
  - 內建文字的解析用 `parse_script(builtin_log, sizeof builtin_log - 1)`，`- 1` 去掉字串結尾的 NUL。
  - 發現 placeholder 時，先 `free(segs[i].text)` 逐一釋放再把 `nsegs = 0`，然後落到最後一行解析內建文字；`segs` 陣列本身保留並由後續 `realloc` 重用。
  - 檔案內容的緩衝區 `buf` 為 `malloc(sz + 1)`，解析後即 `free`；片段的文字是複製出來的，所以不受影響。
  - 檔案存在但是空檔時，載入成功且片段數為 0，之後（未指定 `--loop` 時）`produce()` 會立刻進入提示字元模式；有 `--loop` 則進入 3 秒的 `M_PAUSE` 後重複。
  - 只有 `fopen` 的回傳值被檢查；`ftell`、`malloc`、`fread` 都沒有錯誤處理。
- **關係**：被誰呼叫 → `main`（開 pty 之前，`tools/benchsim.c:421`）；呼叫 → `parse_script`、`LOGI`/`LOGW`。

### `prompt`
*位置：`tools/benchsim.c:235`　簽章：`static void prompt(void)`*

- **用途**：把假 U-Boot 的提示字元（`opt_prompt`，預設 `"=> "`）排入輸出佇列。
- **設計**：集中成單一函式，讓「播放結束」「按鍵打斷播放」「每個命令跑完」「Ctrl-C」都用同一個出口，`--prompt` 就只需改 `opt_prompt` 一處。
- **細節**：只入佇列，不改 `mode`；呼叫端自己決定是否同時把 `mode` 設成 `M_PROMPT`。
- **關係**：被誰呼叫 → `run_command`、`handle_input`、`produce`；呼叫 → `q_str`。

### `run_command`
*位置：`tools/benchsim.c:240`　簽章：`static void run_command(const char *cmd)`*

- **用途**：執行使用者在假提示字元下輸入的一行命令，把回應排入佇列；支援 `version`、`printenv`、`reset`、`help`（或 `?`），其他命令回報 Unknown command。
- **設計**：命令是整行完全比對（`strcmp`），因為只需模擬少數固定指令；回應全部寫成 `\r\n` 結尾，與真機 UART 一致。`reset` 不另外重啟行程，而是把狀態機撥回播放起點，所以重播邏輯與一開始的播放共用 `produce()`，不需要第二份程式。
- **細節**：
  - 先略過前導空白（只處理空格）。空命令只送回提示字元。
  - `version`：固定三行（`U-Boot 2014.07 (benchsim simulated device)` 等），`scripts/test.sh` 以 grep 這串文字檢查 `bench send ... 'version\r'` 是否得到答覆。
  - `printenv`：`baudrate=` 後面印的是實際的 `opt_baud`（用 `snprintf` 到 128 byte 的 `b`），其餘變數為固定內容；最後一行是固定的 `Environment size: 214/131068 bytes`。
  - `reset`：排入 `resetting ...\r\n`，設 `mode = M_PLAY`、`seg_idx = 0`、`delay_until_ns = mono_ns() + 300000000ull`（300 ms 後才開始重播），`LOGI("reset: replaying log")`，然後直接 `return`，不送提示字元（重播結束後才由 `produce()` 送）。
  - 未知命令：`snprintf(b, 300, "Unknown command '%.200s' - try 'help'\r\n", cmd)`，`%.200s` 限制回顯長度，避免超過 `b`（`line[]` 最長 255 字元）。
  - 依程式順序推得的邊界情況：`reset` 把 `mode` 改成 `M_PLAY` 後，若同一次 `handle_input()` 呼叫還有後續位元組（例如 `reset\r\n` 的 `\n` 在同一批資料中），該位元組會被當成播放中的按鍵，清空佇列並回到提示字元，重播會被取消（見 `handle_input`）。單送 `reset\r` 則正常重播。
- **關係**：被誰呼叫 → `handle_input`（收到 CR 或 LF 時）；呼叫 → `prompt`、`q_str`、`mono_ns`（`lib/util.c`）、`LOGI`。

### `handle_input`
*位置：`tools/benchsim.c:280`　簽章：`static void handle_input(const uint8_t *p, size_t n)`*

- **用途**：處理從 pty master 讀到的主機輸入：逐位元組做行編輯與回顯，遇到換行就執行命令；播放途中收到任何位元組則停止播放並進入提示字元模式。
- **設計**：真實 U-Boot 的行為是「在 autoboot 倒數時按任意鍵就中斷」，所以只要 `mode != M_PROMPT`（播放中或迴圈間隔）就立刻進入提示模式：清空尚未送出的佇列（`outq_len = 0`）、`mode = M_PROMPT`、`line_len = 0`、送 `\r\n` 和提示字元。與真機不同的是，這個觸發的按鍵不會被吞掉，而是接著當成命令的第一個字元處理，所以 `bench send "version\r"` 在任何時候都能直接生效（DECISIONS.md：「Unlike real U-Boot the key is not swallowed」）。slave 端已被 `cfmakeraw`（見 `main`），沒有核心的行規範與回顯，所以行編輯與回顯必須由這個函式自己做。
- **細節**：
  - CR/LF 處理：`last_was_cr` 記錄前一個位元組是否是 `\r`；`\r\n` 序列的 `\n` 被略過（`continue`），單獨的 `\r` 或 `\n` 各算一次 Enter。每個位元組處理時都更新 `last_was_cr = (c == '\r')`。Enter 時先送回 `\r\n`，將 `line[line_len]` 補 NUL、`line_len = 0`，再呼叫 `run_command(line)`。
  - 退格：`0x7f` 或 `0x08`，若 `line_len > 0` 則 `line_len--` 並回顯 `"\b \b"`；行空時無動作。
  - Ctrl-C（`0x03`）：清掉目前行，送 `<INTERRUPT>\r\n` 和提示字元。
  - 可顯示字元（`0x20..0x7e`）在 `line_len < sizeof line - 1`（即 255）時才加入行緩衝並回顯；行滿之後的字元被默默丟棄，也不回顯。其他控制字元（Tab、ESC 等）一律忽略。
  - 依程式順序，進入提示模式的檢查在 CRLF 略過檢查之前；因此 `reset` 之後同一批資料的剩餘位元組會再次觸發進入提示模式（見 `run_command`）。
  - 這個函式不做任何 I/O，只改狀態與佇列；寫出由 `drain()` 完成。
- **關係**：被誰呼叫 → `main`（`poll()` 回報可讀、`read(mfd, ...)` 之後，`tools/benchsim.c:481`）；呼叫 → `q_str`、`q_put`、`prompt`、`run_command`、`LOGI`。

### `produce`
*位置：`tools/benchsim.c:321`　簽章：`static void produce(uint64_t now)`*

- **用途**：依狀態機從 `segs[]` 取出下一個片段排入輸出佇列，使佇列裡至少有一個 write chunk 的資料（除非正處於 `#delay`）。播放結束時決定進入提示模式或迴圈重播。
- **設計**：迴圈條件 `outq_len < chunk_bytes && now >= delay_until_ns`：只在佇列快空時才補貨，所以佇列保持很短，`#delay` 之後的內容不會提前排入，且輸入打斷時只需丟掉很少的資料。三種模式：
  - `M_PLAY`：逐片段處理。
  - `M_PAUSE`（只有 `--loop` 的段落間隔用）：時間到後轉回 `M_PLAY` 並把 `seg_idx` 設回 0。
  - `M_PROMPT`：直接 `return`，輸出完全由 `handle_input` 驅動。
- **細節**：
  - 到達最後一個片段後（`seg_idx >= nsegs`）：若 `outq_len` 還有資料就先 `return`，讓 log 的尾巴先送出再處理結束，避免提示字元或下一輪蓋掉尾端。佇列空了之後：有 `--loop` 就 `mode = M_PAUSE`、`delay_until_ns = now + 3000000000ull`（3 秒間隔）並 `LOGD`；否則 `mode = M_PROMPT`、呼叫 `prompt()`、`LOGI("end of log, prompt mode")`。
  - 延遲片段：`start = max(next_due_ns, now) + outq_len * byte_ns`，`delay_until_ns = start + delay_ms * 1000000`。意思是暫停從「目前佇列中的位元組在模擬線路上全部送完」的時刻起算，而不是從片段被取出的時刻起算（DECISIONS.md：「A `#delay` starts after the text queued before it has gone out」），所以 log 中的停頓間隔跟擷取時一致，不受波特率限制拉長或縮短。處理完延遲片段後，只要新的 `delay_until_ns` 晚於 `now`（通常如此：`delay_ms > 0` 或佇列中還有資料），迴圈條件 `now >= delay_until_ns` 就不成立而離開；`#delay 0` 且佇列空、線路閒置時 `delay_until_ns` 等於 `now`，迴圈會繼續取下一個片段。
  - 文字片段：`q_put(s->text, s->len)`；一個片段可能比 `chunk_bytes` 大，整行一次排入，由 `drain()` 分次寫出。
  - `LOGD` 在 release 建置（`LOG_COMPILE_LEVEL=2`）被編譯掉，只在 debug 建置（等級 4）存在。
- **關係**：被誰呼叫 → `main` 事件迴圈每圈一次；呼叫 → `q_put`、`prompt`、`LOGD`/`LOGI`。

### `drain`
*位置：`tools/benchsim.c:357`　簽章：`static void drain(uint64_t now)`*

- **用途**：若已到 `next_due_ns` 且佇列有資料，就把最多一個 chunk 寫到 pty master，並把 `next_due_ns` 往後推「這些位元組的線路時間」。
- **設計**：用「下次可寫時間」做節流，而不是每個位元組 `sleep`：每次寫 `chunk_bytes`（`baud / 10 / 100`，約 10 ms 的資料量，最少 1 byte），寫完把 `next_due_ns += w * byte_ns`（`byte_ns = 10e9 / baud`），長期平均速率就等於 baud/10 byte/s，同時系統呼叫次數只有每秒約 100 次。
- **細節**：
  - 若 `next_due_ns + 50 ms < now`（先前閒置太久），先把 `next_due_ns = now`，不補發積欠的資料，避免閒置後瞬間灌出一大串，破壞速率。
  - 一次最多寫一個 chunk；`main` 的迴圈會再次呼叫。
  - `write()` 失敗：`EAGAIN`/`EWOULDBLOCK`/`EIO` 視為整塊已送出而丟棄（註解：沒有人在讀，真實 UART 也會丟失）；其他 errno 則 `LOGE("write pty: ...")` 後同樣丟棄。這樣不管有沒有 benchd 連著，播放時間軸都不會被阻塞或卡住。master 為非阻塞（`main` 設了 `O_NONBLOCK`），pty 緩衝區滿時就是 EAGAIN 這條路徑。
  - 部分寫入（`0 < w < n`）只移除實際寫出的 `w` 個位元組，其餘留在佇列等下一輪；`next_due_ns` 也只推進 `w` 的量。
  - 佇列用 `memmove` 往前搬，因為佇列長度很小，成本可忽略。
- **關係**：被誰呼叫 → `main` 事件迴圈每圈一次；呼叫 → `write`、`memmove`、`LOGE`。

### `usage`
*位置：`tools/benchsim.c:378`　簽章：`static void usage(FILE *f)`*

- **用途**：把命令列用法印到 `f`：`-l/--log`、`-b/--baud`、`-L/--link`、`--loop`、`--prompt`、`-v`、`-h/--help`，並附預設值（115200、`/tmp/benchsim0`、`"=> "`）。
- **設計**：接收 `FILE *` 參數，讓 `-h` 時輸出到 stdout（正常結束），參數錯誤時輸出到 stderr（以非零碼結束），符合一般 CLI 習慣。
- **細節**：只是 `fprintf`，沒有回傳值；`--loop` 說明裡的「3 s」與 `produce()` 的 3000000000 ns 一致。
- **關係**：被誰呼叫 → `main`（`-h` 與未知選項）；呼叫 → `fprintf`。

### `main`
*位置：`tools/benchsim.c:391`　簽章：`int main(int argc, char **argv)`*

- **用途**：解析選項、建立 pty 與符號連結、安裝訊號處理，然後執行單執行緒事件迴圈（`produce` → `drain` → `poll` → `handle_input`），直到收到訊號，最後移除符號連結。
- **設計**：
  - 選項：`getopt_long` 的短選項字串是 `"l:b:L:vh"`；`--loop` 與 `--prompt` 沒有短選項，使用回傳值 1 與 2。`-b` 用 `strtoul` 轉成 `opt_baud`，小於 50 就印 `benchsim: bad baud rate` 並以 1 結束（同時擋掉轉換失敗得到的 0）。之後 `bench_log_set_role("benchsim")`、`bench_log_init_level(verbose)`。
  - 節奏參數：`byte_ns = 10.0 * 1e9 / opt_baud`（一個 byte 含起始與停止位元共 10 bit），`chunk_bytes = opt_baud / 10 / 100`，小於 1 時補成 1；`load_log()` 準備片段。
  - 建立 pty：`posix_openpt(O_RDWR | O_NOCTTY)` + `grantpt` + `unlockpt`，失敗就 `LOGE` 並結束。`ptsname()` 取得 slave 路徑。接著自己開一個 slave fd（`sfd`）並 `cfmakeraw`。這個自留的 slave fd 有兩個作用：保存 raw termios 設定，以及在 benchd 還沒連上（或暫時離開）時，master 端寫入不會因為沒有 slave 開啟而得到 EIO（DECISIONS.md 的 benchsim 一節）。benchsim 自己從不讀這個 fd。master 被設為 `O_NONBLOCK`，讓 `drain()` 的 `write()` 不會卡住。
  - 發佈連結：`unlink(opt_link)` 再 `symlink(slave, opt_link)`，先 unlink 是為了蓋掉上次殘留的連結（例如被 SIGKILL 的舊行程）；`symlink` 失敗則 `LOGE` 後以 1 結束。
  - 訊號：以 `sigaction` 安裝 `on_signal` 於 SIGINT、SIGTERM、SIGHUP；`signal(SIGPIPE, SIG_IGN)`。
- **細節**：
  - 迴圈開始前 `next_due_ns` 與 `delay_until_ns` 都設為 `mono_ns()`，所以播放立即開始；`mode` 的初始值是 `M_PLAY`。
  - poll 逾時計算：佇列有資料時用 `(next_due_ns - now) / 1e6`，夾在 0 到 50 ms，若算出 0 但還沒到時間則改成 1 ms（避免在不到 1 ms 的差距上空轉）；佇列空、非提示模式、且正處於延遲時，用 `(delay_until_ns - now) / 1e6`，上限 50，否則 `+1`；其餘情況（例如提示模式）預設 50 ms。上限 50 ms 也保證訊號最慢在這個時間內被發現。逾時用 `poll()` 本身，所以沒有忙等待。
  - `poll()` 只監看 `mfd` 的 `POLLIN`；可讀就 `read(mfd, buf, 256)`，`n > 0` 時 `LOGD("rx %zd bytes from host")` 並呼叫 `handle_input()`。
  - 迴圈結束後 `LOGI("signal %d: removing %s")`、`unlink(opt_link)`、`return 0`。行程結束時作業系統關閉 `mfd` 與 `sfd`，持有 slave 的 benchd reader 於是 `read()` 得到 EIO（或 0），觸發 R8 的 DEV_LOST 與每 500 ms 重新開啟（`server/reader.c`、`scripts/demo_r8.sh`）。
  - 沒有 `--unsafe` 選項；與強健性開關的關係只有上述 R8 示範，以及 R10 示範（`scripts/demo_r10.sh`）用較高波特率播放無停頓的 log 來製造快速資料流。
  - 啟動前置失敗（`posix_openpt`、`symlink`）都沒有清理已開的 fd，直接結束，由作業系統回收。
- **關係**：被誰呼叫 → 由 shell 或腳本啟動（`scripts/lib.sh` 的 `start_sim` 以背景方式執行 `benchsim -L "$DEV" ...`）；呼叫 → `load_log`、`produce`、`drain`、`handle_input`、`usage`、`bench_log_set_role` 與 `bench_log_init_level`（`lib/log.c`）、`mono_ns`（`lib/util.c`）。

---

## `tools/bench-evil.c`

`bench-evil` 是獨立的測試工具（單一行程、不 fork、不用執行緒），編譯成 `bench-evil` 執行檔（Makefile:84）。它扮演「行為不良的 benchd 客戶端」，每個 mode 對應 §9 的一個強健性機制：`slow`（R4）、`bad-len` / `bad-magic` / `bad-crc`（R3）、`silent`（R5）、`rst`（R2）、`flood`（R9）、`take-and-die`（R6）、`lag`（R10），另外有只給測試用的 `take-hold`（`docs/DECISIONS.md` 的 bench-evil 一節）。它直接使用 libbench.so 的 `tcp_connect_timeout`、`bn_send`、`bn_recv`、`bench_crc32`、`bn_hdr_encode` 等函式（所以 header 的編碼和 CRC 是正確的，除非 mode 刻意改壞它），伺服器端的反應則由 `scripts/demo_r2.sh`、`demo_r3.sh`、`demo_r4.sh`、`demo_r5.sh`、`demo_r6.sh`、`demo_r9.sh`、`demo_r10.sh` 與 `scripts/test.sh`、`scripts/capture.sh` 呼叫它之後判讀。

這支程式本身不解析 `--unsafe`（`main` 的 long option 表沒有這一項），所以它自己的 `bn_recv` 永遠是 safe 行為；`--unsafe=<switch>` 只加在被測的 `benchd` 上。每個 mode 的輸出都以 `evil:` 開頭並且逐行 flush，因為測試腳本是用 `grep` 比對這些字串（例如 `got INFO`、`BAD_LENGTH`、`0 of 5 silent connections still open`、`32 welcomed, 8 refused`）。全域狀態只有命令列選項與一個共用的接收緩衝區 `rbuf[BN_MAX_PAYLOAD]`（tools/bench-evil.c:32-39）。

### `dial`
*位置：`tools/bench-evil.c:41`　簽章：`static int dial(void)`*

- **用途**：用全域 `host` / `port` 建立一條 TCP 連線，失敗時在 stderr 印出原因。
- **設計**：把「連線 + 錯誤訊息」包成一處，讓每個 mode 只需判斷 `fd < 0`。連線交給函式庫的 `tcp_connect_timeout`，逾時固定 3000 ms，這樣伺服器沒開或被防火牆擋住時工具不會卡在 `connect()`。
- **細節**：
  - 成功回傳已連線的 blocking socket；`tcp_connect_timeout` 在連線成功後會把 socket 設回 blocking 並開 `TCP_NODELAY`（lib/net.c:130-132），`mode_slow` 靠這個才能讓每個單一位元組真的各自成為一個 TCP segment。
  - 失敗回傳 -1，印 `evil: connect <host>:<port>: <strerror(errno)>`；errno 由 `tcp_connect_timeout` 設定（解析失敗是 `EHOSTUNREACH`，逾時是 `ETIMEDOUT`）。
  - `mode_lag` 不使用它（需要在 `connect()` 前設定 `SO_RCVBUF`），`mode_rst` 與 `mode_flood` 直接呼叫 `tcp_connect_timeout`，不印每次的錯誤。
- **關係**：被誰呼叫 → `mode_slow`、`mode_bad`、`mode_silent`、`mode_take_and_die`、`mode_take_hold`；呼叫 → `lib/net.c` 的 `tcp_connect_timeout`。

### `build_packet`
*位置：`tools/bench-evil.c:49`　簽章：`static size_t build_packet(uint8_t *out, uint8_t type, const void *p, uint32_t len)`*

- **用途**：把一個完整封包（12 位元組 header + payload）組在呼叫者給的緩衝區裡，回傳總長度。
- **設計**：`bn_send` 一送出就是合法封包，無法做「一次只送一個位元組」或「事後改壞 header」這種實驗，所以 evil 要自己先把位元組組好再用 `send()` 處理。header 用函式庫的 `bn_hdr_encode`、CRC 用 `bench_crc32`，保證正常欄位與協定完全一致；之後 `mode_bad` 只要在成品上改一兩個位元組就能得到「剛好壞一處」的封包。
- **細節**：
  - `len == 0` 時 CRC 欄位填 0（與 `bn_send` 的 `len ? crc : 0` 規則相同），也不會讀 `p`，所以 `INFO_REQ` 可傳 `NULL`。
  - 不檢查 `out` 容量；呼叫者都用 `uint8_t pkt[256]`，而 HELLO 是 12 + 56 = 68 位元組（`struct bn_hello` 為 32+4+1+3+8+8 = 56），`mode_slow` 再加一個 12 位元組的 INFO_REQ 共 80 位元組，沒有超出。
  - 回傳值是 `BN_HDR_LEN + len`，`mode_slow` 以它作為下一個封包的寫入偏移。
- **關係**：被誰呼叫 → `mode_slow`、`mode_bad`；呼叫 → `lib/proto.c` 的 `bn_hdr_encode`、`lib/crc32.c` 的 `bench_crc32`。

### `make_hello`
*位置：`tools/bench-evil.c:57`　簽章：`static void make_hello(struct bn_hello *h, uint32_t replay)`*

- **用途**：填出一個 HELLO payload：名字取自 `opt_name`，要求回放 `replay` 位元組歷史，其餘欄位全 0。
- **設計**：先 `memset` 整個結構，使 `has_resume = 0`、`resume_offset = 0`、`last_server_start_ns = 0`，也就是「全新連線、不是重連續傳」，伺服器會走一般的 REPLAY 流程而不是 resume 流程（server/session.c 握手段）。名字用 `bench_strlcpy` 截斷，避免超過 `BN_NAME_LEN`（32）而漏掉結尾 NUL。
- **細節**：`replay_bytes` 用 `htobe32` 轉成網路位元組序，因為 payload 是 packed、big-endian 的線上格式；所有 mode 都傳 0，所以不要求歷史回放，只看即時資料。
- **關係**：被誰呼叫 → `hello`、`mode_slow`、`mode_bad`；呼叫 → `lib/util.c` 的 `bench_strlcpy`。

### `hello`
*位置：`tools/bench-evil.c:64`　簽章：`static int hello(int fd, uint32_t replay)`*

- **用途**：在 `fd` 上送出一個正常的 HELLO 封包。
- **設計**：需要「合法握手」的 mode（`flood`、`take-and-die`、`take-hold`、`lag`）只想要一行完成，所以走函式庫的 `bn_send`（header 與 payload 一次 `write()`）。
- **細節**：回傳 `bn_send` 的結果（0 成功、-1 失敗並帶 errno）；所有呼叫者都忽略回傳值，連線若已斷會在後續的 `wait_for` / `bn_recv` 才被發現。`bn_send` 走 `write()` 而不是 `send(..., MSG_NOSIGNAL)`，因此依賴 `main` 裡的 `signal(SIGPIPE, SIG_IGN)` 才不會被自己殺掉。
- **關係**：被誰呼叫 → `mode_flood`、`mode_take_and_die`、`mode_take_hold`、`mode_lag`；呼叫 → `make_hello`、`lib/proto.c` 的 `bn_send`。

### `wait_for`
*位置：`tools/bench-evil.c:72`　簽章：`static int wait_for(int fd, int type, int ms, struct bn_hdr *out)`*

- **用途**：最多等 `ms` 毫秒，一路讀封包並丟掉，直到收到型別為 `type` 的封包為止；途中遇到 ERROR 封包就印出來。回傳 1 找到、0 逾時、-1 連線關閉或出錯。
- **設計**：用 `mono_ms()` 算出絕對期限 `end`，每圈重新計算剩餘時間再交給 `poll`；若只在開頭算一次 `poll` 的毫秒數，被 `EINTR` 或收到不相關封包而重試時總等待時間會超過 `ms`。先 `poll` 再 `bn_recv` 是為了讓逾時可控：`bn_recv` 本身會阻塞到整個封包到齊。收到 ERROR 時先印再比對型別，所以即使呼叫者要等的是別的型別（或 `-1`），伺服器送來的 `BAD_LENGTH`、`SERVER_BUSY` 等原因都會出現在輸出，`scripts/test.sh` 的 `bad-len answered with BAD_LENGTH` 檢查就是靠這行。
- **細節**：
  - `poll(...) <= 0` 一律 `continue`：包含逾時與 `EINTR`，最後由開頭的 `left <= 0` 判斷結束。
  - `bn_recv` 回傳非 1（0 為 EOF，-1 為 I/O 錯誤，-2 為協定錯誤）一律回 -1，不區分原因；`mode_lag` 因為想印出 `bn_err_name(perr)` 所以自己呼叫 `bn_recv`。
  - ERROR payload 的前 2 位元組是 big-endian 錯誤碼（`struct bn_error_hdr`），其後 `h.len - 4` 位元組是文字；只在 `h.len >= 4` 時才處理，避免長度相減變負。
  - `type` 傳 `-1` 時永遠不會比對成功（`h.type` 是 0..255），函式就變成「排空並等到逾時或連線關閉」；`report_reaction` 利用這點。
  - 找到時 payload 還留在全域 `rbuf`，下一次 `bn_recv` 才會蓋掉；呼叫者（`mode_slow` 的 INFO、`mode_take_*` 的 `rbuf[0]`）直接讀 `rbuf`。`out` 可為 `NULL`。
  - 此處 `bn_recv` 的 `cap` 是 `sizeof rbuf` = 65536，剛好等於 `BN_MAX_PAYLOAD`。
- **關係**：被誰呼叫 → `report_reaction`、`mode_slow`、`mode_take_and_die`、`mode_take_hold`；呼叫 → `lib/util.c` 的 `mono_ms`、`lib/proto.c` 的 `bn_recv`、`bn_err_name`。

### `report_reaction`
*位置：`tools/bench-evil.c:100`　簽章：`static void report_reaction(int fd, int ms)`*

- **用途**：送出壞封包後，觀察伺服器的反應：在 `ms` 毫秒內連線被關閉，就印出「關閉花了多久」；否則印「連線仍開著」。
- **設計**：反應只有兩種可觀察的結果（伺服器主動斷線 vs. 沒有動作），所以不用解析封包內容，只要借 `wait_for(fd, -1, ms, NULL)` 把所有封包讀掉（其中的 ERROR 順便被印出）。用 `mono_ms()` 前後相減得到從呼叫到偵測到斷線的時間，供 demo 說明 safe 模式下是立刻斷線而不是等逾時。
- **細節**：
  - 只有 `rc < 0`（EOF / 錯誤 / 協定錯誤）才算關閉；`rc == 0`（逾時）代表仍開著。`rc == 1` 不會發生，因為 `-1` 永遠不匹配。
  - 以 safe 伺服器為例，`bad-magic` / `bad-crc` / `bad-len` 都會收到 ERROR 然後被關閉；若伺服器是 `--unsafe=validate`，行為視 child 是否崩潰而異，這個函式只回報看到的結果。
  - 傳給 `mode_bad` 的等待時間為 3000 ms。
- **關係**：被誰呼叫 → `mode_bad`；呼叫 → `wait_for`、`mono_ms`。

### `mode_slow`
*位置：`tools/bench-evil.c:110`　簽章：`static int mode_slow(void)`*

- **用途**：R4（readn）示範：把 HELLO 與 INFO_REQ 兩個封包（共 80 位元組）每 50 ms 送一個位元組，再等伺服器回 INFO；收到就代表伺服器正確重組了被切碎的封包。回傳 0 成功、1 失敗。
- **設計**：TCP 是位元組串流，封包可能被切成任意大小。若伺服器只用一次 `recv()` 讀 header（`--unsafe=readn` 的行為），一個位元組就會被當成完整 header 而得到錯誤的 `BAD_MAGIC`；safe 的 `readn()` 會一直讀到 12 位元組為止。要讓這件事能重現，客戶端必須真的一位元組一個 segment：`dial` 的 socket 已開 `TCP_NODELAY`，且每個位元組之間 `sleep_ms(50)`，Nagle 就沒有機會合併。兩個封包先用 `build_packet` 全組在 `pkt[256]` 再逐位元組送。整個過程約 80 × 50 ms = 4 秒（程式會印出這個預估值），而伺服器的握手逾時是 `HELLO_TIMEOUT_MS`（5000 ms），位元組之間的間隔遠小於它，所以 safe 伺服器不會因為慢而中斷（R5 與 R4 互不干擾）。
- **細節**：
  - 逐位元組用 `send(fd, pkt + i, 1, MSG_NOSIGNAL)`，失敗就印出是第幾個位元組、errno，並回傳 1（`unsafe=readn` 的伺服器關閉連線後這裡通常會發生，或在最後的 `wait_for` 看到關閉）。
  - 送完後 `wait_for(fd, BN_INFO, 5000, &hh)`；伺服器會先送 WELCOME、REPLAY_BEGIN、REPLAY_END（HELLO 的 replay 為 0）等封包，`wait_for` 會略過它們。
  - 成功時印出 INFO 長度，並把 `rbuf` 前最多 200 位元組 `fwrite` 到 stdout，再印 `...`；`scripts/demo_r4.sh` 用 `| head -8` 截取，`scripts/test.sh` 以 `got INFO` 判斷。
  - 失敗訊息區分 `connection closed`（`rc < 0`）與 `timeout`。
  - 送出路徑上途中失敗時沒有 `close(fd)`，直接 `return 1`，行程結束時由系統回收。
- **關係**：被誰呼叫 → `main`；呼叫 → `dial`、`make_hello`、`build_packet`、`wait_for`、`lib/util.c` 的 `sleep_ms`。

### `mode_bad`
*位置：`tools/bench-evil.c:143`　簽章：`static int mode_bad(const char *which)`*

- **用途**：R3（validate）示範：依 `which` 送出一個壞 HELLO（`bad-magic`、`bad-crc`，其餘情況視為 `bad-len`），然後用 `report_reaction` 觀察伺服器是否關閉連線。固定回傳 0（連線失敗回 1）。
- **設計**：先用 `build_packet` 做出一個完全合法的 HELLO，再「只破壞一處」，這樣每個變體只測一種檢查：
  - `bad-magic`：把前兩個位元組改成 `'X' 'Y'`（0x5859，而不是 0x424E）。
  - `bad-crc`：`pkt[8] ^= 0xFF`，位元組 8 是 CRC32 欄位的第一個位元組，payload 沒動，CRC 就對不上。
  - `bad-len`：用 `bn_hdr_encode` 重寫 header，`len = 0x00100000`（1 MiB）、CRC 隨意填 `0xDEADBEEF`，只送 header，再用 8192 位元組的 `'A'` 緩衝區持續送足 1 MiB。safe 伺服器只要看到 header 的 `len > BN_MAX_PAYLOAD`（65536）就回 `ERROR BAD_LENGTH` 並斷線，不會去讀這個 payload；`--unsafe=validate` 的伺服器信任 `len`，把 1 MiB 讀進握手用的 `char buf[4096]`（server/session.c 的 `handshake`），造成堆疊溢位（`stack smashing detected` 或 SIGSEGV，見 `docs/DECISIONS.md` R3）。
- **細節**：
  - 所有 `send` 都帶 `MSG_NOSIGNAL`，所以伺服器先斷線時回傳 `EPIPE` / `ECONNRESET` 而不是信號。
  - `bad-len` 的迴圈在 `send` 回傳 `<= 0` 時印 `server stopped accepting after N bytes (...)` 並離開，這正好顯示 safe 伺服器在讀完 1 MiB 之前就已經關閉；若伺服器 child 崩潰（unsafe）也會在這裡停下。
  - 迴圈每圈送 `min(len - sent, 8192)`，所以最後一圈不會多送。
  - 結尾固定 `fflush(stdout)`，等 3000 ms 反應，`close(fd)` 後回 0；是否算通過由腳本看伺服器是否仍存活（`server_alive`）與日誌決定，不看這個回傳值。
- **關係**：被誰呼叫 → `main`（mode 為 `bad-len` / `bad-magic` / `bad-crc`）；呼叫 → `dial`、`make_hello`、`build_packet`、`report_reaction`、`lib/proto.c` 的 `bn_hdr_encode`。

### `raise_nofile`
*位置：`tools/bench-evil.c:186`　簽章：`static void raise_nofile(int need)`*

- **用途**：必要時把行程的 `RLIMIT_NOFILE` 軟限制提高到 `need + 32`，讓一次開很多連線的 mode 不會先被 `EMFILE` 擋住。
- **設計**：多數系統預設軟限制是 1024，`--count` 超過這個值就開不出那麼多 socket。只在目前軟限制「小於」需求時才動它，而且不超過硬限制（`rlim_max`），因為一般使用者無法越過硬限制。額外加的 32 是留給 stdout、stderr 與函式庫內部可能用到的檔案描述子。
- **細節**：`getrlimit` 失敗或 `setrlimit` 失敗都被忽略（沒有回傳值）；若硬限制仍不夠，後面的 `tcp_connect_timeout` 會失敗，那些連線會被計入 `failed`（flood）或在 `dial` 印出錯誤（silent）。
- **關係**：被誰呼叫 → `mode_silent`、`mode_flood`；呼叫 → libc 的 `getrlimit` / `setrlimit`。

### `mode_silent`
*位置：`tools/bench-evil.c:195`　簽章：`static int mode_silent(void)`*

- **用途**：R5（timeout）示範：開 `--count` 條 TCP 連線，一個位元組都不送，觀察伺服器多久後把它們踢掉。回傳 0。
- **設計**：safe 伺服器在 `handshake()` 對沒送 HELLO 的連線 `poll` 5 秒（`HELLO_TIMEOUT_MS`），逾時就送 `ERROR HANDSHAKE_TIMEOUT` 並關閉（server/session.c:196-208）；`--unsafe=timeout` 時這段被略過，每條沉默連線會永遠占住一個 child 與一個 slot，`demo_r5.sh` 開 32 條就能把預設 `--max-clients 32` 用滿，讓其他人連不進來。客戶端用 `poll` 同時監看所有 fd 並以 200 ms 為粒度檢查，這樣能量到「伺服器大約幾秒後關閉」而不需要多執行緒或每條連線一個行程。
- **細節**：
  - `raise_nofile(opt_count)` 之後 `calloc` 一個 fd 陣列；`dial()` 失敗的項目是 -1，不計入 `open_n`。`calloc` 的回傳值沒有檢查。
  - 保持時間 `hold = opt_sec > 0 ? opt_sec : 30`（預設 30 秒）；迴圈在「全部被關」或「到期」時結束。
  - 每一圈重新 `calloc` 一個 `pollfd` 陣列，已關閉的項目 `fd = -1`，`poll` 會忽略負的 fd；圈尾 `free`。
  - 有 `POLLIN | POLLHUP | POLLERR` 時 `recv` 最多 512 位元組：回傳 `<= 0` 才視為被關，`close` 並 `open_n--`；收到資料（例如 `HANDSHAKE_TIMEOUT` 的 ERROR 封包）時內容直接丟掉，連線仍留著，等下一圈讀到 EOF 才算被關。
  - 只有「第 0 條」或「最後一條」被關時才印 `server dropped silent connection #i after X s (N left)`，避免 32 條各印一行。
  - 結尾印 `N of M silent connections still open after X s`；`scripts/test.sh` 用 `0 of 5 silent connections still open` 判斷 safe 行為（`--count 5 --sec 7`，7 秒大於 5 秒逾時）。
  - 結束時沒有逐一 `close`，由行程結束回收。
- **關係**：被誰呼叫 → `main`；呼叫 → `raise_nofile`、`dial`、`mono_ms`。

### `mode_rst`
*位置：`tools/bench-evil.c:237`　簽章：`static int mode_rst(void)`*

- **用途**：R2（sigpipe）示範：連續 `--count` 次「連上去，馬上以 RST 結束」，不送任何資料。回傳 0。
- **設計**：對 `close()` 前的 socket 設 `SO_LINGER {l_onoff = 1, l_linger = 0}`，核心會丟掉未送資料並送 RST 而不是 FIN。伺服器滿載時，master 的 `reject_busy` 會先 `poll` 100 ms 等 HELLO（`recv` 可能拿到 `ECONNRESET`），接著對這條已被重置的連線寫 `SERVER_BUSY`，這個 `write()` 在沒有忽略 SIGPIPE 時會殺掉 master（server/main.c:114-132，`docs/DECISIONS.md` R2）。因此 `demo_r2.sh` 先用 `flood --count 3` 把 `--max-clients 3` 的伺服器占滿，再跑 `rst --count 40`。這一支只管製造 RST，是否死亡由 `--unsafe=sigpipe` 決定。
- **細節**：
  - 每次連線直接用 `tcp_connect_timeout`（3 秒），失敗就印 `connect #i failed: ... (server down?)` 並 `break`；之後的連線不再嘗試。
  - 每次之後 `sleep_ms(5)`，避免 40 條連線在同一個瞬間擠進 listen backlog。
  - 結束印 `sent N connect+RST`，N 是成功連上的次數；就算全部失敗也回傳 0，是否正確要看腳本對伺服器存活的檢查。
  - 不送 HELLO，所以伺服器日誌裡拒絕訊息的名字會是 `?`（HELLO 沒收到時 `name` 預設為 `"?"`）。
- **關係**：被誰呼叫 → `main`；呼叫 → `lib/net.c` 的 `tcp_connect_timeout`、`sleep_ms`。

### `mode_flood`
*位置：`tools/bench-evil.c:256`　簽章：`static int mode_flood(void)`*

- **用途**：R9（max-clients）示範：盡快開 `--count` 條連線並各送 HELLO，統計伺服器 WELCOME / 拒絕（`SERVER_BUSY`）/ 失敗的數量，然後把存活的連線維持 `--sec` 秒（預設 30）。回傳 0。
- **設計**：分成三個階段。
  1. 連線：先全部連完並各自送 HELLO，不在中間讀回應，這樣是「同一時間打進去」而不是一條一條處理。名字用 `snprintf(opt_name, ..., "flood%03d", i)` 在每次呼叫 `hello()` 之前改寫，伺服器滿載時 `reject_busy` 會從 HELLO 取名字寫進日誌（server/main.c:118-123），所以每條連線帶不同名字才能在日誌分辨誰被拒絕。
  2. 分類：逐條 `poll` 最多 3000 ms 後讀第一個封包；收到 `BN_WELCOME` 記為 welcomed；其他情況裡，收到 `BN_ERROR`（在 safe 伺服器就是 `SERVER_BUSY`）記為 busy 並關掉該連線；其他型別或讀取失敗記為 failed 並關閉。`poll` 逾時（沒有任何回應）也計 failed，但該連線不會被關閉。
  3. 維持：每 100 ms 以 `MSG_DONTWAIT` 把每條存活連線的接收緩衝區排空，並每超過 10 秒對每條送一個 8 位元組全零的 `BN_PING`。排空是為了不讓伺服器 child 因為客戶端不讀而卡在寫入；PING 是為了保持在伺服器 60 秒閒置限制（`IDLE_TIMEOUT_MS`）之內，當 `--sec` 超過 60 時連線不會被判閒置。
- **細節**：
  - `raise_nofile(opt_count)` 之後 `calloc` fd 陣列（回傳值沒檢查）；連線失敗的項目是 -1，計入 failed。
  - 分類階段中，`bn_recv` 在 EOF 或 I/O 錯誤時回傳前不會寫入 `h`，而後面的 `h.type == BN_ERROR` 仍會讀它（`h` 是未初始化的區域變數），結果只會落在 busy 或 failed 兩個計數之一，所以不影響流程但讀的是不確定值；實務上伺服器滿載會先送 `SERVER_BUSY` 才關閉，因此多半會正確歸入 busy。
  - 結果輸出格式 `flood of N: A welcomed, B refused (SERVER_BUSY), C failed`；`scripts/test.sh` 對 `--count 40` 檢查 `32 welcomed, 8 refused`（預設 `--max-clients 32`）。`--unsafe=max-clients` 時伺服器不設上限，一直 fork 到失敗為止，welcomed 數量就會遠大於 32（`demo_r9.sh` 用 `--count 200 --sec 6`）。
  - 維持階段中，WELCOME 之後的 REPLAY_BEGIN / DATA / REPLAY_END / PONG 都由排空迴圈丟掉；被伺服器關閉的連線只會讓 `recv` 回傳 0，不會被標記為 -1，之後的 PING 會因對方已關閉而失敗（SIGPIPE 已被忽略）。
- **關係**：被誰呼叫 → `main`；呼叫 → `raise_nofile`、`hello`、`lib/net.c` 的 `tcp_connect_timeout`、`lib/proto.c` 的 `bn_recv` / `bn_send`、`sleep_ms`。

### `mode_take_and_die`
*位置：`tools/bench-evil.c:317`　簽章：`static int mode_take_and_die(void)`*

- **用途**：R6（ctrl-release）示範：握手、取得控制權，確認「granted」之後不送 BYE 直接 `_exit(1)`，模擬控制者當機。回傳 1（被拒絕、沒有回應或連線失敗），成功時不會返回。
- **設計**：控制權記錄在共享記憶體的 `ctrl_pid`；如果伺服器只在收到明確的 RELEASE / BYE 才釋放，控制者崩潰後就沒人能再取得控制權。safe 伺服器有四條釋放路徑（斷線、SIGCHLD、`TAKE` 時 `kill(pid,0)` 檢查、閒置逾時），`--unsafe=ctrl-release` 全部關掉（`docs/DECISIONS.md` R6）。必須等到 `CTRL_RESULT` 且 `granted` 才死，否則還沒取得控制權就崩潰沒有意義。
- **細節**：
  - 流程：`dial` → `hello(fd, 0)` → `bn_send(BN_TAKE, NULL, 0)` → `wait_for(BN_CTRL_RESULT, 5000)`。
  - `rbuf[0]` 是 `struct bn_ctrl_result.granted`；被拒絕時 `rbuf + 12` 是 `holder`（偏移 12 = granted 1 + pad 3 + `holder_idle_sec` 4 + `auto_release_in_sec` 4），以 `%.32s` 印出，因為該欄位固定 32 位元組不保證有 NUL。
  - 成功時先 `fflush(stdout)` 再 `_exit(1)`：`_exit` 不會 flush stdio，也不跑 atexit；此外 `main` 已把 stdout 設成行緩衝，所以輸出在 `_exit` 前其實已經送出。socket 沒有 `close`，由核心在行程終止時關閉；若 socket 上還有未讀資料（伺服器已送出的 WELCOME 等），Linux 會以 RST 而不是 FIN 結束連線，`scripts/capture.sh` 用這個 mode 產生 `captures/rst.pcap` 來對照正常結束的 FIN。
  - `scripts/test.sh` 與 `demo_r6.sh` 使用 `--name crashy`；測試接著檢查日誌有 `control released by crashy (disconnect)`，且下一個客戶端能立刻取得控制權。
- **關係**：被誰呼叫 → `main`；呼叫 → `dial`、`hello`、`wait_for`、`lib/proto.c` 的 `bn_send`。

### `mode_take_hold`
*位置：`tools/bench-evil.c:338`　簽章：`static int mode_take_hold(void)`*

- **用途**：握手、取得控制權，然後安靜地持有 `--sec` 秒（預設 30）、不送任何輸入，最後以 BYE 正常結束。回傳 0 成功；連線失敗回 1；沒取得控制權（被拒或沒有 CTRL_RESULT）回 2。
- **設計**：這是 `make test` 的測試輔助 mode（不屬於 §9 的 R1-R12，見 `docs/DECISIONS.md` bench-evil 一節）。`scripts/test.sh` 用 `take-hold --name alice --sec 8` 在背景佔住控制權，用來驗證三件事：第二個客戶端被拒絕並看到 `held by alice`，閒置的持有者在 `--idle-release 3` 後被自動釋放，以及釋放後新的客戶端能取得控制權。它刻意不送輸入，因為伺服器的閒置計時 `ctrl_last_input_ns` 只在取得控制權當下（server/control.c:56）以及控制者送出輸入或 BAUD 時（server/session.c:351、397）更新；PING 不會重置計時（server/session.c 的 `BN_PING` 分支不碰它），所以每 10 秒的 PING 只維持連線，不會讓控制權永遠不閒置。
- **細節**：
  - 回傳 2 與 `take-and-die` 的 1 區分開，測試腳本可以分辨「沒搶到」和「連線層級錯誤」。
  - 持有期間每 100 ms 以 `MSG_DONTWAIT` 排空接收緩衝區（避免伺服器 child 卡在寫入），每超過 10 秒送一次 8 位元組的 `BN_PING`，與伺服器 60 秒閒置限制（`IDLE_TIMEOUT_MS`）保持距離。
  - 迴圈不會偵測控制權已被伺服器閒置釋放，也不會因為連線被關而提前結束，一直跑到時間到才送 `BN_BYE`；`bn_send` 失敗被忽略。
  - 最後 `close(fd)`，沒有 `shutdown(SHUT_WR)` 後排空，所以不保證是乾淨的 FIN 交換（客戶端 `bench` 才有這個流程）。
- **關係**：被誰呼叫 → `main`；呼叫 → `dial`、`hello`、`wait_for`、`lib/proto.c` 的 `bn_send`、`sleep_ms`。

### `print_live`
*位置：`tools/bench-evil.c:368`　簽章：`static void print_live(const struct bn_hdr *h)`*

- **用途**：把剛收到、放在全域 `rbuf` 的 DATA 或 EVENT 封包顯示在 stdout；其他型別忽略。
- **設計**：`mode_lag` 要讓人看到「恢復讀取之後收到什麼」，才能判斷資料是否完整、有沒有 `DROPPED` 事件。只輸出序列埠原始位元組與事件文字，格式簡單，方便 `demo_r10.sh` 的 awk 以「`line NNNN: ...`」與「`EVENT DROPPED`」行判斷完整性。
- **細節**：
  - DATA：payload 開頭是 16 位元組 `struct bn_data_hdr`（`offset` 8 + `ts_ns` 8），其後是原始位元組；只在 `h->len >= 16` 時用 `fwrite(rbuf + 16, 1, len - 16, stdout)`，條件避免無號減法下溢。
  - EVENT：payload 開頭是 20 位元組 `struct bn_event_hdr`（offset 8、ts 8、`code` 2、pad 2），事件碼是位於 `rbuf + 16` 的 big-endian `uint16_t`，以 `bn_evt_name` 轉成名稱，其後 `len - 20` 位元組是文字。輸出格式為 `"\n[EVENT %s] %.*s\n"`，前面的換行讓事件獨立成行，即使剛好插在 UART 的半行文字之後。
  - 沒有用 `h->type` 以外的欄位，也不驗證 `rbuf`；呼叫者必須剛剛 `bn_recv` 成功。
- **關係**：被誰呼叫 → `mode_lag`；呼叫 → `lib/proto.c` 的 `bn_evt_name`。

### `dial_small_rcvbuf`
*位置：`tools/bench-evil.c:380`　簽章：`static int dial_small_rcvbuf(void)`*

- **用途**：建立 TCP 連線，並在 `connect()` 之前把 `SO_RCVBUF` 設成 4096，使本端公告的接收視窗很小。失敗回 -1。
- **設計**：R10 要求「客戶端不讀，伺服器的 child 因而落後」。若用一般的 `dial`，loopback 上的 TCP 接收緩衝區會自動調大到數百 KB 到數 MB，伺服器能把一大段資料全塞進核心緩衝區，child 根本不會被擋住，環形緩衝區也就不會被覆蓋。接收緩衝區必須在 `connect()` 之前設定，因為視窗縮放（window scale）在 SYN 交握時就談定了，連線建立後再設定無法縮小一開始公告的視窗（`docs/DECISIONS.md` 的 `lag` 項）。因此不能重用 `tcp_connect_timeout`，要自己做 `getaddrinfo` / `socket` / `setsockopt` / `connect`。
- **細節**：
  - 只用 `getaddrinfo` 回傳的第一個位址，不像 `tcp_connect_timeout` 會輪流嘗試；解析失敗直接回 -1 且不印訊息。
  - `socket()` 的回傳值沒檢查；若失敗，後面 `connect` 會失敗並走「印錯誤、`close`、回 -1」的路徑。
  - `connect()` 是 blocking 且沒有逾時，與 `dial` 的 3 秒不同；連線失敗印 `evil: connect: <strerror>`。
  - 沒有設定 `TCP_NODELAY`（這個 mode 不依賴小封包的時機）。核心可能把 4096 調整後使用（Linux 對 `SO_RCVBUF` 有最小值並會加倍），重點是「很小」。
  - `freeaddrinfo(res)` 在成功與失敗路徑都會執行。
- **關係**：被誰呼叫 → `mode_lag`；呼叫 → libc 的 `getaddrinfo` / `socket` / `setsockopt` / `connect`。

### `mode_lag`
*位置：`tools/bench-evil.c:399`　簽章：`static int mode_lag(void)`*

- **用途**：R10（overrun）示範：以小接收緩衝區連線並 HELLO，之後 `--sec` 秒（預設 30）完全不讀 socket，接著讀 `--after` 秒（預設 5）並把收到的內容印出。回傳 0（連線失敗回 1）。
- **設計**：落後的客戶端 child 的讀取位置會落在環形緩衝區的 `tail` 之前（例如 `demo_r10.sh` 的 `--history 4K` 加上以 921600 baud 播放的無暫停日誌）。safe 伺服器偵測到 `my_offset < tail`，送 `EVENT DROPPED` 並跳到 `tail`；`--unsafe=overrun` 則繼續讀已被覆蓋的位元組，畫面上出現被攪亂的行或環形緩衝區的 record header。這個 mode 先「不讀」製造落後，再「讀」並印出結果，讓腳本比對 `lag.txt`。
  - HELLO 的 `replay` 為 0，因此歷史不會被回放，只看落後期間之後的資料。
  - 落後階段每秒 `sleep_ms(1000)` 一次；在剩餘整秒數是 10 的倍數時送 8 位元組的 `BN_PING`，使客戶端在伺服器 60 秒閒置限制（`IDLE_TIMEOUT_MS`）之內仍被視為活著，所以 `--sec` 超過 60 也能測。PONG 回覆不讀，留在接收緩衝區裡。
- **細節**：
  - 讀取階段每圈 `poll` 200 ms，有資料就 `bn_recv`；`rc != 1` 時印 `connection closed (...)`：`rc == -2` 顯示 `bn_err_name(perr)`，否則是 `EOF/error`，然後離開迴圈。
  - 讀取前印 `evil: reading again for N s:` 與一行 `-` 分隔線，讀完後再印一次分隔線；`demo_r10.sh` 以 `sed -n '/^-----/,$p'` 取分隔線之後的內容分析，所以這兩行的格式不能隨意改。
  - 計算 `(end - mono_ms()) / 1000 % 10` 用的是無號整數；若 `sleep_ms` 讓時間超過 `end`，差值會繞回很大的數，PING 是否送出就不確定。這只影響結尾那一圈的額外 PING，對整體流程沒有影響。
  - `print_live` 之後才 `fflush`，確保重導向到檔案時順序正確。
- **關係**：被誰呼叫 → `main`；呼叫 → `dial_small_rcvbuf`、`hello`、`print_live`、`lib/proto.c` 的 `bn_recv` / `bn_send` / `bn_err_name`、`sleep_ms`。

### `usage`
*位置：`tools/bench-evil.c:436`　簽章：`static void usage(FILE *f)`*

- **用途**：把用法說明印到 `f`（`-h` 時是 stdout，錯誤時是 stderr）。
- **設計**：目的檔案由呼叫者決定，這樣 `-h` 的輸出可以被管線接走，而錯誤用法不污染 stdout。
- **細節**：
  - 列出全部 mode、`--count`、`--sec`、`--after`、`--name`、`-h`。
  - `take-hold` 也使用 `--sec`（預設 30），但說明文字只提到 silent / flood / lag。
  - `--hold` 選項雖然被 `main` 接受（`opt_hold`），但從未被使用，用法也沒有列出。
- **關係**：被誰呼叫 → `main`；呼叫 → libc 的 `fprintf`。

### `main`
*位置：`tools/bench-evil.c:448`　簽章：`int main(int argc, char **argv)`*

- **用途**：解析選項與 `<mode> <host[:port]>`，做一次性初始化，依 mode 名稱呼叫對應函式並回傳其結果。
- **設計**：
  - 用 `getopt_long`，`optstring` 是 `"h"` 且開頭沒有 `+`，所以 GNU getopt 會把選項重排：選項可以放在位置參數之後，因此 `scripts/demo_r2.sh` 的 `bench-evil flood 127.0.0.1:$PORT --count 3 --sec 20` 才能運作。long option 用代碼 1-5 對應 `--count`、`--sec`、`--after`、`--name`、`--hold`，`-h` / `--help` 為 `'h'`。
  - `signal(SIGPIPE, SIG_IGN)`：`hello` / `bn_send` 走 `write()`，客戶端對已關閉的連線寫入會收到 SIGPIPE；evil 本來就是要面對會斷線的伺服器，不能自己先被殺掉。
  - `bench_log_set_role("evil")` 與 `bench_log_init_level(0)` 初始化函式庫的日誌：`bn_send` / `bn_recv` 內有 TRACE 日誌，必須讓 role 與等級有確定的值（等級預設 INFO，可用 `BENCH_LOG` 環境變數調整）。
  - `setvbuf(stdout, NULL, _IOLBF, 0)` 改成行緩衝：輸出被導向檔案或管線時（測試腳本與 demo 都這樣做）預設是全緩衝，行程被 `kill` 或 `_exit` 時輸出會丟失，並且會與背景程序的輸出順序錯亂。
- **細節**：
  - 數值選項用 `atoi`，不做錯誤檢查；`opt_count < 1` 被矯正為 1；`opt_sec` 預設 -1，由各 mode 決定「`> 0` 用它，否則預設 30」。
  - 未知選項或缺少參數（`default` 分支）→ `usage(stderr)`、回傳 1；`-h` → `usage(stdout)`、回傳 0。
  - `(void)opt_hold;` 只是為了不產生「未使用」警告。
  - 位置參數少於 2 個 → 用法、回傳 1。`parse_hostport` 的預設連接埠是 `BN_DEFAULT_PORT`（9000），格式錯誤印 `evil: bad address` 並回傳 1。
  - 分派表：`slow` → `mode_slow`；`bad-len` / `bad-magic` / `bad-crc` → `mode_bad(mode)`；`silent`、`rst`、`flood`、`take-and-die`、`lag`、`take-hold` 各自對應 `mode_*`。未知的 mode 印 `evil: unknown mode '<name>'` 與用法，回傳 1。
  - 不解析 `--unsafe`，所以 `bench_unsafe_mask` 在這個行程中保持 0。
  - 回傳值就是 mode 函式的回傳值，作為行程結束碼；`scripts/test.sh` 與 demo 主要看伺服器狀態與輸出內容，不直接依賴它。
- **關係**：被誰呼叫 → 作業系統（`bench-evil` 執行檔進入點）；呼叫 → `usage`、`lib/util.c` 的 `parse_hostport` / `bench_strlcpy`、`lib/log.c` 的 `bench_log_set_role` / `bench_log_init_level`、各個 `mode_*`。

---

## `tools/benchd.lua`

這個檔案是 Wireshark / tshark 用的 Lua dissector，把 benchd 的二進位協定（`include/bench_proto.h`，spec §6）從 TCP 位元組流解成可讀的封包樹與 Info 欄。它不屬於 benchd 的任何執行期行程（master / reader / per-client child / bench client 都不載入它），而是離線分析工具：由 `wireshark -X lua_script:tools/benchd.lua captures/session.pcap` 或 `tshark -X lua_script:tools/benchd.lua -r ...` 載入，在 Wireshark 自己的行程裡執行。`scripts/capture.sh`（第 15 行定義 `LUA=$ROOT/tools/benchd.lua`，第 48-49 行呼叫 tshark）用它印出 `captures/session.pcap` 的摘要。它只依賴 Wireshark 的 Lua API（`Proto`、`ProtoField`、`DissectorTable`、`NSTime`、`pinfo.desegment_*`），不依賴 repo 內任何 C 程式碼；但所有常數（type、error code、event code、payload 偏移與長度）都必須與 `include/bench_proto.h` 手動保持一致。程式碼使用 Lua 5.3 的原生位元運算子（`>>`、`~`、`&`），因此需要支援這些語法的 Lua 版本才能載入。

整個檔案的結構由上到下是：協定與查表定義（第 10-66 行）、CRC32 表與 `crc32()`（第 68-84 行）、輔助函式 `ns_time()`、`printable()`（第 86-97 行）、payload 解碼 `payload()`（第 99-152 行）、進入點 `benchd.dissector()`（第 154-196 行）、port 註冊（第 198 行）。

兩個貫穿全檔的機制先在此說明，各函式再引用：

**Desegmentation（TCP 重組）**。benchd 在 TCP 上跑的是自訂的 length-prefixed 框架（12 byte header 內有 `len`），而 TCP 不保留訊息邊界，所以一個 TCP segment 可能含好幾個 benchd 封包，一個封包也可能橫跨好幾個 segment（`bench-evil slow` 模式即是每 50 ms 送 1 byte，見 `tools/bench-evil.c:4`，對應 R4 `readn`）。dissector 在 `benchd.dissector` 內用 `while off < total` 迴圈逐個封包切；當剩餘位元組不足以構成完整封包時，不去解半個封包，而是設定 `pinfo.desegment_offset`（未完成封包的起點）與 `pinfo.desegment_len`（還缺多少）後離開迴圈，讓 Wireshark 的 TCP 層保留該段位元組、等後續 segment 到齊後以重組後的 tvb 再呼叫一次 dissector。有兩個請求點：(1) 剩餘不足 12 byte，連 `len` 都讀不到，只能要 `DESEGMENT_ONE_MORE_SEGMENT`；(2) header 完整、`len` 已知但 payload 不夠，精確要 `12 + len - avail` 個位元組。此機制要有效，TCP 層的 `tcp.desegment_tcp_streams` 必須開啟，`scripts/capture.sh:48` 明確傳了 `-o tcp.desegment_tcp_streams:TRUE`。

**CRC 檢查**。每個封包 header 的第 8-11 byte 是 payload 的 CRC32（只涵蓋 payload，不含 header；IEEE 802.3，反射多項式 0xEDB88320，與 `lib/crc32.c` 的 `bench_crc32()` 及 zlib 相同）。dissector 在確認整個封包已在 tvb 內之後，才對 `buf(off + 12, len)` 重新計算 CRC，與 header 中的值比對。規則與 C 端一致：`len == 0` 時 CRC 欄必須是 0（`lib/proto.c:52` 發送端 `len ? bench_crc32() : 0`，`lib/proto.c:129` 接收端同樣處理）；`len > 0` 時必須等於 `crc32(payload)`。結果放在衍生欄位 `benchd.crc_ok`，不符時另掛 `PI_CHECKSUM / PI_ERROR` 專家資訊 "CRC32 mismatch"。這必須放在 desegmentation 之後，因為 payload 沒收齊就無法算 CRC。我在 `captures/session.pcap` 上實測：`-Y "benchd.crc_ok==1"` 命中 53 個 frame，`benchd.crc_ok==0` 為 0 個。

### `benchd`
*位置：`tools/benchd.lua:10`、`tools/benchd.lua:198`　簽章：`static Proto benchd  /* file-scope local，非函式 */`*

- **用途**：用 `Proto("benchd", "benchd remote serial console")` 建立協定物件，是整個腳本唯一對 Wireshark 公開的東西：欄位掛在它的 `fields` 表上，進入點 `benchd.dissector` 是它的成員，封包樹節點也以它為型別（`root:add(benchd, ...)`）。第 198 行再用 `DissectorTable.get("tcp.port"):add(9000, benchd)` 把 TCP 9000 預設綁給它。
- **設計**：port 9000 對應 `BN_DEFAULT_PORT`（`include/bench_proto.h:26`）。但 server 的 port 可以用 `PORT` 環境變數改（`scripts/lib.sh:11`），所以預設綁定只是方便手動開 pcap；`scripts/capture.sh:48-49` 一律傳 `-d tcp.port==$PORT,benchd`，不依賴這一行。註冊放在檔尾，等 `benchd.dissector` 與所有欄位都定義完才執行。
- **細節**：協定名 "benchd" 同時是 display filter 的前綴（`benchd.type`、`benchd.crc_ok`）。本物件與 `--unsafe=<switch>` 無關：dissector 是離線被動工具，不讀 R1-R12。
- **關係**：被誰使用 → `f`（`benchd.fields`）、`benchd.dissector`。對照 → `scripts/capture.sh:48-49`。

### `types`
*位置：`tools/benchd.lua:12-18`　簽章：`static table types  /* [u8 type] = "NAME" */`*

- **用途**：封包 type 數值到名稱的查表，共 18 項：client→server 為 `0x01 HELLO, 0x02 INPUT, 0x03 TAKE, 0x04 RELEASE, 0x05 INFO_REQ, 0x06 BAUD, 0x07 PING, 0x08 BYE`；server→client 為 `0x81 WELCOME, 0x82 DATA, 0x83 EVENT, 0x84 REPLAY_BEGIN, 0x85 REPLAY_END, 0x86 CTRL_RESULT, 0x87 INFO, 0x88 STATUS, 0x89 ERROR, 0x8A PONG`，與 `enum bn_type`（`include/bench_proto.h:31-39`）逐項相同。
- **設計**：同一張表有兩個消費者。一是 `f.type` 的 value-string（第 33 行），讓封包樹顯示 `HELLO (0x01)`；二是 `benchd.dissector` 第 178 行以 `types[t]` 取名字，用於子樹標籤與 Info 欄。兩處共用一份，名稱不會不一致。方向（client 或 server 送出）Lua 端不判斷，由 Wireshark 顯示的 src/dst port 表示。
- **細節**：表與 C 端要手動同步，沒有編譯期檢查。查不到時 `benchd.dissector` 退回 `string.format("0x%02x", t)`（第 178 行），所以漏改只會顯示十六進位，不會丟例外。
- **關係**：被誰使用 → `f.type`、`benchd.dissector`。對照 → `include/bench_proto.h` 的 `enum bn_type`、`lib/proto.c` 的 `bn_type_name()`。

### `errors`
*位置：`tools/benchd.lua:19-23`　簽章：`static table errors  /* [u16 code] = "NAME" */`*

- **用途**：ERROR 封包 `code` 的名稱查表，12 項（1 `BAD_MAGIC` 到 12 `BAD_REQUEST`），對應 `enum bn_err`（`include/bench_proto.h:41-46`）。其中 12 `BAD_REQUEST` 是擴充：請求被拒但連線保持開啟（見該 enum 的註解）。
- **設計**：同時作 `f.errcode` 的 value-string（第 66 行）與 `payload()` 第 147 行 Info 摘要的來源，所以抓包時 Info 欄直接寫 `ERROR BAD_CRC` 之類，不必對照數值。
- **細節**：查不到的 code 在 Info 欄顯示 `?`（第 147 行），不丟 Lua 例外。
- **關係**：被誰使用 → `f.errcode`、`payload()`。對照 → `lib/proto.c` 的 `bn_err_name()`。

### `events`
*位置：`tools/benchd.lua:24-27`　簽章：`static table events  /* [u16 code] = "NAME" */`*

- **用途**：EVENT 封包 `code` 的名稱查表，7 項：`1 DEVICE_LOST, 2 DEVICE_BACK, 3 CTRL_TAKEN, 4 CTRL_RELEASED, 5 BAUD_CHANGED, 6 DROPPED, 7 SERVER_SHUTDOWN`，對應 `enum bn_evt`（`include/bench_proto.h:48-51`，C 端常數前綴為 `EV_`，這裡省略）。
- **設計**：作 `f.evcode` 的 value-string（第 56 行）與 `payload()` 第 127 行 Info 摘要來源。EVENT 與 DATA 共用 `offset`、`ts_ns` 開頭，Info 欄上的 `EVENT CTRL_TAKEN: ...` 因此能和前後的 DATA 對照時序。
- **細節**：查不到時顯示 `?`（第 127 行）。
- **關係**：被誰使用 → `f.evcode`、`payload()`。對照 → `lib/proto.c` 的 `bn_evt_name()`。

### `devstate`
*位置：`tools/benchd.lua:28`　簽章：`static table devstate  /* {[0]="LOST", [1]="UP"} */`*

- **用途**：裝置狀態查表，對應 C 端匿名 enum `DEV_LOST = 0`、`DEV_UP = 1`（`include/bench_proto.h:53`）。
- **設計**：只給 `f.devstate`（第 46 行）當 value-string，WELCOME 與 STATUS 共用同一個欄位。這個欄位讓抓包時能直接看出「裝置掉了」，不必另外從 EVENT 推斷。
- **細節**：只有兩個值，沒有 `?` 退路；其他值由 Wireshark 自己的 value-string 機制顯示為 Unknown。
- **關係**：被誰使用 → `f.devstate`。

### `f`
*位置：`tools/benchd.lua:30-66`　簽章：`static table f = benchd.fields; /* 36 個 ProtoField */`*

- **用途**：`local f = benchd.fields` 之後以 `f.xxx = ProtoField...` 賦值，把 36 個欄位註冊進 `benchd` 協定。欄位名稱一律為 `benchd.<名稱>`，可直接用於 display filter（例如 `benchd.type == 0x82`、`benchd.crc_ok == 0`）。
- **設計**：
  - 型別對應 C 結構：u8 → `uint8`、u16 → `uint16`、u32 → `uint32`、u64 → `uint64`、定長 char 陣列 → `string`。`include/bench_proto.h` 註明多位元組欄位為大端序（網路位元組序），與 Wireshark `uint*` 欄位預設讀法一致，所以 `tree:add(f.xxx, 範圍)` 不需要自己轉位元組序。
  - 欄位重用：同一個欄位物件被多種封包共用，不為每個 type 另設一份：`f.ver`（header 與 WELCOME 的 `proto_ver`）、`f.baud`（BAUD、WELCOME、STATUS）、`f.viewers`（WELCOME、STATUS）、`f.devstate`（WELCOME、STATUS）、`f.ctrlname`（WELCOME、STATUS）、`f.head`（WELCOME、REPLAY_END）、`f.offset`/`f.ts`（DATA、EVENT）、`f.idle`（CTRL_RESULT 的 `holder_idle_sec`、STATUS 的 `ctrl_idle_sec`）、`f.cts`（PING、PONG）、`f.data`（INPUT、DATA）、`f.text`（EVENT、INFO、ERROR）。代價是標籤必須夠泛用，例如 `Holder idle (s)`。
  - `f.crc_ok`（第 36 行）是 `bool`，不對應任何線上位元組，由 `benchd.dissector` 計算後以 `set_generated()` 加入。`f.ts`（第 55 行）是 `absolute_time`，值由 `ns_time()` 轉出。`f.resume` 是 `uint8` 而非 `bool`，與 C 端 `uint8_t has_resume` 一致。`f.evcode`、`f.errcode` 為 `uint16`，掛 `events`、`errors`。
  - 64 位元的奈秒計數（`start`、`laststart`、`cts`、`sts`、`resoff`）以 `uint64` 顯示原始值，只有 DATA/EVENT 的 `ts` 轉成時間格式。
- **細節**：C 端 struct 裡的 `pad[]` 欄位沒有對應 `ProtoField`，dissector 直接略過。欄位表要與 `include/bench_proto.h` 手動同步。
- **關係**：被誰使用 → `payload()`、`benchd.dissector`。對照 → `include/bench_proto.h` 各 `struct bn_*`。

### `crc_table`
*位置：`tools/benchd.lua:69-76`　簽章：`static uint32_t crc_table[0..255]  /* 載入腳本時建好 */`*

- **用途**：CRC32 的 256 項查表，供 `crc32()` 使用。
- **設計**：腳本載入時以頂層迴圈一次建好，之後只讀。C 版 `lib/crc32.c` 用 `crc_ready` 旗標在第一次呼叫時才初始化，這裡不需要，因為載入階段只跑一次。每個 `i` 做 8 次：最低位為 1 則 `(c >> 1) ~ 0xEDB88320`，否則 `c >> 1`；`c % 2 == 1` 等價於 C 的 `c & 1`。Lua 整數是 64 位元，但 `c` 起始小於 2^32，右移與對 0xEDB88320 的 XOR 不會超出 32 位元，所以不必遮罩。
- **細節**：`~` 是 Lua 5.3 的二元 XOR，舊版 Lua 無此語法，整個腳本會在載入時語法錯誤。
- **關係**：被誰使用 → `crc32`。對照 → `lib/crc32.c` 的 `crc_init()`。

### `crc32`
*位置：`tools/benchd.lua:77`（CRC 表建構迴圈在 `tools/benchd.lua:68-76`）　簽章：`static uint32_t crc32(TvbRange tvbr)`*

- **用途**：對一段 tvb 範圍（封包的 payload）計算標準 CRC32（IEEE 802.3，與 zlib `crc32()`、`lib/crc32.c` 的 `bench_crc32()` 結果相同），回傳 32 位元無號整數（以 Lua 整數表示）。
- **設計**：Wireshark 內建的 Lua API 沒有現成的 CRC32 函式可用於 `TvbRange`，所以自己實作。採查表法（每 byte 一次查表加位移），與 C 版同一個演算法。重點：
  - 表 `crc_table[0..255]` 在腳本載入時（top-level 迴圈，第 69-76 行）一次建好，之後每次呼叫只讀表；C 版是第一次呼叫時才惰性初始化（`lib/crc32.c` 的 `crc_ready`），這裡不需要，因為腳本載入只執行一次。
  - 建表：對每個 `i` 做 8 次「若最低位為 1 則 `(c >> 1) ~ 0xEDB88320`，否則 `c >> 1`」。`c % 2 == 1` 等價於 C 版的 `c & 1`。Lua 的整數是 64 位元，但 `c` 起始小於 2^32，右移與 XOR 0xEDB88320 都不會超出 32 位元，所以不必另外遮罩。
  - 計算：初值 `0xFFFFFFFF`，每 byte `c = crc_table[(c ~ b) & 0xFF] ~ (c >> 8)`，最後 `c ~ 0xFFFFFFFF`（初值與結尾反相，標準 CRC-32 的規定）。
- **細節**：
  - `tvbr:bytes()` 先把整段 payload 複製成 `ByteArray`，再以 `get_index(i)`（0-based）逐 byte 取值；payload 最大 65536 byte，迴圈在 Lua 中逐 byte 跑，對分析工具來說可以接受。
  - 只被「`len > 0`」的路徑呼叫；`len == 0` 時由 `benchd.dissector` 直接要求 CRC 欄為 0，不走這個函式（空資料的 CRC32 本來就是 0，但 C 端明確用 0）。
  - 使用 Lua 5.3 的 `>>`、`~`（二元 XOR）、`&`；舊版 Lua 無此語法，整個腳本會在載入時語法錯誤。
  - 沒有 `--unsafe` 相關行為。`--unsafe=validate`（R3）讓 benchd 自己跳過 CRC 檢查，但 dissector 仍然會檢查——因為它看的是線上封包，用來證明封包的 CRC 是否正確（例如 `bench-evil bad-crc` 會把 CRC 的第一個 byte XOR 0xFF，`tools/bench-evil.c:158`）。
- **關係**：被誰呼叫 → `benchd.dissector`（`tools/benchd.lua:184`，唯一呼叫點）。對應 → `lib/crc32.c` `bench_crc32()`；比對的另一端是 `lib/proto.c` 的 `bn_recv()` CRC 檢查（第 128-134 行，受 `UNSAFE(R_VALIDATE)` 控制）。呼叫 → `ByteArray:get_index`、`TvbRange:bytes`（Wireshark API）。

### `ns_time`
*位置：`tools/benchd.lua:86`　簽章：`static NSTime ns_time(TvbRange tvbr)`*

- **用途**：把 8 byte 的 64 位元「自 epoch 起算的奈秒數」轉成 Wireshark 的 `NSTime`（秒 + 奈秒），給 `absolute_time` 欄位 `f.ts` 顯示。
- **設計**：DATA 與 EVENT 的 `ts_ns` 是單一 64 位元整數（奈秒），但 Wireshark 的 `absolute_time` 欄位需要 `NSTime`。因此用 `tvbr:uint64()` 取得 `UInt64` 物件，再以 `UInt64` 的 `/` 與 `%`（整數除法與取餘）拆成「秒」與「秒內奈秒」，用 `:tonumber()` 轉成 Lua 數字，最後 `NSTime.new(secs, nsec)`。用 `UInt64` 物件的運算子做拆分，是因為 `tvbr:uint64()` 回傳的就是 `UInt64`；拆完後的「秒」與「秒內奈秒」都遠小於 2^53，才用 `:tonumber()` 轉成一般數字。
- **細節**：
  - 輸入必須是剛好 8 byte 的範圍；呼叫端 `p(8, 8)` 保證這一點，且 `payload()` 在呼叫前已用 `len >= 16`（DATA）、`len >= 20`（EVENT）檢查過。
  - 除數與模數都是常數 `1000000000`（每秒奈秒數）。
  - 結果在 `tree:add(f.ts, p(8, 8), ns_time(p(8, 8)))` 以第三個參數明確傳入，使 Wireshark 顯示這個自訂換算出的時間，而不是自行把這 8 byte 解釋為別種時間編碼。
  - 時間值是 server 端 ring record 建立時的時戳（`server/ring.c:66` 的 `now_ns()`，由 `server/session.c` 的 `send_record()` 填入 `bn_data_hdr.ts_ns`／`bn_event_hdr.ts_ns`；`DROPPED` 事件則由 `send_dropped()` 在送出時以 `now_ns()` 填入），不是封包捕獲時間。
- **關係**：被誰呼叫 → `payload()`（DATA 分支第 120 行、EVENT 分支第 124 行）。呼叫 → `NSTime.new`、`TvbRange:uint64`（Wireshark API）。

### `printable`
*位置：`tools/benchd.lua:93`　簽章：`static string printable(string s, int max)`*

- **用途**：把一段（可能含換行、控制字元的）裝置輸出轉成適合放進單行 Info 欄的字串，並截短到 `max` 個位元組。
- **設計**：UART 的 DATA/INPUT 內容常含 `\r\n`、ESC 序列，直接放進 Info 欄會破壞欄位顯示，所以：先把 `\r` 改成兩個字元 `\r`、`\n` 改成兩個字元 `\n`（讓換行仍可辨識），再把其餘所有控制字元（`%c`，如 ESC、NUL、TAB）換成 `.`。順序很重要——必須先處理 `\r\n` 再處理 `%c`，否則兩者都會被 `.` 吃掉而無法看出換行。最後若長度超過 `max` 就截斷並附上 `…`。
- **細節**：
  - 以 `s:gsub(...):gsub(...):gsub(...)` 串接；`gsub` 回傳「字串, 取代次數」兩個值，但串接時中間的呼叫只取第一個值，最後 `s = ...` 也只接第一個值，取代次數被丟掉。
  - 長度判斷與 `s:sub(1, max)` 都以位元組計，且是在 `\r\n` 展開成兩個字元「之後」才計算，所以實際顯示的原始內容會少於 `max`。若截斷點落在多位元組 UTF-8 字元中間，會切出不完整字元（本函式沒有處理）。
  - 附加的 `…` 是 UTF-8（3 byte）字元，使結果長度可略大於 `max`。
  - 呼叫端傳的上限：INPUT 為 30（第 108 行）、DATA 為 40（第 122 行）。實測 tshark 顯示 `DATA "U-Boot SPL 2014.07 (benchsim built-in ge…"`。
  - 不涉及 `--unsafe`。
- **關係**：被誰呼叫 → `payload()`（INPUT 第 108 行、DATA 第 122 行）。

### `payload`
*位置：`tools/benchd.lua:100`　簽章：`static string payload(int t, Tvb buf, int off, int len, TreeItem tree)`*

- **用途**：依封包 type `t` 把 payload（從 `buf` 的 `off` 起、長 `len` byte）逐欄加進封包樹 `tree`，回傳放進 Info 欄的簡短摘要（沒有摘要則回傳空字串）。
- **設計**：用一條 `if / elseif` 鏈對 type 分派，每個分支的欄位偏移與長度必須逐項對應 `include/bench_proto.h` 的 packed struct。分支條件除了 `t == 0x..` 之外還加上 `len >= N`（N 是該 struct 的 `sizeof`），目的是容錯：payload 若比預期短（例如被截斷的捕獲、或由 `bench-evil` 這類惡意客戶端送出的假封包），就跳過解欄位而不是對超出範圍的 tvb 取值而引發 Wireshark 例外。條件失敗時，鏈上其他分支也都不符合（因為 `t` 不同），最後落到第 151 行 `return ""`，該封包只顯示 header。內部區域函式 `p(o, n)` 是 `buf(off + o, n)` 的縮寫，使各分支寫成相對 payload 起點的偏移，與 struct 的欄位偏移一致。
- **細節**：各 type 的處理（偏移都是相對 payload 起點；「長度條件」即該分支的 `len >=`）：
  - `0x01 HELLO`（`len >= 56` = `sizeof(bn_hello)`）：`name`（0，32 byte）、`replay_bytes`（32）、`has_resume`（36）、`resume_offset`（40，8 byte）、`last_server_start_ns`（48，8 byte）。摘要 `name=<stringz>`，`stringz()` 在第一個 NUL 處截斷。37-39 為 `pad`，不顯示。
  - `0x02 INPUT`（無長度條件）：整段加為 `f.data`，摘要為 `"printable(內容, 30)"`。`len > 0 and ... or ""` 裡的 `len > 0` 判斷其實多餘，因為 `benchd.dissector` 只在 `len > 0` 時才呼叫本函式。
  - `0x06 BAUD`（`len >= 4`）：`baud`，摘要是波特率數字。
  - `0x07 PING`（`len >= 8`）：`client_ts_ns`；沒有摘要回傳（此分支沒有 `return`，落到最後的 `return ""`）。
  - `0x81 WELCOME`（`len >= 208` = `sizeof(bn_welcome)`）：`proto_ver`（0，與 header 共用 `f.ver`）、`dev_state`（1）、`baud`（4）、`viewers`（8）、`history_cap`（12）、`head`（16）、`server_start_ns`（24）、`dev_path`（32，128 byte）、`ctrl_name`（160，32 byte）、`server_version`（192，16 byte）。摘要 `dev=<路徑> viewers=<數>`。
  - `0x82 DATA`（`len >= 16` = `sizeof(bn_data_hdr)`）：`offset`（0）、`ts_ns`（8，用 `ns_time` 轉換）；若 `len > 16`，其後 `len - 16` byte 為裝置原始資料，加為 `f.data`，摘要 `"printable(資料, 40)"`。`offset` 欄位顯示的是 record 結尾的邏輯 ring 偏移（見 `docs/DECISIONS.md` Protocol 一節）。
  - `0x83 EVENT`（`len >= 20` = `sizeof(bn_event_hdr)`）：`offset`、`ts_ns`、`code`（16，u16），若 `len > 20`，其後為 UTF-8 文字 `f.text`。摘要 `<事件名>[: 文字]`（查不到事件名為 `?`）。實測：`EVENT CTRL_TAKEN: control taken by capture`。
  - `0x84 REPLAY_BEGIN`（`len >= 16`）：`from_offset`、`dropped_bytes`。沒有摘要。
  - `0x85 REPLAY_END`（`len >= 8`）：`head`（與 WELCOME 共用 `f.head`）。沒有摘要。
  - `0x86 CTRL_RESULT`（`len >= 44` = `sizeof(bn_ctrl_result)`）：`granted`（0）、`holder_idle_sec`（4，用 `f.idle`）、`auto_release_in_sec`（8，`f.autorel`）、`holder`（12，32 byte）。摘要 `granted|denied holder=<名字>`，判斷是 `granted` byte 是否等於 1。`--unsafe=ctrl-release`（R6）時 server 會送 `auto_release_in = UINT32_MAX`（`docs/DECISIONS.md`），dissector 只當作一般 u32 顯示，不特別轉成 "never"。
  - `0x87 INFO`（無長度條件）：整段加為 `f.text`（key=value 文字），摘要 `<len> bytes of key=value`。
  - `0x88 STATUS`（`len >= 56` = `sizeof(bn_status)`）：`dev_state`（0）、`you_are_ctrl`（1）、`baud`（4）、`viewers`（8）、`ctrl_idle_sec`（12，用 `f.idle`）、`rx_bytes`（16）、`ctrl_name`（24，32 byte）。摘要 `viewers=<數> ctrl=<名字>`。
  - `0x89 ERROR`（`len >= 4` = `sizeof(bn_error_hdr)`）：`code`（0）；若 `len > 4`，其後是錯誤文字 `f.text`。摘要為錯誤名稱（`BAD_CRC` 等，未知為 `?`）。
  - `0x8A PONG`（`len >= 16`）：`client_ts_ns`、`server_ts_ns`。沒有摘要。
  - 沒有分支的 type（TAKE、RELEASE、INFO_REQ、BYE 等 payload 為空的封包，以及未知 type）：直接回傳 `""`。
  - 所有分支都只讀取 struct 定義內的位元組；若 payload 比 struct 長，多出的部分不解碼（INPUT/DATA/EVENT/INFO/ERROR 的變長尾端例外，它們會取到 `len` 結尾）。
  - 摘要若為空字串，呼叫端不會在 Info 欄加空格。
  - 不涉及 `--unsafe`；惡意封包（`--unsafe=validate` 下 server 不會擋的）若落在這裡只是欄位長度不足而被跳過。
- **關係**：被誰呼叫 → `benchd.dissector`（`tools/benchd.lua:187`，唯一呼叫點，僅在 `len > 0` 時）。呼叫 → `ns_time`、`printable`，以及 Wireshark 的 `TreeItem:add`、`TvbRange:string/stringz/uint`。資料來源 → `f.*` 欄位、`events`、`errors`。對照 → `include/bench_proto.h` 各 `struct bn_*`。

### `benchd.dissector`
*位置：`tools/benchd.lua:154`　簽章：`int benchd_dissector(Tvb buf, Pinfo pinfo, TreeItem root)`*

- **用途**：Wireshark 對每個 TCP 9000 的 segment（或經 TCP 層重組後的資料）呼叫的進入點：把 `buf` 內連續的 benchd 封包逐個切開、驗證、掛到封包樹並彙總 Info 欄；資料不夠一個完整封包時，要求 Wireshark 做 TCP 重組。回傳已消耗的位元組數 `off`。
- **設計**：整個函式是一個 `while off < total` 迴圈，每圈處理一個封包，順序是「先確認資料夠，再解碼」，原因是後面的步驟（讀 `len`、算 CRC、解 payload）都需要資料已經在 tvb 內：
  1. **不足 12 byte**（第 158-162 行）：header 還沒到齊，讀不到 `len`，設 `pinfo.desegment_offset = off`、`pinfo.desegment_len = DESEGMENT_ONE_MORE_SEGMENT`，`break`。`desegment_offset` 設為 `off`（而非 0），使 Wireshark 知道此點之前的位元組已在本次呼叫解完，只保留 `off` 之後的部分等後續 segment。
  2. **magic 不符**（第 163-166 行）：`buf(off, 2):uint() ~= 0x424E`（`BN_MAGIC`）表示這不是 benchd 的框架（例如擷取從連線中途開始，或位元組流已失去同步）。把從 `off` 到結尾的整段掛成一個 benchd 節點（`root:add(benchd, buf(off))`），並在其標籤後 `append_text(" [not benchd framing]")`，然後 `break`；不嘗試重新同步，因為沒有可靠的同步標記，亂切會產生更多假封包。
  3. **`len` 過大**（第 167-171 行）：`len > 65536`（`BN_MAX_PAYLOAD`）就只掛 12 byte header 節點，標註 `[length N exceeds limit]`，`break`。這一步必須在要求重組之前做，否則一個偽造的巨大 `len`（例如 `bench-evil bad-len` 的 1 MiB，`tools/bench-evil.c:162-172`，或亂碼造成的 4 GiB）會讓 Wireshark 一直等永遠收不到的資料。上限與 C 端 `lib/proto.c:110` 的 `h->len > BN_MAX_PAYLOAD || h->len > cap` 中的 `BN_MAX_PAYLOAD` 相同（等於 65536 仍接受；C 端另外還受呼叫者緩衝區 `cap` 限制，dissector 沒有這項）。C 端這項檢查（連同 magic、ver）整段包在 `if (!UNSAFE(R_VALIDATE))` 內（`lib/proto.c:101`），R3 unsafe 時 server 不擋；dissector 是離線工具，不受影響，仍會擋。
  4. **payload 不足**（第 172-176 行）：`avail < 12 + len` 時設 `desegment_offset = off`、`desegment_len = 12 + len - avail`（精確缺少的位元組數）並 `break`。
  5. **解碼**（第 177-189 行）：type 取 `buf(off + 3, 1)`，名稱查 `types`，查不到用 `0x%02x`。以 `root:add(benchd, buf(off, 12 + len), "benchd " .. name)` 建立一個涵蓋整個封包（header + payload）的子樹，之後依序加入 `magic / ver / type / len / crc` 五個 header 欄位。
  6. **CRC 檢查**（第 182-186 行）：`ok = (len == 0 and crc == 0) or (len > 0 and crc32(payload) == crc)`。`tree:add(f.crc_ok, ok):set_generated()` 加入布林欄位並標為「衍生」（沒有對應的線上位元組，Wireshark 標示為衍生欄位）。若不符，對 CRC 欄項目 `add_expert_info(PI_CHECKSUM, PI_ERROR, "CRC32 mismatch")`，於專家資訊視窗和封包樹以錯誤顯示。CRC 錯誤的封包仍會繼續解碼 payload 並前進到下一個封包：CRC 只涵蓋 payload、不含 header，不影響封包邊界，而此時 magic 與 `len` 上限已通過、整個封包也已在 tvb 內，用 `len` 往前切是安全的；dissector 的目的就是把壞封包標出來，而不是丟棄。
  7. **摘要與前進**（第 187-189 行）：僅在 `len > 0` 時呼叫 `payload()`；`info[#info+1]` 收集 `"<名稱> <摘要>"`（摘要空則只放名稱）；`off = off + 12 + len` 進入下一圈。
  - 迴圈結束後（第 191-194 行）若 `info` 非空，才設 `pinfo.cols.protocol = "BENCHD"` 與 `pinfo.cols.info = table.concat(info, " | ")`：一個 frame 內的多個封包以 ` | ` 串在同一個 Info 欄。若本次呼叫只有「等待重組」而沒解出任何封包，欄位維持 Wireshark 原本的 TCP 顯示，等重組完成的那個 frame 再填入。
- **細節**：
  - **回傳值**：`off`，即已完整解碼的位元組數；重組請求時就是未完成封包的起點。magic 不符或 `len` 超限而 `break` 時，`off` 是該壞封包的起點（不含它）。
  - **版本欄位不檢查**：`ver` 只顯示不驗證（C 端 `lib/proto.c:106` 會回 `BAD_VERSION`）；因此錯誤版本的封包仍會被解碼。type 不在 `types` 表中的封包也照常切分，只是名稱顯示十六進位。
  - **每次呼叫從 `off = 0` 開始**：Wireshark 對每個 segment（或重組後的資料）獨立呼叫一次，所以擷取檔若從連線中途開始，第一個 segment 會顯示 `[not benchd framing]`，dissector 不會跨呼叫保留狀態。
  - **一個 segment 多個封包**：由迴圈處理，這是 benchd 常見情形，因為 `bn_send` 每個封包只做一次 `write()`（`docs/DECISIONS.md` Protocol），但 TCP 可把相鄰的 `write()` 合併成一個 segment。我手上的 `captures/session.pcap` 中沒有出現多封包 frame，這條路徑只有從程式碼讀出，未用實際擷取驗證。
  - **一個封包跨 segment**：由 `desegment_*` 處理。最典型的來源是 R4 的 `bench-evil slow`（1 byte / 50 ms）和超過 MSS 的 DATA/INFO 封包。
  - **與 `--unsafe` 的關係**：dissector 本身沒有開關；它是用來觀察 R3、R4 現象的：`bench-evil bad-magic` 會觸發 `[not benchd framing]` 分支，`bad-len` 會觸發 `[length 1048576 exceeds limit]`（`len=0x00100000`），`bad-crc` 會顯示 `benchd.crc_ok == 0` 與 `CRC32 mismatch` 專家資訊。這三者在 `--unsafe=validate` 下 server 不會擋，但 dissector 都會標出來。
  - 限制：只掛在 TCP port 上，不判斷封包方向；方向由 Wireshark 顯示的 src/dst port 提供。
- **關係**：被誰呼叫 → Wireshark / tshark 的 TCP dissector 透過 `DissectorTable "tcp.port"` 第 198 行的註冊（9000）或 `-d tcp.port==PORT,benchd`（`scripts/capture.sh:49`）。呼叫 → `crc32`（第 184 行）、`payload`（第 187 行），以及 `types`、`f.*`。對應 C 端 → `lib/proto.c` 的 `bn_recv`（同一份格式的接收端驗證）。

---

## `scripts/lib.sh`

這個檔案是所有 `scripts/*.sh` 共用的 bash 函式庫，不能單獨執行，只能被 `. "$(dirname "$0")/lib.sh"` 載入（檔頭第 1 行註解）。它由「工具行程」，也就是正在跑 `test.sh`、`stress.sh`、`capture.sh` 或 `demo_rN.sh` 的那個 bash 本身執行，不屬於 benchd 的 master、reader 或 child。它負責三件事：載入時設定環境變數與工作目錄（第 9-23 行）、註冊結束時清理背景行程的 trap（第 37-38 行）、提供啟動／停止 `benchsim` 與 `benchd`、查詢伺服器狀態和印出彩色結果的函式。它依賴 `build/bin` 內的四個執行檔 `benchd`、`bench`、`benchsim`、`bench-evil`，以及系統工具 `ss`、`pgrep`、`ps`、`awk`、`sed`、`getconf`。

載入時（不是函式，但後面所有函式都依賴它）會做下列事情：

- **環境旋鈕**（第 3-7 行註解）：`BIN`（執行檔目錄，預設 `$ROOT/build/bin`）、`PORT`（預設 9000）、`DEV`（benchsim 的 pty symlink，預設 `/tmp/benchsim0`）、`WORK`（輸出目錄，預設 `$ROOT/out/<script 名稱>`）。全部用 `${VAR:-default}` 寫成，所以呼叫端可以在 `source` 之前先設定：`test.sh` 先把 `PORT` 設成 19400、`DEV` 設成 `/tmp/benchsim-test-$$`，`demo_r7.sh` 先把 `BIN` 指到 `build/debug/bin`。預設值固定，所以兩個 demo 不能同時跑（會搶同一個 `PORT` 與 `DEV`），要同時跑必須自己覆寫。
- `ROOT` 由 `${BASH_SOURCE[0]}` 推出（第 9 行），不受目前工作目錄影響；`SCRIPT_NAME` 取 `$0`（即主 script，不是 `lib.sh`）去掉 `.sh`（第 13 行），所以每個 script 的輸出都在 `out/<自己的名稱>/`。
- `LOG_SIM`（`$WORK/benchsim.log`）與 `LOG_D`（`$WORK/benchd.log`）每次載入都被 `: >` 清空（第 17-19 行），因此 demo 末尾 grep 日誌只會看到這一次執行的內容。
- 全域狀態：`BG_PIDS`（需要在結束時殺掉的背景 pid 陣列）、`SIM_PID`、`D_PID`（benchd master 的 pid，也是 `pgrep -P` 找 child 的父行程）。

### `c_head`
*位置：`scripts/lib.sh:25`　簽章：`c_head <文字...>`*

- **用途**：印出一個區段標題：先空一行，再以粗體青色（`ESC[1;36m`）印 `== <文字>`。
- **設計**：腳本輸出很長（尤其是 `test.sh` 與 demo），區段標題讓人一眼看出每個步驟；前置空行把區塊分開。用 `$*` 把所有參數接成一個字串，所以呼叫端可以不加引號傳多個詞，或像大多數呼叫那樣傳一個帶 `$(...)` 展開的引號字串。
- **細節**：
  - 逸出碼無條件輸出，沒有檢查 stdout 是否為 tty；重導向到檔案時檔案裡會有原始的 `\033[...m`。
  - 格式字串固定、使用者文字走 `%s`，所以文字內容中的 `%` 或反斜線不會被 `printf` 當成格式。
  - 標題文字中的 `$(...)` 是在 `c_head` 被呼叫之前就由呼叫端求值（例如 `demo_r8.sh:15` 的 `cpu_pct` 要 1 秒），標題因此會延遲出現。
- **關係**：被誰呼叫 → 幾乎所有 script（`test.sh`、`stress.sh`、`capture.sh`、`demo_r1`..`demo_r12`）以及同檔的 `show_server_log`；呼叫 → `printf`。

### `c_ok`
*位置：`scripts/lib.sh:26`　簽章：`c_ok <文字...>`*

- **用途**：以粗體綠色（`ESC[1;32m`）印一行成功結果。
- **設計**：與 `c_bad` 配對，demo 的判定行（例如 `no zombies`、`alice got control`）一律用綠色表示「預期的安全行為」，紅色表示「不安全行為」，讓 safe/unsafe 兩次執行的差別一眼可見。`parse_mode` 也用它印 SAFE 橫幅。
- **細節**：無回傳碼設計（永遠是 `printf` 的 0）；所以 demo 的判定不能靠 `c_ok` 的結果，只能靠前面的 `if`。逸出碼無條件輸出。
- **關係**：被誰呼叫 → `parse_mode`、各 demo、`test.sh`、`stress.sh`；呼叫 → `printf`。

### `c_bad`
*位置：`scripts/lib.sh:27`　簽章：`c_bad <文字...>`*

- **用途**：以粗體紅色（`ESC[1;31m`）印一行失敗或不安全的結果。
- **設計**：同 `c_ok`。`parse_mode` 用它印 UNSAFE 橫幅，demo 用它印 `zombie children`、`master exited`、`viewers are FROZEN` 等「機制被關掉之後的症狀」；`test.sh`、`stress.sh` 用它印失敗並接著 `exit 1`。
- **細節**：只印訊息，不會自己結束腳本；`exit` 由呼叫端決定。
- **關係**：被誰呼叫 → `parse_mode`、`test.sh`、`stress.sh`、各 demo；呼叫 → `printf`。

### `c_note`
*位置：`scripts/lib.sh:28`　簽章：`c_note <文字...>`*

- **用途**：以暗淡（`ESC[2m`）印一行補充說明。
- **設計**：用於「不是判定、只是讓讀者知道前提」的訊息，例如 `stress.sh:36` 提醒 `server.clients` 包含 `bench info` 自己、`demo_r9.sh:11` 顯示 `ulimit -u` 的值、`capture.sh` 提示缺少 `tshark`／`screen`。暗淡色讓它不搶判定行的注意。
- **關係**：被誰呼叫 → `stress.sh`、`capture.sh`、`demo_r8.sh`、`demo_r9.sh`；呼叫 → `printf`。

### `cleanup`
*位置：`scripts/lib.sh:30`　簽章：`cleanup`（由 `trap cleanup EXIT` 呼叫，無參數）*

- **用途**：腳本結束時殺掉所有背景啟動的 `benchsim`、`benchd` 與 `bench-evil`，確保下一次執行不會碰到殘留的伺服器或佔用中的埠。
- **設計**：
  - 兩階段：先對 `BG_PIDS` 每個 pid 送 `SIGTERM`（讓 benchd 走有序關機路徑：`shutdown_server()` 先等 child 最多 1 秒送完 `SERVER_SHUTDOWN` 事件，`server/main.c` `shutdown_server`），`sleep 1.3` 給它這 1 秒窗口；仍沒結束的再 `kill -9`。這樣既不會在平常情況下硬殺（避免日誌被截斷、讓關機事件有機會傳出），又保證不會因為卡住的行程（如 R7 unsafe 的死鎖）讓腳本掛住或遺留行程。
  - 最後 `wait 2>/dev/null` 回收所有子行程，避免 shell 在結束時印 `Terminated` 之類的工作控制訊息。
  - benchd 的 child 與 reader 用 `prctl(PR_SET_PDEATHSIG, SIGTERM)`，所以 master 被殺後它們也會跟著結束（DECISIONS Server）；這就是只追蹤 master pid 就夠的原因。
- **細節**：
  - `kill` 的錯誤（行程已經不在）全部被 `2>/dev/null` 吞掉，所以已經結束的 pid（如 `stop_sim` 殺掉的 benchsim，或 `demo_r12.sh` 中被重啟的舊 benchd）重複出現在 `BG_PIDS` 也沒有關係。
  - `${BG_PIDS[@]}` 為空時什麼都不做。
  - 第 38 行另有 `trap 'exit 130' INT TERM`：使用者按 Ctrl-C 或腳本收到 SIGTERM 時，用 `exit 130` 離開，進而觸發 EXIT trap 執行 `cleanup`；130 = 128 + SIGINT 的慣例。
- **關係**：被誰呼叫 → `trap cleanup EXIT`（`lib.sh:37`）；讀取 → `BG_PIDS`（由 `start_sim`、`start_server` 以及 demo 內的 `BG_PIDS+=($!)` 填入）。

### `need_bins`
*位置：`scripts/lib.sh:40`　簽章：`need_bins`（無參數）*

- **用途**：確認 `$BIN` 下有 `benchd`、`bench`、`benchsim`、`bench-evil` 四個可執行檔，缺任何一個就印提示並結束腳本。
- **設計**：每個 script 開頭就檢查，把「忘了 `make`」變成一行清楚的錯誤，而不是後面某個 `bench-evil: command not found` 造成難以判讀的失敗。因為腳本是用 `. lib.sh` 載入（同一個 shell），這裡的 `exit 1` 會結束整個腳本，並觸發 `cleanup`。
- **細節**：
  - 錯誤訊息寫到 stderr：`missing $BIN/<name> — run 'make' first`。
  - `demo_r7.sh` 在載入 lib 前把 `BIN` 指向 `build/debug/bin`，所以 `need_bins` 檢查的是 debug 版。
  - 只檢查 `-x`，不檢查版本是否與原始碼一致。
- **關係**：被誰呼叫 → 每個 script 載入 `lib.sh` 後的第一行（`test.sh:13` 等）；呼叫 → 無。

### `start_sim`
*位置：`scripts/lib.sh:47`　簽章：`start_sim [benchsim 選項...]`*

- **用途**：在背景啟動 `benchsim`（模擬 UART 裝置的 pty），等到 `$DEV` 這個 symlink 出現才回傳。
- **設計**：
  - 先 `rm -f "$DEV"`：benchsim 啟動時會自己建立 symlink，但腳本要用「`$DEV` 是否存在」判斷「新的 benchsim 準備好了」，所以必須先刪掉舊 symlink，否則上一輪留下的檔案會讓等待立即成功。
  - `benchsim -L "$DEV" "$@"`：`-L` 指定 symlink 路徑；其餘選項由呼叫端決定，例如 `--loop`（持續輪播）、`-l file -b baud`（`demo_r10.sh`）。沒有給 `-l` 時 benchsim 使用內建的 generic U-Boot 文字（`tools/benchsim.c` `load_log`）。
  - 等待是輪詢 `seq 50` 次、每次 `sleep 0.05`，最長約 2.5 秒；比起固定 `sleep` 更快也更可靠。
- **細節**：
  - stderr（benchsim 的日誌）附加到 `$LOG_SIM`，stdout 不重導向。
  - `SIM_PID=$!` 並加入 `BG_PIDS`；`stop_sim` 之後再呼叫 `start_sim` 會再次設定新的 `SIM_PID`。
  - 失敗（2.5 秒內沒出現）時印 `benchsim did not come up` 到 stderr 並 `return 1`；各 script 沒有檢查這個回傳值，之後的步驟會因為裝置不在而失敗。
  - 該函式只回傳 0 或 1，沒有 `exit`。
- **關係**：被誰呼叫 → 所有 script 的啟動階段，`test.sh:33`、`:65`、`demo_r8.sh:11`、`:25` 會在中途重新啟動；呼叫 → `benchsim`（`tools/benchsim.c`）。

### `stop_sim`
*位置：`scripts/lib.sh:57`　簽章：`stop_sim`（無參數）*

- **用途**：結束目前的 `benchsim`，用來模擬「UART 裝置被拔掉」。
- **設計**：benchsim 收到 SIGTERM 後會刪掉 symlink 並結束，pty 主端隨之關閉，benchd 的 reader 對該 fd 的 `read()` 就會失敗（EIO 或 0），重開 `$DEV` 也因 symlink 不見而失敗（`tools/benchsim.c` 主迴圈後的 `unlink(opt_link)`）；這與真實 USB-serial 被拔掉的行為相符，所以 `test.sh` 的 device 組與 `demo_r8.sh` 用它觸發 R8 的裝置掉線路徑。結束後 `wait` 回收，並把 `SIM_PID` 清空。
- **細節**：
  - 若 `SIM_PID` 為空就不 `kill`（`[ -n "$SIM_PID" ] && ...`）。
  - 沒有從 `BG_PIDS` 移除這個 pid；`cleanup` 稍後再 `kill` 一次會因為行程不存在而失敗，但錯誤被吞掉。
  - 不等待 symlink 消失；呼叫端（`test.sh:62`、`demo_r8.sh:19`）用 `sleep` 等 reader 反應。
- **關係**：被誰呼叫 → `test.sh:61`、`demo_r8.sh:18`；呼叫 → `kill`、`wait`。

### `wait_port`
*位置：`scripts/lib.sh:59`　簽章：`wait_port`（使用全域 `PORT`）*

- **用途**：等到 TCP `$PORT` 有行程在 LISTEN，才讓腳本繼續送連線。
- **設計**：benchd 在背景啟動後需要一點時間才會 `bind`/`listen`；固定 `sleep` 要嘛太長要嘛不夠。改輪詢 `ss -ltn "sport = :$PORT"` 的輸出是否含 `LISTEN`，最多 100 次、每次 0.05 秒，約 5 秒。
- **細節**：
  - 逾時時印 `benchd is not listening on $PORT` 到 stderr 並 `return 1`；`start_server` 回傳這個值，但呼叫端不檢查。
  - 只確認「埠上有人在 listen」，不確認那個行程就是剛啟動的 benchd；如果埠被前一次殘留的伺服器佔用，這裡仍會成功（新的 benchd 其實 bind 失敗）。`cleanup` 的 SIGKILL 階段就是為了避免這種殘留。
  - `ss` 失敗時 `2>/dev/null` 吞掉錯誤，視為尚未 LISTEN。
- **關係**：被誰呼叫 → `start_server`；呼叫 → `ss`、`grep`。

### `start_server`
*位置：`scripts/lib.sh:69`　簽章：`start_server [benchd 選項...]`*

- **用途**：在背景啟動 `benchd -d "$DEV" -p "$PORT" ...`，記下 master pid 並等它開始監聽。
- **設計**：
  - 固定帶 `-d "$DEV" -p "$PORT"`，其餘選項（`--max-clients`、`--history`、`--idle-release`、`$UNSAFE_OPT`）原樣傳入。這樣每個 demo 只需要寫「與預設不同的選項」。
  - benchd 本身不 daemonize（`server/main.c` 的 `main` 在前景執行，內部 `fork()` 出 reader 與 child），所以 `$!` 就是 master 的 pid；`pgrep -P "$D_PID"` 才找得到 reader 與 child。
  - stderr（benchd 的日誌，含 child 與 reader，因為它們繼承 stderr）附加到 `$LOG_D`，讓 demo 最後能 `grep`/`tail` 日誌。
- **細節**：
  - `D_PID=$!`，並加入 `BG_PIDS`。`demo_r12.sh` 重複呼叫會覆寫 `D_PID` 並再加入一個 pid。
  - 回傳值是 `wait_port` 的回傳值（0 或 1）。
  - 呼叫前綴的環境變數對 benchd 有效：`BENCH_FAULT=die_in_lock:3 start_server ...`（`demo_r7.sh:19`）。
- **關係**：被誰呼叫 → 所有 script；呼叫 → `benchd`（`server/main.c`）、`wait_port`。

### `server_alive`
*位置：`scripts/lib.sh:76`　簽章：`server_alive`（使用全域 `D_PID`、`PORT`；回傳 0 表示存活）*

- **用途**：判斷 benchd 是否仍在正常服務。
- **設計**：同時檢查兩件事：`kill -0 "$D_PID"`（行程存在）以及 `ss` 顯示埠仍在 LISTEN。只用 `kill -0` 不夠：尚未被 `wait` 的 zombie（例如 R2 unsafe 被 SIGPIPE 殺死的 master，腳本還沒有 `wait` 它）對 `kill -0` 仍回傳成功，但它的 listen socket 已經關了。兩者都成立才算存活。
- **細節**：沒有輸出，純粹靠回傳碼；`kill -0` 的錯誤被吞掉；`ss` 的輸出由 `grep -q` 消耗。
- **關係**：被誰呼叫 → `test.sh` 的 `check "survives ..." server_alive`（第 72、79、82、84 行）、`demo_r2.sh:21`、`demo_r3.sh:26`；呼叫 → `kill -0`、`ss`。

### `reader_pid`
*位置：`scripts/lib.sh:77`　簽章：`reader_pid`（輸出 reader 的 pid）*

- **用途**：找出 benchd 的 reader 行程 pid（唯一持有序列埠 fd 的行程）。
- **設計**：用 `pgrep -P "$D_PID" -x benchd-reader`：只找 master 的直接子行程，且行程名稱完全等於 `benchd-reader`（reader 啟動時以 `PR_SET_NAME` 改名，DECISIONS Server），再用 `head -1` 取第一個。這樣比 `pidof` 或 `ps | grep` 準確，不會誤抓 `benchd-child` 或別的 benchd 實例。
- **細節**：找不到就輸出空字串；結果是在 `R=$(reader_pid)` 這種命令替換中使用。
- **關係**：被誰呼叫 → `demo_r8.sh:14`（之後把 pid 傳給 `cpu_pct`）；呼叫 → `pgrep`。

### `children_of`
*位置：`scripts/lib.sh:78`　簽章：`children_of`（每行輸出一個 pid）*

- **用途**：列出 master 的所有直接子行程 pid，包括 reader 與所有 child。
- **設計**：`pgrep -P "$D_PID"`。demo 用 `| wc -l` 來數行程數，以觀察 R5（沈默連線被清掉）與 R9（行程數是否被限制）的效果；`2>/dev/null` 避免 `pgrep` 對不存在的 pid 印錯誤。
- **細節**：數字包含 reader，所以 R9 safe 預期約為 `max-clients + 1`；zombie 也算（`pgrep` 會列出）。
- **關係**：被誰呼叫 → `demo_r5.sh:20`、`demo_r9.sh:18`；呼叫 → `pgrep`。

### `info`
*位置：`scripts/lib.sh:79`　簽章：`info`（輸出 `bench info --raw` 的 `key=value` 行）*

- **用途**：對 `127.0.0.1:$PORT` 做一次 INFO 請求，輸出伺服器的系統資訊（`key=value` 逐行）。
- **設計**：固定加 `--raw`，讓輸出在互動終端與管線中都是機器可解析的 `key=value`，而不是畫框的版本（`sub_info` 只有在 `!o->raw && isatty` 時才畫框，`client/subcmd.c`）。`2>&1` 把錯誤訊息合併進輸出，這樣 `out=$(info)` 失敗時（例如伺服器額滿時的 `SERVER_BUSY`）錯誤原因能被腳本印出來（`demo_r5.sh:19`）。
- **細節**：
  - 每次呼叫都是一次完整的連線、握手、INFO、BYE，期間算一個 client（DECISIONS Protocol：`server.clients` 含提問者本人），也會佔一個名額。
  - 回傳碼是 `bench info` 的退出碼（成功 0，連線失敗或沒有 INFO 回覆為 1）。
- **關係**：被誰呼叫 → `info_key`、`stress.sh:35`、`demo_r3.sh`、`demo_r5.sh`、`demo_r6.sh`、`demo_r8.sh`；呼叫 → `bench info`（`client/subcmd.c` `sub_info`）。

### `info_key`
*位置：`scripts/lib.sh:80`　簽章：`info_key <key>`（輸出該 key 的值）*

- **用途**：取得 INFO 中某個 key 的值，例如 `info_key dev.rx_bytes`。
- **設計**：`info | sed -n "s/^$1=//p"`：只列出以 `<key>=` 開頭的行並去掉前綴，所以輸出就是值本身，可以直接放進算術比較（`demo_r8.sh:27` 的 `rx1`、`rx2`）。
- **細節**：
  - `$1` 中的 `.` 在 regex 裡是萬用字元，但實際的 key 夠獨特，不會誤配。
  - key 不存在或伺服器沒回應時輸出空字串；`demo_r3.sh:26` 就利用「命令成功與否」來判斷伺服器是否還有回應（輸出被丟掉）。
- **關係**：被誰呼叫 → `demo_r8.sh:27`、`demo_r3.sh:26`；呼叫 → `info`、`sed`。

### `cpu_pct`
*位置：`scripts/lib.sh:83`　簽章：`cpu_pct <pid>`（輸出整數百分比）*

- **用途**：量測指定行程在約 1 秒內使用了多少比例的單一核心，輸出整數百分比；行程不存在時輸出 `?`。
- **設計**：不用 `top`/`ps` 的取樣，而是直接讀 `/proc/<pid>/stat` 的第 14、15 欄（`utime + stime`，單位是 clock tick），間隔 `sleep 1` 後再讀一次，差值乘 100 除以 `getconf CLK_TCK`（每秒的 tick 數），即 `(b - a) * 100 / hz`。這是可重現的「這 1 秒的 CPU 使用率」，用來證明 R8 unsafe 時 reader 空轉（約 100%）以及 safe 時回到接近 0%。
- **細節**：
  - 任一次讀取失敗（`/proc/<pid>/stat` 不存在）就 `echo "?"` 並 `return`，而不是輸出錯誤的數字。
  - 整數除法，精度只到 1%。
  - awk 以空白分欄，只在行程名稱（第 2 欄）沒有空白時欄位位置才正確；`benchd-reader` 沒有空白。
  - 函式耗時約 1 秒，這也是 `demo_r8.sh` 的標題行出現會有延遲的原因。
- **關係**：被誰呼叫 → `demo_r8.sh:15`、`:21`、`:30`；呼叫 → `awk`、`getconf`、`sleep`。

### `parse_mode`
*位置：`scripts/lib.sh:93`　簽章：`parse_mode <safe|unsafe|空> <機制名稱>`*

- **用途**：處理 `demo_rN.sh safe|unsafe` 的第一個參數，設定全域 `MODE` 與 `UNSAFE_OPT`，並印出一行大字橫幅標示現在是 SAFE 還是 UNSAFE。
- **設計**：
  - 所有 12 個 demo 共用同一個邏輯：把「機制名稱」（如 `zombie`、`ctrl-release`，即 `--unsafe` 接受的名稱，`lib/util.c` 的 `robust_names`）當第二個參數傳入。unsafe 時 `UNSAFE_OPT="--unsafe=<名稱>"`，safe 時為空字串；demo 用 `start_server $UNSAFE_OPT`（不加引號，空字串會展開成零個參數）把它交給 benchd，或給 client（`demo_r11/12`）。
  - 預設值 `safe`（`MODE=${1:-safe}`），所以不帶參數就是安全模式。
- **細節**：
  - 其他值印 `usage: $0 safe|unsafe` 到 stderr 並 `exit 1`；`exit` 在 sourced 函式內會結束整個腳本。
  - 橫幅：unsafe 用 `c_bad "### <script>: UNSAFE — mechanism '<名稱>' disabled"`，safe 用 `c_ok "### <script>: SAFE — mechanism '<名稱>' enabled"`。
  - 必須有第二個參數，否則橫幅會顯示空的名稱，`--unsafe=` 也無效。
  - 這是 sourced 函式，所以 `MODE`、`UNSAFE_OPT` 會成為呼叫腳本的變數。
- **關係**：被誰呼叫 → `demo_r1.sh`..`demo_r12.sh`（`demo_r1.sh:7` 等）；呼叫 → `c_ok`、`c_bad`。

### `show_server_log`
*位置：`scripts/lib.sh:107`　簽章：`show_server_log [行數]`（預設 15）*

- **用途**：印出 benchd 日誌檔 `$LOG_D` 的最後幾行，並加上標題。
- **設計**：demo 的結論往往藏在伺服器端日誌（`rejected ... server full`、`EOWNERDEAD recovered`、`client ... killed by signal`），腳本每個 demo 結尾呼叫它，讓使用者不必自己去 `out/<name>/benchd.log` 翻找。`${1:-15}` 讓每個 demo 依需要調整行數（4 到 12 行）。
- **細節**：標題用 `c_head`，內容用 `tail -n`；日誌檔在載入 `lib.sh` 時已被清空，所以只會看到本次執行的內容。
- **關係**：被誰呼叫 → `demo_r2.sh`、`demo_r3.sh`、`demo_r4.sh`、`demo_r5.sh`、`demo_r6.sh`、`demo_r7.sh`、`demo_r8.sh`；呼叫 → `c_head`、`tail`。

## `scripts/test.sh`

`make test` 的實作（`Makefile:88-89`，`test` 依賴 `release`），規格 §10.3 的自動檢查。它由工具行程（bash）執行：先做四項靜態「建置事實」檢查，再啟動一個 `benchsim` 和一個 `benchd --idle-release 3`，然後依序檢查協定、控制權、裝置掉線、`bench-evil` 的每種攻擊模式（safe 模式下伺服器必須存活）、最後確認沒有 zombie。每個檢查都是一次 `check` 呼叫，通過印綠色 `PASS`、失敗印紅色 `FAIL`；全部通過回傳 0，只要有失敗就印出失敗數與 `$WORK/test.log` 的位置並 `exit 1`。腳本本身沒有加 `--unsafe`，測的是 safe（預設）行為。依賴 `lib.sh` 的 `need_bins`、`start_sim`、`start_server`、`stop_sim`、`server_alive`、`c_head`、`c_ok`、`c_bad`。

主流程（行號為 `scripts/test.sh`）：

- 第 10-12 行：`PORT=${PORT:-19400}`、`DEV=${DEV:-/tmp/benchsim-test-$$}`，再載入 `lib.sh`。用 19400 和含 pid 的 DEV 名稱，是為了不和正在手動跑的 benchd（9000）與 `/tmp/benchsim0` 衝突。第 13 行 `need_bins`，第 14-15 行初始化 `PASS`、`FAIL`，第 24 行清空 `$WORK/test.log`，第 25 行 `A=127.0.0.1:$PORT` 作為所有 client 的位址。
- **build（27-31）**：1) 原始碼（`server client lib tools` 的 `.c`、`.h`）內沒有 `pthread_create(`；2) 所有 `$BIN/*` 與 `build/lib/libbench.so.1` 的動態符號表沒有 `pthread_create`；3) `ldd` 顯示 `benchd` 與 `bench` 都連結到 `build` 目錄下的 `libbench.so.1`；4) release 版 `benchd` 與 `libbench.so.1` 的字串中找不到 `tx pkt type=`（TRACE 訊息被編譯期移除，見 `docs/DECISIONS.md` Logging）。對應規格「不得使用 thread」、「共享 `.so`」、「兩層 debug log」。
- **啟動（33-35）**：`start_sim`（不加 `--loop`，播放內建的 generic U-Boot 文字一次，因為沒有給 `-l`）、`start_server --idle-release 3`（把閒置自動釋放控制權縮短成 3 秒以便測試）、`sleep 2.5` 讓開機輸出先進入歷史。
- **protocol（37-43）**：`bench info --raw` 輸出含 `host.kernel=` 與 `ctrl.holder=`；`tail -n 0` 輸出 0 位元組；`tail -n 1K` 輸出介於 1 與 1024 位元組；`tail -n 64K` 含 `U-Boot SPL`；`send 'version\r' --wait 1500` 得到 `U-Boot 2014.07 (benchsim`（證明輸入經 benchd 寫進裝置、裝置回應又回到 client）；連到沒有人監聽的 `127.0.0.1:1` 時 `bench info` 必須失敗（非 0）。
- **control（45-58）**：`bench-evil take-hold --name alice --sec 8` 在背景持有控制權；0.7 秒後 `bob` 的 `bench send` 必須以 exit 2 被拒絕，且錯誤訊息含 `held by alice`；再等 3.5 秒，3 秒閒置自動釋放後 `ctrl.holder=(none)`，bob 這時能拿到控制權；殺掉 alice 的 `bench-evil`，執行 `take-and-die`（拿到控制權後 `_exit`，沒有 BYE），0.3 秒後 bob 又能拿到控制權（崩潰釋放），歷史裡也要有 `control released by crashy (disconnect)` 事件。對應 R6 `ctrl-release`。
- **device（60-67）**：`stop_sim` 模擬拔線，0.8 秒後 `dev.state=LOST`；掉線時 `bench send 'x' --wait 300` 必須收到 `DEVICE_LOST` 錯誤；`start_sim` 重新插上，1.2 秒後 `dev.state=UP` 且 `dev.reconnects=1`。對應 R8 `dev-reopen`。
- **robustness（69-84）**：對 `slow`、`bad-len`、`bad-magic`、`bad-crc`、`take-and-die` 各跑一次 `bench-evil`（輸出存 `$WORK/evil-<mode>.txt`），每次都 `check "survives $m" server_alive`；另檢查 slow 模式有收到 `got INFO`（R4）、bad-len 模式被回 `BAD_LENGTH`（R3）；`silent --count 5 --sec 7` 之後必須顯示 `0 of 5 silent connections still open`（R5，5 秒 HELLO 逾時小於 7 秒觀察窗）；`rst --count 20` 之後伺服器存活（R2）；`flood --count 40 --sec 2` 必須輸出 `32 welcomed, 8 refused`（R9，預設 `--max-clients 32`）且存活；`lag --sec 3 --after 1` 之後存活（落後的 client 不影響伺服器）。
- **hygiene（86-89）**：`sleep 1` 後 `ps -o stat= --ppid $D_PID` 不含 `Z`（R1 沒有 zombie）；`pgrep -P $D_PID | wc -l` 等於 1，也就是所有 client child 都已被收走，只剩 reader。
- **結語（91-92）**：全過印 `all N checks passed`，否則 `exit 1`。

### `check`
*位置：`scripts/test.sh:16`　簽章：`check "描述" 命令 [參數...]`*

- **用途**：執行一個檢查命令，成功就計入 `PASS` 並印綠色 `PASS <描述>`，失敗就計入 `FAIL` 並印紅色 `FAIL <描述>`。
- **設計**：
  - 命令的 stdout 與 stderr 都附加到 `$WORK/test.log`（`>>"$WORK/test.log" 2>&1`），畫面上只有一行結果；失敗時詳細輸出留在日誌，最後一行的 `details: $WORK/test.log` 指向它。
  - 命令用 `"$@"` 直接執行，所以參數可以是外部程式（`"$BIN/bench" send ...`）、`grep -q ...`，也可以是 shell 函式（`server_alive`）。需要管線、`!` 否定或變數展開的檢查，就包成 `bash -c "..."` 傳進來（檢查字串中的 `\$(...)` 是延遲到內層 shell 才展開，`'$BIN/bench'` 則在外層就展開）。
  - 計數器 `PASS`、`FAIL` 是全域變數，`check` 必須在主 shell 中被直接呼叫（沒有放進管線或子 shell），計數才會保留到最後第 92 行的結語。
- **細節**：
  - `local desc=$1; shift` 後 `$@` 只剩命令本身。
  - 回傳碼固定是 `if` 之後的 `printf` 的 0，不反映檢查結果；整體成敗靠 `FAIL` 計數。
  - 在 safe 模式下測試；沒有使用 `--unsafe`。
- **關係**：被誰呼叫 → `scripts/test.sh` 第 28-89 行的全部檢查：build 4 次（28-31）、protocol 6 次（38-43）、control 6 次（49-58）、device 3 次（63-67）、robustness 12 次（第 72 行迴圈內的 `check "survives $m"` 執行 5 次，加上 74-84 行的 7 次）、hygiene 2 次（88-89），共 33 次執行；呼叫 → 被檢查的命令、`printf`。

## `scripts/stress.sh`

`make stress` 的實作（`Makefile:91-92`），規格 §10.1「同時服務 10 個以上 client」的證明。由工具行程（bash）執行：啟動 `benchsim --loop` 與歷史 1 MiB、`--max-clients 32` 的 `benchd`，同時啟動 20 個 `bench watch`，在第 5 秒擷取 `pstree`、TCP 連線數、`bench info`，等全部結束後比較每個 client 收到的位元組是否完全相同。這個檔案沒有定義任何函式，完整說明見下方 `### `scripts/stress.sh``。依賴 `lib.sh` 的 `need_bins`、`start_sim`、`start_server`、`info`、`c_head`、`c_note`、`c_ok`、`c_bad`，以及 `pstree`、`ss`、`sha256sum`、`stat`。

### `scripts/stress.sh`
*位置：`scripts/stress.sh:1-73`　用法：`scripts/stress.sh`（可用環境變數 `N`、`DUR` 覆寫）*

- **用途**：用 20 個同時連線的唯讀 client 證明 benchd「一個連線一個 child」的架構能同時服務至少 10 個 client，而且每個 client 看到的 UART 位元組流完全一致（單一 ring 廣播，見 README 架構圖）。結果寫到 `out/stress/summary.md`（README 需求對照表引用的證據）。
- **設計**：
  - 「一致性」不用肉眼比對，而用 sha256：每個 bot 都用 `-n 1M` 要求重播最多 1 MiB 歷史（超過 ring 容量時 server 以現有歷史為上限），所以所有 bot 的位元組流都從同一個 offset 開始。但每個 bot 在 `--duration` 內收到的總量略有差異（連線時間不同），所以只比較「所有檔案共同的前 `min` 位元組」（`min` 是最小檔案的大小），再取 sha256；若 broadcast 順序一致，20 個雜湊必須只有 1 個相異值。
  - 另外用「起始 offset 是否相同」（`first_offset=`）把關：如果各 bot 的起點不同，比較共同前綴就沒有意義。
  - `pstree` 與 `pgrep -x benchd-child` 提供「確實是 fork 出來的行程」證據（child 名稱由 `PR_SET_NAME` 設為 `benchd-child`，見 DECISIONS）；`ss` 提供 TCP 層證據。
  - 每個 bot 的 `bench watch` 是背景工作，其 pid 存在 `BOTS`（不是 `BG_PIDS`），由腳本自己逐一 `wait` 取得退出碼；它們靠 `--duration` 自己結束，所以不需要 cleanup 殺。
- **細節**：
  - 第 9-13 行：載入 lib（9）、`need_bins`（10）、`N=${N:-20}`（11）、`DUR=${DUR:-20}`（12）；第 13 行先刪掉上次的 `bot*.bin`、`bot*.err`。
  - 第 15-18 行：`start_sim --loop` 讓裝置持續輸出，`start_server --history 1M --max-clients 32`，`sleep 2` 讓歷史累積一些資料。
  - 第 20-28 行：用 `seq -w 1 "$N"` 產生零填充編號（`bot01`..`bot20`），每個 bot 執行 `bench watch 127.0.0.1:$PORT --name botNN -n 1M --duration $DUR --out $WORK/botNN.bin`，stdout 丟掉、stderr 存 `botNN.err`（`sub_watch` 結束時把 `watch: name=... first_offset=... last_offset=...` 摘要印在 stderr，`client/subcmd.c:236`），然後 `sleep 5`。
  - 第 30-36 行（t=5 秒快照）：`pstree -p $D_PID | tee pstree.txt`；`nchild` 是名稱恰為 `benchd-child` 的直接子行程數（不含 reader）；`est` 是 `ss -tn state established "( sport = :$PORT )"`（只算伺服器端 socket，所以不會把 client 端的 socket 重複算一次）去掉標題列的行數；接著呼叫 `info` 取得 `bench info --raw` 結果存 `info.txt` 並只顯示 `server.clients`、`server.max_clients` 與 `hist.*`。`server.clients` 會比 20 多 1，因為 `bench info` 自己也算一個完成握手的 client（DECISIONS Protocol）；第 36 行 `c_note` 提醒這件事。
  - 第 38-40 行：逐一 `wait "$p" || fail=$((fail + 1))`，`fail` 是非 0 退出的 watcher 數（連線被切斷或 15 秒沒資料時 `sub_watch` 回傳 1）。
  - 第 42-54 行：`first_offsets` 把所有 `.err` 的 `first_offset=N` 取出 `sort -u`；`min` 是最小的 `.bin` 大小；對每個 `.bin` 執行 `head -c "$min" | sha256sum | cut -c1-16` 並輸出一列到 `hashes.txt`（標題列在迴圈外面，不進入 `hashes.txt`，所以第 54 行 `awk '{print $4}'` 取的全是雜湊欄）；`uniq_hashes` 是相異雜湊數。
  - 第 56-67 行：把各項結果寫成 markdown 表格到 `summary.md`（並 `tee` 到畫面）。
  - 第 68-73 行：PASS 條件同時成立才算通過：`uniq_hashes == 1`、`fail == 0`、`nchild >= N`、`first_offsets` 只有一行；否則印 `FAIL` 並 `exit 1`。
- **關係**：被誰呼叫 → `make stress`（`Makefile:91-92`）；呼叫 → `lib.sh` 的 `start_sim`、`start_server`、`info`，以及 `benchd`（server/main.c）、`bench watch`（client/subcmd.c 的 `sub_watch`）。

## `scripts/capture.sh`

`make capture` 的實作（`Makefile:94-95`），規格 §10.2 封包擷取。由工具行程（bash）執行：啟動 `benchsim` 與 `benchd`，在 loopback 上用 `tcpdump`（或有權限的 `dumpcap`）錄下 TCP `$PORT` 的流量，輸出兩個檔案：`captures/session.pcap`（連線、重播、即時資料、info、take、輸入 `version`、release、quit 的完整一段）與 `captures/rst.pcap`（一次正常 FIN 結束的 `bench info` 與一次 `bench-evil take-and-die` 的異常結束，用來對照 FIN 與 RST）。若系統有 `tshark`，會用 `tools/benchd.lua` 解碼並印摘要。互動部分用 detached GNU `screen` 驅動 TUI，沒有 `screen` 時退回非互動子指令（DECISIONS Scripts）。檔案定義四個函式：`pick_capture`、`cap_start`、`cap_stop`、`summary`。

主流程（行號為 `scripts/capture.sh`）：

- 第 11-15 行：載入 lib、`need_bins`、`CAPDIR=$ROOT/captures`、`LUA=$ROOT/tools/benchd.lua`。第 27 行 `CAP=$(pick_capture)`。
- 第 53-56 行：印出所選工具，`start_sim`、`start_server`（沒有 `--loop`，一遍內建開機文字），`sleep 2.5` 讓歷史有內容可重播。
- 第 59-81 行（`session.pcap`）：`cap_start`；若有 `screen`，在 detached screen 內執行 `bench --name capture -n 2K 127.0.0.1:$PORT`（`LC_ALL=C.UTF-8`），再用 `screen -X stuff` 依序送出 `Ctrl-] i`（`\x1d` 是 Ctrl-]，info）、`Ctrl-] t`（take）、`version\n`、`Ctrl-] r`（release）、`Ctrl-] q`（quit），各間隔 0.5-1.5 秒，最後 `screen -X quit`；沒有 `screen` 時改跑 `bench tail`、`bench info`、`bench send 'version\n' --wait 1500`。之後 `cap_stop`、`ls -l`，用 `summary` 印出摘要但用 `grep -v` 濾掉只含 `DATA` 的列並只取前 40 行，另外單獨計算 `DATA` 封包數。
- 第 84-95 行（`rst.pcap`）：`cap_start`；先 `bench info`（`conn_bye` 走 BYE + `shutdown(SHUT_WR)` + 等 EOF 的正常流程，DECISIONS Client，所以是 FIN）；再 `bench-evil take-and-die`（拿到控制權後 `_exit(1)`，沒有 BYE，核心直接關閉 socket）；`sleep 0.5`、`cap_stop`、`ls -l`；有 `tshark` 時只列出 `tcp.flags.fin==1 || tcp.flags.reset==1` 的封包（frame 編號、來源/目的埠、旗標字串）。最後 `c_note` 印出用 Wireshark 開啟的指令。

### `pick_capture`
*位置：`scripts/capture.sh:17`　簽章：`pick_capture` （無參數；用 stdout 回傳 `dumpcap` 或 `tcpdump`）*

- **用途**：決定要用哪一個擷取工具，結果以 `echo` 輸出，由第 27 行 `CAP=$(pick_capture)` 取得。
- **設計**：規格指定 `sudo tcpdump -i lo`，但每次要輸入 sudo 密碼很不方便，所以優先用「已經授權 capture 能力」的 `dumpcap`（Wireshark 隨附，可由 `setcap` 賦予 `cap_net_raw`），這樣不必 root；兩者都不符合時才退回 `tcpdump`（DECISIONS Scripts）。使用者可用環境變數 `CAPTURE=tcpdump|dumpcap` 強制指定，優先於自動偵測。
- **細節**：
  - 若 `$CAPTURE` 非空直接 `echo` 並 `return`（第 18 行），不檢查該工具是否存在。
  - 否則 `command -v dumpcap` 找到路徑 `d`，必須同時 `-x` 可執行，且 `getcap "$d"` 的輸出含 `cap_net_raw`，才回 `dumpcap`（第 21-22 行）；`getcap` 不存在或沒有能力時 `2>/dev/null | grep -q` 失敗，自然落到 `tcpdump`。
  - `local d` 避免污染全域變數。
- **關係**：被誰呼叫 → `scripts/capture.sh:27`；呼叫 → `command`、`getcap`、`grep`。

### `cap_start`
*位置：`scripts/capture.sh:30`　簽章：`cap_start <pcap 檔案>`*

- **用途**：在背景啟動擷取程式，只錄 loopback 上 `tcp port $PORT` 的封包並寫到指定檔案，並把背景 pid 記在全域變數 `CAP_PID`。
- **設計**：
  - 用 BPF 過濾 `tcp port $PORT`，檔案只含 benchd 的流量，不會混入其他服務。
  - `dumpcap` 分支：`dumpcap -q -i lo -f "tcp port $PORT" -w file`。`tcpdump` 分支：`sudo tcpdump -i lo -U -Z "$(id -un)" -w file "tcp port $PORT"`；`-U` 讓每個封包寫入後立即 flush，這樣 `cap_stop` 結束時檔案不會少最後幾個封包；`-Z` 讓 tcpdump 在開啟介面之後把權限降成目前使用者，輸出檔案的擁有者就是使用者而不是 root（DECISIONS Scripts）。
  - 最後 `sleep 1.5`：擷取程式（尤其經過 sudo）需要一點時間開啟介面並開始監聽，不等的話緊接著的第一批封包（TCP 三向交握）會漏掉。
- **細節**：
  - 先 `rm -f "$1"`，確保每次產生全新的檔案。
  - stderr 都導向 `$WORK/capture.err`，不污染畫面；sudo 需要密碼時是在終端機上提示，所以首次執行可能在此暫停。
  - `$!` 是背景工作的 pid：`tcpdump` 分支裡是 `sudo` 行程的 pid（不是 `tcpdump` 本身），這是 `cap_stop` 要用 `sudo kill` 的原因。
- **關係**：被誰呼叫 → `scripts/capture.sh:60`、`:85`；呼叫 → `dumpcap` 或 `sudo tcpdump`。

### `cap_stop`
*位置：`scripts/capture.sh:40`　簽章：`cap_stop`（使用全域 `CAP`、`CAP_PID`）*

- **用途**：結束 `cap_start` 啟動的擷取程式並等它收尾寫檔。
- **設計**：先 `sleep 0.5`，讓還在核心裡的最後幾個封包被擷取程式讀走；再送 `SIGINT`（tcpdump 與 dumpcap 收到 SIGINT 都會正常 flush 並關檔，不會留下截斷的 pcap）；最後 `wait` 回收。
- **細節**：
  - `dumpcap` 分支直接 `kill -INT "$CAP_PID"`；`tcpdump` 分支因為 `CAP_PID` 是 root 的 `sudo` 行程，一般使用者的 `kill` 通常沒有權限，所以先 `sudo kill -INT`，失敗才退回 `kill -INT`（`|| kill -INT`，第 42 行）。
  - `wait "$CAP_PID" 2>/dev/null` 忽略「不是子行程」之類的錯誤。
- **關係**：被誰呼叫 → `scripts/capture.sh:78`、`:89`；呼叫 → `sudo kill` / `kill`。

### `summary`
*位置：`scripts/capture.sh:46`　簽章：`summary <pcap 檔案>`*

- **用途**：用 `tshark` 搭配自製的 Wireshark Lua dissector `tools/benchd.lua` 把 pcap 轉成每列一個 benchd 封包的文字摘要（frame 編號、來源埠、目的埠、Info 欄）。
- **設計**：`tshark` 不是必要依賴，所以第一行用 `command -v tshark` 檢查，缺少時只印一行 `(install tshark for a decoded summary)` 就 `return`，整個擷取流程仍然成功，pcap 還是已經產生。
- **細節**：
  - 參數：`-X lua_script:"$LUA"` 載入 dissector；`-o tcp.desegment_tcp_streams:TRUE` 讓 TCP 串流重組，這樣被切成多個 segment 的 benchd 封包才能完整解碼；`-d tcp.port==$PORT,benchd` 把目前的埠明確綁到 `benchd` 協定（`tools/benchd.lua` 只預設註冊 9000，第 198 行，所以用其他 `PORT` 時必須這樣指定）；`-Y benchd` 只留下 benchd 封包；`-T fields -e frame.number -e tcp.srcport -e tcp.dstport -e _ws.col.Info` 輸出欄位。
  - `2>/dev/null` 吞掉 tshark 的雜訊，`| cut -c1-140` 限制每列寬度。
- **關係**：被誰呼叫 → `scripts/capture.sh:80`、`:81`（印摘要與計算 DATA 封包數）；呼叫 → `tshark`、`tools/benchd.lua`。

## `scripts/demo_r1.sh`

R1 `zombie` 的示範腳本：20 個 client 依序連上又離開，之後檢查 benchd 底下有沒有 `<defunct>` 子行程。由工具行程（bash）執行，被觀察的是 benchd master 的 SIGCHLD 處理（`server/main.c` 的 `on_chld`、`reap_children`）。沒有定義函式，說明見下方 block。

### `scripts/demo_r1.sh`
*位置：`scripts/demo_r1.sh:1-17`　用法：`scripts/demo_r1.sh safe|unsafe`*

- **用途**：示範「不收屍」的後果。unsafe 時每個結束的 child 都留成 zombie；safe 時 master 立刻回收。
- **設計**：
  - safe（預設）：master 為 `SIGCHLD` 安裝只設旗標的 handler（`sa_handler = on_chld`，`SA_RESTART | SA_NOCLDSTOP`，`server/main.c:392-397`），主迴圈每輪（`poll` 最長 200 ms）檢查 `got_chld` 後呼叫 `reap_children()`，以 `waitpid(-1, WNOHANG)` 迴圈回收。handler 內只做 `sig_atomic_t` 賦值，是 async-signal-safe；真正的 `waitpid`、鎖與 log 都在主迴圈做。
  - unsafe：`--unsafe=zombie` 讓 `UNSAFE(R_ZOMBIE)` 為真，master 完全不安裝 `SIGCHLD` handler，也不呼叫 `waitpid`（直到關機），所以每個結束的 child 都留在行程表裡直到 master 結束。
- **細節**：
  - 第 5-9 行：載入 lib、`need_bins`、`parse_mode "$1" zombie`（把 `UNSAFE_OPT` 設為空或 `--unsafe=zombie`）、`start_sim --loop`、`start_server $UNSAFE_OPT`。
  - 第 11-13 行：連續 20 次 `bench info --raw`（依序執行，不並行），每次連線後 child 因 BYE 結束，再 `sleep 0.5` 讓 safe 模式有時間回收。
  - 第 14-15 行：`ps -o pid,stat,comm --ppid "$D_PID"` 列出 master 的直接子行程；第 16 行 `z=$(ps -o stat= --ppid ... | grep -c Z)` 計算狀態含 `Z` 的行數。
  - 第 17 行：`z > 0` 印紅色 `N zombie (<defunct>) children`，否則綠色 `no zombies`。預期 unsafe 為 20，safe 為 0（README 表格）。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`c_head`、`c_ok`、`c_bad`；被觀察 → `server/main.c` 的 `reap_children`。

## `scripts/demo_r2.sh`

R2 `sigpipe` 的示範腳本：伺服器額滿後，連續湧入的 client 直接 RST，觀察 master 是否被 SIGPIPE 殺死。由工具行程（bash）執行，被觀察的是 master 的 `reject_busy()`（`server/main.c`）。沒有定義函式。

### `scripts/demo_r2.sh`
*位置：`scripts/demo_r2.sh:1-26`　用法：`scripts/demo_r2.sh safe|unsafe`*

- **用途**：示範「對已 RST 的連線寫入會產生 SIGPIPE」這個典型伺服器殺手，以及忽略 SIGPIPE 的防護。
- **設計**：
  - 要讓這條路徑可重現，必須先把伺服器塞滿，因為 SIGPIPE 發生在 master 自己回覆 `SERVER_BUSY` 的 `write()`（`reject_busy()`，`server/main.c:114`），而不是 child 內；所以腳本用 `--max-clients 3` 與 3 個長連線 flood client 佔滿名額。
  - Linux 上對 RST 連線的第一次 `write()` 回 ECONNRESET 且不送訊號；`reject_busy()` 先 `recv()`（讀掉這個錯誤），接下來的 `write()` 才引發 SIGPIPE（DECISIONS R2）。這就是 40 個 RST client 能殺死 master 的原因。
  - safe：master `signal(SIGPIPE, SIG_IGN)`（`server/main.c:338-339`），`write()` 改回傳 `EPIPE`，由 `bn_send_error` 的回傳值處理（只記一則 `busy reply ... failed ... (ignored)`）。unsafe：`signal(SIGPIPE, SIG_DFL)`（第 341 行）明確恢復預設動作，避免父行程（如 Python launcher）遺留的 `SIG_IGN` 讓 unsafe 示範「意外安全」。
- **細節**：
  - 第 9-11 行：`parse_mode "$1" sigpipe`、`start_sim --loop`、`start_server --max-clients 3 $UNSAFE_OPT`。
  - 第 13-16 行：`bench-evil flood --count 3 --sec 20` 放背景（`BG_PIDS+=($!)`，讓 cleanup 一定會殺掉它）佔滿 3 個名額，`sleep 1` 等它們完成握手。
  - 第 17-19 行：`bench-evil rst --count 40`：連線 40 次，每次設 `SO_LINGER {1, 0}` 後立刻 `close`，於是核心送 RST 而不是 FIN。`sleep 1`。
  - 第 20-25 行：`server_alive` 為真則綠色 `master pid ... still listening`；否則 `wait "$D_PID"` 取退出狀態 `st`，印紅色 `NO — master exited with status $st (...)`，當 `st > 128` 時用 `kill -l $((st-128))` 轉成訊號名稱。預期 unsafe 是 141（128 + SIGPIPE 13，README 表格），safe 為存活。
  - 第 26 行：`show_server_log 6` 印 benchd 日誌最後 6 行。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`server_alive`、`show_server_log`；被觀察 → `server/main.c` 的 `reject_busy`。

## `scripts/demo_r3.sh`

R3 `validate` 的示範腳本：對 benchd 送三種畸形封包（錯誤 magic、錯誤 CRC、宣稱 1 MiB 長度），觀察 child 是否崩潰。由工具行程（bash）執行，被觀察的是 child 的握手（`server/session.c` `handshake`）與 `lib/proto.c` 的 `bn_recv`。沒有定義函式。

### `scripts/demo_r3.sh`
*位置：`scripts/demo_r3.sh:1-33`　用法：`scripts/demo_r3.sh safe|unsafe`*

- **用途**：示範「相信對方標頭」的破壞力：標頭的 `len` 不經檢查就拿來 `recv`，會爆掉固定大小的堆疊緩衝區。
- **設計**：
  - safe：`bn_recv()` 依序檢查 magic、version、`len <= BN_MAX_PAYLOAD` 且 `len <= cap`、CRC（`lib/proto.c:101-114`、`:128-134`），任何一項失敗回 -2，child 的 `violation()` 回 `ERROR` 後乾淨關閉。bad-len 對應 `ERROR BAD_LENGTH`。
  - unsafe：`--unsafe=validate` 略過這些檢查。`handshake()` 的緩衝區是 `char buf[4096]`（`server/session.c:192`），而 bad-len 宣稱的 `len = 0x00100000`（1 MiB），`bn_recv()` 內的 `readn(fd, buf, h->len)` 就把 1 MiB 寫進 4 KiB 的堆疊（DECISIONS R3），child 因 `-fstack-protector-strong` 印出 `stack smashing detected` 並被 SIGABRT 殺死，或直接 SIGSEGV。
  - 故意先放一個正常的 `viewer` 觀察者，證明「單一 child 崩潰不會影響其他 client 與 master」：這正是 fork-per-client 的隔離優勢。
- **細節**：
  - 第 9-12 行：`parse_mode "$1" validate`、`start_sim --loop`、`start_server $UNSAFE_OPT`。
  - 第 14-17 行：`bench watch --name viewer --duration 8 --out $WORK/viewer.bin` 放背景（`BG_PIDS+=($!)`），`sleep 1`。
  - 第 19-23 行：依序對 `bad-magic`、`bad-crc`、`bad-len` 執行 `bench-evil`，每次後 `sleep 0.5`。
  - 第 25-31 行：`server_alive && info_key server.clients` 同時成立（行程還活、listen socket 還在、且還能回應 INFO）才印綠色 `benchd alive: server.clients=... server.unsafe=...`；否則紅色 `benchd is not answering`；接著 `show_server_log 12`。
  - 第 32-33 行：`grep -E 'killed by signal|stack smashing' "$LOG_D" || c_ok "(none)"`。`killed by signal` 是 master 的 `reap_children()` 在 child 被訊號殺死時記的 `LOGW`（`server/main.c:103-104`）；`stack smashing detected` 是 glibc 寫到 stderr 的訊息，而 `start_server` 把 benchd（含 child）的 stderr 都導向 `$LOG_D`，所以兩者都在同一個日誌中。
  - 預期（README 表格）：unsafe 時出現其中一種；safe 時印 `(none)`。DECISIONS 還提到 unsafe 的 child 可能在持鎖時死亡，這時日誌還會出現 R7 的 `EOWNERDEAD recovered`。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`server_alive`、`info`、`info_key`、`show_server_log`；被觀察 → `lib/proto.c` 的 `bn_recv`、`server/session.c` 的 `handshake`。

## `scripts/demo_r4.sh`

R4 `readn` 的示範腳本：client 把封包一個位元組一個位元組地送，觀察伺服器是否把「還沒收完」誤判為協定錯誤。由工具行程（bash）執行，被觀察的是 `lib/proto.c` 的 `bn_recv` 與 `readn`。沒有定義函式。

### `scripts/demo_r4.sh`
*位置：`scripts/demo_r4.sh:1-13`　用法：`scripts/demo_r4.sh safe|unsafe`*

- **用途**：示範 TCP 是位元組串流而非訊息：一次 `recv()` 不保證收到完整標頭。
- **設計**：
  - safe：`bn_recv()` 用 `readn()` 迴圈，直到湊滿標頭與 payload 長度才解析（`lib/proto.c:92-95`、`:117-125`），因此即使對方每 50 ms 才送 1 個位元組也能還原封包。
  - unsafe：`--unsafe=readn` 時標頭只做一次 `recv(fd, raw, sizeof raw, 0)`（`raw` 是 `BN_HDR_LEN` 位元組的陣列），payload 也只做一次 `recv`（`lib/proto.c:86-91`、`:117-118`）。`raw` 預先 `memset` 為 0，第一次 `recv` 只拿到 1 個位元組，其餘位元組是 0，magic 不符，child 回 `BAD_MAGIC` 並關閉連線，client 其實沒有做錯事（README：「bogus BAD_MAGIC」）。
  - 這裡的 `bench-evil slow` 模式送出的是「HELLO + INFO_REQ」兩個封包，每個位元組 `send(…, 1)` 後 `sleep_ms(50)`（`tools/bench-evil.c:110-141`）；最後等 INFO 回覆最多 5 秒。
- **細節**：
  - 第 8-10 行：`parse_mode "$1" readn`、`start_sim --loop`、`start_server $UNSAFE_OPT`。
  - 第 11-12 行：`bench-evil slow 127.0.0.1:$PORT | head -8`：只看前 8 行輸出。safe 會看到 `got INFO ... reassembled the dribbled packets correctly`；unsafe 會看到 `no INFO reply (connection closed)` 之類的結果。
  - 第 13 行：`show_server_log 4`，unsafe 時最後幾行是 BAD_MAGIC 的協定違規記錄。
  - `test.sh` 的 `slow client still gets INFO` 檢查（safe 模式）用同一個 `bench-evil` 模式。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`show_server_log`；被觀察 → `lib/proto.c` 的 `bn_recv`。

## `scripts/demo_r5.sh`

R5 `timeout` 的示範腳本：32 個連上後一句話都不說的 socket 佔滿伺服器，觀察正常使用者能否進來。由工具行程（bash）執行，被觀察的是 child 的握手逾時（`server/session.c`）。沒有定義函式。

### `scripts/demo_r5.sh`
*位置：`scripts/demo_r5.sh:1-21`　用法：`scripts/demo_r5.sh safe|unsafe`*

- **用途**：示範 slowloris 式的資源耗盡：沈默連線佔住 child 與名額，伺服器對所有人關門。
- **設計**：
  - safe：child 在握手時最多等 `HELLO_TIMEOUT_MS = 5000`（`server/server.h:28`），逾時送 `ERROR HANDSHAKE_TIMEOUT` 並結束（`server/session.c:196-210`），名額在 child 結束時釋放；`session_main()` 在 fork 之後先開啟 TCP keepalive 並設定 `SO_RCVTIMEO/SO_SNDTIMEO`（參數為 `HELLO_TIMEOUT_MS / 1000` 與 `IDLE_TIMEOUT_MS / 1000`，`server/session.c:442-444`），握手完成後再把收送逾時都改成 `IDLE_TIMEOUT_MS = 60000`（第 235-236 行），主迴圈另以 `last_rx_ms` 檢查 60 秒閒置（第 462 行）。
  - unsafe：`--unsafe=timeout` 讓這些全部不設定，`bn_recv()` 在 `readn()` 上無限期阻塞，沈默連線永遠不會被清掉。
  - 必須讓 `--max-clients` 等於沈默連線數（32），才能用「滿了」這個可觀察的結果顯示差別。
- **細節**：
  - 第 7-9 行：`parse_mode "$1" timeout`、`start_sim --loop`、`start_server --max-clients 32 $UNSAFE_OPT`。
  - 第 10-13 行：`bench-evil silent --count 32 --sec 15` 放背景（`BG_PIDS+=($!)`），`sleep 1`。silent 模式只 `connect()` 不送 HELLO，並輪詢哪些連線被伺服器關閉。
  - 第 14-15 行（t=1 秒）：`info | head -3`。此時 32 個名額已被佔滿，所以 safe 與 unsafe 兩種模式的第一次嘗試都會被拒絕（回應 SERVER_BUSY），差別在 5 秒之後。
  - 第 16-19 行（t≈7 秒）：`sleep 6` 後再 `info`，成功就印綠色 `connected: server.clients=...`，失敗就印紅色 `refused: <錯誤訊息>`。預期 safe 成功（沈默連線在第 5 秒左右被切斷）、unsafe 仍被拒絕。
  - 第 20-21 行：`children_of | wc -l` 顯示目前 master 的直接子行程數（含 reader），再 `show_server_log 4`。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`info`、`children_of`、`show_server_log`；被觀察 → `server/session.c` 的 `handshake`、`session_main`。

## `scripts/demo_r6.sh`

R6 `ctrl-release` 的示範腳本：持有控制權的 client 不告而別（沒有 BYE）之後，其他人能不能拿到控制權。由工具行程（bash）執行，被觀察的是控制權釋放路徑（`server/control.c`、`server/session.c` 的 `finish`、`server/main.c` 的 `reap_children`）。沒有定義函式。

### `scripts/demo_r6.sh`
*位置：`scripts/demo_r6.sh:1-28`　用法：`scripts/demo_r6.sh safe|unsafe`*

- **用途**：示範「持有者死亡後控制權永遠被鎖住」的問題，以及伺服器如何避免。
- **設計**：
  - safe 有多條釋放路徑互相備援：child 的 `finish()` 發 `PIPE_RELEASE` 給 reader（`server/session.c:72-73`）、master 的 SIGCHLD 路徑直接在 shm 釋放（`server/main.c:95-96`）、`ctrl_take_locked()` 在 TAKE 時用 `kill(ctrl_pid, 0)` 檢查持有者是否已死（`server/control.c:43-49`）、reader 的閒置檢查（`ctrl_check_idle_locked`）。
  - unsafe：`--unsafe=ctrl-release` 讓這四條全部失效（DECISIONS R6），`ctrl_pid` 永遠指向已死的 pid。`ctrl_auto_release_in()` 回傳 `UINT32_MAX`（`server/control.c:32-33`），放進 `CTRL_RESULT` 的 `auto_release_in_sec`；`bench send` 看到 `UINT32_MAX` 就印「no auto-release」（`client/subcmd.c:266-268`）。
  - 用 `bench-evil take-and-die` 製造「拿到控制權後 `_exit(1)`」，沒有 BYE 也沒有關閉握手。
- **細節**：
  - 第 9-12 行：`parse_mode "$1" ctrl-release`、`start_sim`（單次播放）、`start_server $UNSAFE_OPT`。
  - 第 14-16 行：`bench-evil take-and-die --name crashy`，`sleep 0.5` 讓伺服器偵測斷線。
  - 第 18-19 行：`info | grep -E '^ctrl\.'` 印出伺服器眼中的控制權狀態：safe 為 `ctrl.holder=(none)`，unsafe 仍顯示 `crashy`（`server/sysinfo.c:170`）。
  - 第 21-24 行：`bench send ... --name alice --wait 800 'version\r'`，用 `rc=$?` 判斷：`rc == 0` 綠色 `alice got control`；否則紅色 `alice was refused (exit $rc)`。被拒時 `sub_send` 回傳 2（`client/subcmd.c`）。
  - 第 26-28 行：`bench tail -n 4K | grep -a 'control'` 顯示歷史中的 `control taken by ...` / `control released by ...` 事件；再 `show_server_log 8`。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`info`、`show_server_log`；被觀察 → `server/control.c`、`server/session.c`、`server/main.c`。

## `scripts/demo_r7.sh`

R7 `robust-mutex` 的示範腳本：某個 child 在持有共享記憶體 mutex 時死亡，其他行程是否還能繼續。由工具行程（bash）執行，被觀察的是 `server/robust.c` 的 `shm_mutex_init`、`shm_lock`。這個腳本需要 debug 版（fault injection 只在 `-DBENCH_FAULT_INJECTION` 的 debug 編譯，見 `Makefile:18`）。沒有定義函式。

### `scripts/demo_r7.sh`
*位置：`scripts/demo_r7.sh:1-45`　用法：`scripts/demo_r7.sh safe|unsafe`*

- **用途**：示範行程間 mutex 的致命弱點（owner 死亡 = 永久死鎖），以及 `PTHREAD_MUTEX_ROBUST` 的恢復流程。
- **設計**：
  - safe：mutex 用 `PTHREAD_PROCESS_SHARED | PTHREAD_MUTEX_ROBUST` 初始化。下一個加鎖者拿到 `EOWNERDEAD`，在 `shm_lock()` 內修復（`ring_repair`、檢查 `ctrl_pid` 是否已死、`slots_recount`）、`lock_recoveries++`、呼叫 `pthread_mutex_consistent()`，並在 `shm_unlock()` 之後才記 `EOWNERDEAD recovered`（鎖內不做 I/O，DECISIONS）。
  - unsafe：`--unsafe=robust-mutex` 讓 `shm_mutex_init` 不設 `ROBUST` 屬性（`server/robust.c:33-34`），owner 死掉後鎖永遠不會被釋放，所有要鎖的行程（其他 child、reader、新連線）都卡死。
  - 故障注入：`BENCH_FAULT=die_in_lock:3` 讓第 3 個連線的 child 在握手完成後 `shm_lock(g_shm); abort();`（`server/session.c:290-297`），並先關掉 core dump（`setrlimit(RLIMIT_CORE, {0,0})`）以免產生巨大檔案。
- **細節**：
  - 第 11-13 行（在載入 lib 之前）：`ROOT_GUESS` 由腳本路徑推出；`BIN=${BIN:-$ROOT_GUESS/build/debug/bin}`；若 `$BIN/benchd` 不存在，執行 `make -C "$ROOT_GUESS" debug`。之所以在 `source lib.sh` 之前設定，是因為 `lib.sh` 的 `BIN` 預設值是 release 目錄。第 14-16 行載入 lib、`need_bins`、`parse_mode "$1" robust-mutex`。
  - 第 18-19 行：`start_sim --loop`；`BENCH_FAULT=die_in_lock:3 start_server $UNSAFE_OPT`。bash 讓函式呼叫前的環境變數指派在函式執行期間有效，所以 benchd 會繼承 `BENCH_FAULT`。
  - 第 21-28 行：啟動 `viewer1`、`viewer2` 兩個 `bench watch --duration 12`（連線 #1、#2），輸出存 `$WORK/viewerN.bin`，`BG_PIDS+=($!)`；`sleep 3` 後 `b1` 取 `viewer1.bin` 大小。
  - 第 30-36 行：`timeout 3 bench watch --name victim --duration 1` 是連線 #3，child 會在持鎖時 `abort()`；用 `timeout 3` 是因為 unsafe 下可能卡住。`sleep 1` 後 `b2`，再 `sleep 4` 後 `b3`，印出 `before / right after / 4 s later`。
  - 第 37 行：`b3 > b2` 綠色 `viewers keep receiving data`；否則紅色 `viewers are FROZEN`。
  - 第 39-44 行：以 `timeout 3 bench info --raw` 檢查新 client 還能不能拿到系統資訊；成功時顯示 `server.clients` 與 `server.lock_recoveries`（safe 應 ≥ 1，`server/sysinfo.c:156`），失敗（或逾時）印 `no — the server hangs`。
  - 第 45 行：`show_server_log 8`，safe 時可以看到 `EOWNERDEAD recovered`。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`show_server_log`；被觀察 → `server/robust.c` 的 `shm_mutex_init`、`shm_lock`、`shm_unlock`。

## `scripts/demo_r8.sh`

R8 `dev-reopen` 的示範腳本：UART 裝置被拔掉再插回，reader 行程會如何反應。由工具行程（bash）執行，被觀察的是 reader（`server/reader.c`）的 `read()` 錯誤處理與重新開啟邏輯。沒有定義函式。

### `scripts/demo_r8.sh`
*位置：`scripts/demo_r8.sh:1-34`　用法：`scripts/demo_r8.sh safe|unsafe`*

- **用途**：示範「裝置錯誤只重試不處理」的兩種後果：CPU 空轉，以及裝置回來後永遠不會恢復。
- **設計**：
  - safe：reader 的 `read()` 回 0 或非 EAGAIN/EINTR 錯誤時呼叫 `dev_lost()`（關 fd、`dev_state = DEV_LOST`、寫入 `device lost` 事件），並每 500 ms 嘗試 `dev_open()`，成功時 `dev_back()` 寫入 `device reconnected` 事件（`server/reader.c:251-257`、`:272-277`）。
  - unsafe：`--unsafe=dev-reopen` 時遇到這些錯誤只 `continue`（第 252-255 行）：fd 沒關、狀態仍是 UP，而 pty 主端已關閉，`poll()` 立刻回 POLLHUP，於是迴圈全速空轉（100% CPU）；因為從不重開，benchsim 重啟後建立的新 pty 也永遠不會被接上。
  - 「拔線」用 `stop_sim`（SIGTERM 讓 benchsim 刪掉 symlink 並結束，`tools/benchsim.c` 主迴圈後半），「插回」用 `start_sim` 重新建立 symlink。
- **細節**：
  - 第 9-13 行：`parse_mode "$1" dev-reopen`、`start_sim --loop`、`start_server $UNSAFE_OPT`、`sleep 1`。
  - 第 14-15 行：`R=$(reader_pid)`；印出 reader 的 CPU 使用率（`cpu_pct`，約 1 秒）與 `dev.state`、`dev.rx_bytes`。
  - 第 17-22 行：`stop_sim`，`sleep 1`；`c_note` 顯示此刻的 `dev.state`（safe 為 `LOST`；unsafe 因為從不呼叫 `dev_lost`，仍是 `UP`）；再量一次 reader CPU，並用 `top -b -n 1 -p "$R" | tail -2` 顯示該行程的一列。
  - 第 24-30 行：`start_sim --loop`，`sleep 2`；`rx1`、`rx2` 相隔 2 秒讀取 `dev.rx_bytes`；印出 `dev.state` 與 `dev.reconnects`；`rx2 > rx1`（`2>/dev/null` 吞掉變數非數字的錯誤）綠色 `data is flowing again`，否則紅色 `no new data from the device`；再量一次 CPU。
  - 第 32-34 行：`bench tail -n 64K | grep -a '──── device'` 顯示歷史中的 `device lost` / `device reconnected` 事件（`client/subcmd.c` 的 `print_event` 以 `──── <文字> <時間> ────` 格式輸出）；再 `show_server_log 6`。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`stop_sim`、`start_server`、`reader_pid`、`cpu_pct`、`info`、`info_key`、`show_server_log`；被觀察 → `server/reader.c` 的 `reader_main`、`dev_lost`、`dev_back`。

## `scripts/demo_r9.sh`

R9 `max-clients` 的示範腳本：以最快速度開 200 條連線，觀察伺服器是否無限 fork。由工具行程（bash）執行，被觀察的是 master 的 `handle_accept()`（`server/main.c`）。沒有定義函式。

### `scripts/demo_r9.sh`
*位置：`scripts/demo_r9.sh:1-20`　用法：`scripts/demo_r9.sh safe|unsafe`*

- **用途**：示範「一連線一個行程」模型若沒有上限，被連線洪水打到會把機器的行程數用光（fork bomb 效果）。
- **設計**：
  - safe：`slots_in_use() >= max_clients` 時呼叫 `reject_busy()` 回 `ERROR SERVER_BUSY` 並關閉（`server/main.c:164-171`），所以最多 32 個 child。
  - unsafe：`--unsafe=max-clients` 跳過該檢查，每個連線都 `fork()`，直到 `fork()` 失敗（EAGAIN），失敗時記 `fork for <peer>: ...`（`server/main.c:177`）並回 `SERVER_BUSY`。
  - 為了保護執行腳本的機器，腳本在啟動 benchd 之前先 `ulimit -u` 限制行程數；因為 `RLIMIT_NPROC` 計算的是該使用者的 task（含 thread），不是只有行程，所以用 `ps -L -u $(id -u)` 的行數（每個 task 一行）加 150 作為上限（DECISIONS Scripts）。限制在 shell 中設定，benchd 與其 child 會繼承。
- **細節**：
  - 第 8 行：`parse_mode "$1" max-clients`。第 10-11 行：`cap=$(( $(ps -L -u "$(id -u)" --no-headers | wc -l) + 150 ))`，`ulimit -u "$cap" 2>/dev/null && c_note ...`；設定失敗（例如硬限制較低）時靜默跳過，不印提示。
  - 第 12-13 行：`start_sim --loop`、`start_server $UNSAFE_OPT`（預設 `--max-clients 32`）。
  - 第 14-17 行：`bench-evil flood --count 200 --sec 6` 放背景（`BG_PIDS+=($!)`）；`sleep 4`。flood 逐一連線並送 HELLO，再分類回覆（`WELCOME` 或 `ERROR`）。
  - 第 18 行：`children_of | wc -l` 顯示 benchd 的直接子行程數。safe 約 32 個 child + 1 個 reader；unsafe 可到 `ulimit` 上限。第 19 行：分別 `grep -c` 日誌中的 `fork for`（fork 失敗）與 `server full`（忙碌拒絕）數量。
  - 第 20 行：`wait "${BG_PIDS[-1]}"`（負索引需要 bash 4.3 以上）等 flood 工作結束，讓輸出完整。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`children_of`；被觀察 → `server/main.c` 的 `handle_accept`、`reject_busy`。

## `scripts/demo_r10.sh`

R10 `overrun` 的示範腳本：一個 client 停止讀取，落後的距離超過只有 4 KiB 的歷史 ring，觀察資料是否被誤送。由工具行程（bash）執行，被觀察的是 child 的 `pump()`（`server/session.c`）與 ring 的覆寫處理。這個檔案另外內嵌一支 awk 程式做完整性檢查，其中定義了 awk 函式 `flush_pending`。

### `scripts/demo_r10.sh`
*位置：`scripts/demo_r10.sh:1-37`　用法：`scripts/demo_r10.sh safe|unsafe`*

- **用途**：示範環形緩衝區被覆寫（overrun）時，慢速讀者要嘛被明確告知「掉了 N 位元組」，要嘛無聲地讀到被覆寫的垃圾。
- **設計**：
  - 製造可觀察的落後：產生 3000 行編號行 `line 0001: the quick brown fox jumps over the lazy dog`（`seq -w` 補零）作為 benchsim 的輸入，以 `-b 921600` 快速播放（約 92 KB/s，沒有 `#delay`），配合 `--history 4K`（程式下限，`server/main.c` 會把小於 4096 的值提高到 4096）使 ring 很小。
  - `bench-evil lag` 在 `connect()` 前把 `SO_RCVBUF` 設成 4 KiB（避免 loopback 自動調整把落後藏在數 MB 緩衝區裡，`tools/bench-evil.c` `dial_small_rcvbuf`），握手後 15 秒不讀 socket（剩餘秒數為 10 的倍數時送 PING 維持連線），再讀 2 秒。
  - safe：`pump()` 在鎖內檢查 `s->off < g_shm->tail`，發現被追過就算出被丟掉的位元組數、跳到 `tail`，並送出 `EVENT DROPPED`（`server/session.c:138-141`）；資料保持乾淨，只是有明確標註的缺口。
  - unsafe：`--unsafe=overrun` 略過 `tail` 檢查，child 沿著已經過時的 offset 讀 ring，遇到被覆寫的記錄標頭時長度亂碼，只能夾到 1024 繼續（`ring_copy`，`server/ring.c:118-123`，DECISIONS R10），於是被覆寫的位元組（包含記錄標頭）進到螢幕。
  - 檢查用 awk 逐行驗證，而不是只比對字串，因為「缺口」本身是合法的（只要有 `DROPPED` 事件交代）：差別在於是否「無聲」。
- **細節**：
  - 第 9-14 行：`parse_mode "$1" overrun`；`LOGF=$WORK/numbered.log`；第 12 行用 `for i in $(seq -w 1 3000); do echo "line $i: ..."; done >"$LOGF"`；`start_sim -l "$LOGF" -b 921600 --loop`；`start_server --history 4K $UNSAFE_OPT`。
  - 第 15-16 行：`bench-evil lag --sec 15 --after 2 >"$WORK/lag.txt"`。
  - 第 17-18 行：`sed -n '/^-----/,$p'` 取出 lag 輸出中第一條 `-----` 分隔線之後的內容，`head -25 | cut -c1-100` 顯示恢復讀取後收到的前 25 行。
  - 第 19-36 行：完整性檢查。管線 `tr -d '\r' | sed -n '/^-----/,$p' | LC_ALL=C awk '...'`，awk 結束碼為 `exit (bad + jumps > 0)`，第 36 行 `$?` 取的就是管線最後一個命令（awk）的結束碼；`0` 印綠色 `every gap was announced by a DROPPED event; no corrupted data`，否則紅色 `data was silently lost or corrupted`。
  - awk 規則依序為：`/^-----/` 分隔線（設 `frag=1`，跳過）；`/EVENT DROPPED/`（`drops++`，清掉 `pending`，`frag=1`）；空行跳過；完整行 `^line [0-9]+: the quick brown fox jumps over the lazy dog$`（先 `flush_pending()`，取編號 `n`，若 `prev` 有值且 `n != prev+1`、`n != 1`（benchsim 輪播回到第 1 行）且 `gap` 為 0，就 `jumps++`；然後更新 `prev`、清 `gap`、`frag`）；其他行：若 `frag` 為真，視為被 DROPPED 切斷的行尾，`frag=0; gap=1` 並略過，否則 `flush_pending()` 後把該行存入 `pending` 並設 `gap=1`。`END` 再 `flush_pending()`，印出 `N DROPPED events, M silent jumps in line numbers, K corrupted lines`。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`；被觀察 → `server/session.c` 的 `pump`、`server/ring.c`。

### `flush_pending`
*位置：`scripts/demo_r10.sh:22`　簽章：`function flush_pending()`（awk 函式，無參數、無回傳值，使用 awk 全域變數 `pending`、`bad`）*

- **用途**：結算「目前暫存的、不屬於完整編號行的那一行」：如果還有暫存內容，就把它計為一行損毀資料並印出前 72 個字元，然後清空暫存。
- **設計**：
  - 不能看到不合格的行就立刻判定損毀，因為 `bench-evil lag` 會在 `DROPPED` 事件前後各切斷一行（缺口前的半行）。所以先把這種行放進 `pending`，之後如果遇到 `EVENT DROPPED`（規則：`pending = ""`）就視為「被誠實宣告的缺口」而丟掉；只有在下一個完整行出現或輸入結束（`END`）時它還在，才用本函式判為損毀。
  - 輸出只顯示前 4 個損毀範例（`bad <= 4`），避免 unsafe 模式輸出被洗版，`bad` 仍會完整計數，用於 `END` 的摘要與結束碼。
- **細節**：
  - 第 22 行：`if (pending != "") { bad++; if (bad <= 4) print "  corrupted: " substr(pending, 1, 72) } pending = ""`。
  - 在 awk 裡函式內的變數是全域（沒有宣告為 local 參數），所以直接修改外層的 `pending`、`bad`。
  - 呼叫點（`demo_r10.sh`）：完整行規則（第 27 行）、非完整行規則（第 32 行）、`END`（第 33 行）。
- **關係**：被誰呼叫 → `demo_r10.sh` 內嵌 awk 程式的上述三處；呼叫 → `print`、`substr`（awk 內建）。

## `scripts/demo_r11.sh`

R11 `term-restore` 的示範腳本（針對 client）：互動式 `bench` 崩潰後，終端機是否停留在 raw 模式。由工具行程（bash）執行，被測試的是 `bench` client 的 `term_init()`（`client/term.c`）。因為 `bench` 的互動模式需要真正的 tty，腳本借助 detached GNU `screen`。沒有定義函式。

### `scripts/demo_r11.sh`
*位置：`scripts/demo_r11.sh:1-24`　用法：`scripts/demo_r11.sh safe|unsafe`*

- **用途**：示範「TUI 程式必須在崩潰時還原終端」：unsafe 讓使用者的 shell 留在無回顯、無行編輯的 raw 模式。
- **設計**：
  - safe：`term_init()` 在設定 raw 模式後，用 `atexit(term_restore)` 並對 `SIGINT SIGTERM SIGHUP SIGSEGV SIGABRT SIGBUS SIGQUIT` 安裝 `on_fatal`（`client/term.c:136-144`）。handler 還原 termios 與捲動區，再把訊號恢復為 `SIG_DFL` 並 `raise(sig)`，「用原本會死的方式死」（`client/term.c:106-115`），所以結束狀態仍是 SIGSEGV。
  - unsafe：`--unsafe=term-restore` 時兩者都不註冊，SIGSEGV 直接殺掉行程，termios 維持在 raw。
  - 注意 `--unsafe=...` 是傳給 client（`bench`），伺服器是以正常方式啟動（`start_server` 沒有帶 `$UNSAFE_OPT`），因為 R11 是 client 機制（DECISIONS：client 與 server 各自忽略對方的名稱）。
- **細節**：
  - 第 8 行：沒有 `screen` 就印 `needs GNU screen` 並 `exit 1`。第 9-11 行：`parse_mode "$1" term-restore`、`start_sim --loop`、`start_server`。
  - 第 12-13 行：`SESSION=r11-$$`；在 detached screen 內以 `LC_ALL=C.UTF-8` 執行 `bash -c "'$BIN/bench' $UNSAFE_OPT --name crashy 127.0.0.1:$PORT; echo \"exit=\$?\" >'$WORK/exit.txt'; stty -a >'$WORK/stty.txt'; sleep 2"`，也就是 client 結束後（不論怎麼結束）立刻把退出碼與 `stty -a` 寫入檔案。
  - 第 14-18 行：`sleep 1.5` 等 client 啟動；`P=$(pgrep -f "^$BIN/bench .*--name crashy" | head -1)` 取得 pid；`kill -SEGV "$P"`；`sleep 1`。
  - 第 19-23 行：`cat exit.txt` 顯示退出碼（safe 與 unsafe 都應是被 SIGSEGV 殺死的 139）；`grep -oE -- '-?(icanon|echo|isig|icrnl)( |$)' stty.txt` 列出四個旗標狀態；若 `stty.txt` 含 `-icanon`（關閉 canonical 模式）就紅色 `terminal left in RAW mode`，否則綠色 `terminal restored (icanon echo)`。
  - 第 24 行：`screen -S "$SESSION" -X quit` 關掉 screen。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`need_bins`、`c_head`、`c_ok`、`c_bad`；被觀察 → `client/term.c` 的 `term_init`、`on_fatal`。

## `scripts/demo_r12.sh`

R12 `reconnect` 的示範腳本（針對 client）：benchd 重啟時，已連線的互動式 `bench` 是退出還是自動重連。由工具行程（bash）執行，被測試的是 `client/session.c` 的 `app_disconnect()` 與 `handle_welcome()`。同樣借助 detached GNU `screen` 提供 tty。沒有定義函式。

### `scripts/demo_r12.sh`
*位置：`scripts/demo_r12.sh:1-23`　用法：`scripts/demo_r12.sh safe|unsafe`*

- **用途**：示範伺服器重啟對長時間開著的 client 的影響。
- **設計**：
  - safe：`app_disconnect()` 之後以退避時間表 `{1, 2, 4, 8, 16, 30}` 秒重試（`client/session.c:121`、`:139-141`），重連成功後若 WELCOME 的 `server_start_ns` 與上次不同，就在畫面插入 `server restarted` 分隔線（否則是 `reconnected`，`client/session.c:195`），並重新重播歷史。
  - unsafe：`--unsafe=reconnect` 時 `app_disconnect()` 直接 `quit = 2`、顯示 `connection lost` 後離開（`client/session.c:133-138`），行程結束。
  - 伺服器端是用正常方式重啟：`kill "$D_PID"` 觸發有序關機（送 `server shutdown` 事件，然後結束），`start_server` 再啟動新的 benchd（新的 `D_PID`，新的 `server_start_ns`）；因為 `SO_REUSEADDR` 永遠開啟，同一埠可以立刻重新監聽。
- **細節**：
  - 第 7 行：沒有 `screen` 則 `exit 1`。第 8-10 行：`parse_mode "$1" reconnect`、`start_sim --loop`、`start_server`。
  - 第 11-12 行：`SESSION=r12-$$`；在 detached screen 內以 `LC_ALL=C.UTF-8` 執行 `bash -c "LANG=C.UTF-8 '$BIN/bench' $UNSAFE_OPT --name survivor 127.0.0.1:$PORT; echo \"client exited: \$?\"; sleep 30"`，client 結束時畫面上會留下 `client exited: N`，並維持 30 秒以便擷取。
  - 第 13-17 行：`sleep 1.5`；`c_head "restart benchd"`；`kill "$D_PID"; wait "$D_PID"`；`sleep 1`；`start_server`（會把新的 pid 加入 `BG_PIDS` 並覆寫 `D_PID`）。
  - 第 18-20 行：`sleep 3` 等退避重連；`screen -X hardcopy "$WORK/screen.txt"` 把 screen 當前畫面存檔；`grep -a -E 'server shutdown|server restarted|replay end|client exited|RECONNECT|VIEW'` 擷取關鍵字，`cut -c1-80`。這些關鍵字分別來自關機事件、重連分隔線（`client/session.c:195`）、重播結束分隔線（`replay end · live`，`:234`）、unsafe 的 `client exited`、狀態列的 `RECONNECTING` 與 `VIEW` 徽章（`client/ui.c`）。
  - 第 21-22 行：`pgrep -f "^$BIN/bench .*--name survivor"` 找得到行程就綠色 `client still running and reconnected`，否則紅色 `client is gone`。
  - 第 23 行：`screen -S "$SESSION" -X quit`。
- **關係**：被誰呼叫 → 手動執行；呼叫 → `lib.sh` 的 `parse_mode`、`start_sim`、`start_server`、`need_bins`、`c_head`、`c_ok`、`c_bad`；被觀察 → `client/session.c` 的 `app_disconnect`、`handle_welcome`。
