# 2 分鐘 Demo 影片：逐字稿與操作步驟

依照 `docs/screenshots/README.md` 的「2 分鐘影片建議腳本」展開。每一段分成三部分：

- **操作**：你在鏡頭前實際要做的事（指令、按鍵、滑鼠點哪裡）
- **旁白**：逐字稿，照念即可
- **為什麼**：這段在證明哪一項課堂要求、為什麼要這樣拍（只給你自己看，不用念）

語速抓每秒約 5 個字。每段旁白都控制在該段秒數內，念不完就刪括號裡的句子。

---

## 錄影前準備（不入鏡）

### 1. 建置與預先產生封包檔

```bash
cd ~/hw/nsc/midterm
make && make debug
make capture
```

- `make debug` 是為了最後一段要比較 release / debug 執行檔裡的 TRACE 字串，必須先有 debug 版。
- `make capture` **一定要在啟動主要 benchd 之前先跑**：它會自己用 port 9000 起一組 server。
  另外它會用 `sudo tcpdump`，鏡頭前輸入密碼不好看，而且整段要跑十幾秒，所以先跑好，
  影片裡直接開產生的 `captures/session.pcap`。

### 2. 開好四個終端機

| 終端機 | 用途 | 是否入鏡 |
|---|---|---|
| T0 | 跑 benchsim + benchd（背景服務） | 不入鏡 |
| T1 | 主要操作：make、ldd、stress、demo、kill | 入鏡 |
| T2 | alice 的 client | 入鏡（與 T3 左右並排） |
| T3 | bob 的 client | 入鏡 |

T0 執行：

```bash
cd ~/hw/nsc/midterm
build/bin/benchsim -l sim/pynq_boot.log --loop &
build/bin/benchd -d /tmp/benchsim0 2>/tmp/benchd.log &
```

- benchd 的 log 導到 `/tmp/benchd.log`，否則每有 client 連線，log 就會噴在終端機上弄亂畫面。

T1、T2、T3 都先執行（指令比較短，鏡頭前打字比較快）：

```bash
cd ~/hw/nsc/midterm && export PATH=$PWD/build/bin:$PATH
```

### 3. 注意 port 衝突（很重要）

`make stress`、`scripts/demo_r2.sh` 預設也用 **port 9000 和 `/tmp/benchsim0`**，會跟 T0 的主要
server 撞在一起（stress 會連到錯的 server，結果判定 FAIL）。所以在 T1 先設定：

```bash
export PORT=9100 DEV=/tmp/benchsim1
```

這樣腳本會另外起一組自己的 benchsim + benchd，不影響 T0。

### 4. 其他

- 終端機字體放大到 1080p 錄影時看得清楚的程度（約 16–18pt），清空畫面（`clear`）。
- 先把架構圖準備好：用編輯器或 `less README.md` 捲到 `## Architecture` 那段。
- 每段建議分開錄，再剪接；`make stress` 等待的那幾秒可以剪掉或加速。

---

## 0:00–0:15 架構圖

**操作**

- 畫面停在 `README.md` 的 `## Architecture` ASCII 圖。
- 邊講邊用滑鼠游標依序指：`master ── accept() ── fork()` → `child` → 中間的 `shared memory` /
  `ring buffer` → 下方的 `reader` → `serial device`。

**旁白**

> 這是我的期中專題 benchd，讓很多人透過 TCP 共用同一個序列埠。master 只負責 accept，每來一個
> client 就 fork 一個 child；序列埠只由 reader 這個行程獨佔；收到的資料放進共享記憶體的 ring
> buffer，每個 child 用自己的位置去讀。

**為什麼**

- 老師先看懂架構，後面每個 demo 才知道在證明什麼。
- 「reader 獨佔序列埠」說明為什麼多個 child 不會互搶 byte；「每個 child 自己的位置」說明為什麼
  慢的 client 不會拖累別人。這兩點是整個設計的核心，所以要用游標指出來。

---

## 0:15–0:30 建置與共用 `.so`

**操作**（T1）

```bash
make clean && make
```

等編譯跑完（應該零 warning），接著：

```bash
ldd build/bin/benchd build/bin/bench | grep libbench
```

游標指兩行 `libbench.so.1 => .../build/lib/libbench.so.1`。

**旁白**

> 用 Makefile 建置，一次產生 server、client 和共用函式庫。ldd 可以看到 benchd 和 bench 兩支程式
> 都連到同一個 libbench.so.1，封包編解碼、CRC 和 log 都放在這裡，兩邊共用。

**為什麼**

- 對應「Makefile 建置」與「共用 `.so`」兩項要求。
- 用 `make clean` 再 `make` 是為了讓老師看到真的從頭編譯、沒有 warning；如果編譯太久，可以
  只錄最後幾行。
- `grep libbench` 只留重點兩行，不然 ldd 會列出 libc 等一堆無關的東西。

---

## 0:30–1:00 多人共用主控台（alice / bob）

**操作**（T2、T3 左右並排）

1. T2：`bench localhost --name alice`，畫面開始跑開機 log。
2. 等 2–3 秒，T3：`bench localhost --name bob`。bob 一連上就先看到歷史紀錄回放，畫面跟 alice 對齊。
3. T2（alice）：按 `Ctrl-]` 再按 `t` 取得控制權，狀態列變成綠色 `CTRL`。
4. T2：按一次 `Enter`（中斷 autoboot，出現 `=>` 提示字元），輸入 `version` 再 `Enter`。
   兩邊同時出現 U-Boot 版本資訊；bob 那邊會出現一條 `control taken by alice` 的分隔線。
5. T3（bob）：隨便打一個字母，狀態列閃出 `read-only — Ctrl-] t 取得控制`，字元沒有送出。
6. T3：按 `Ctrl-]` 再按 `i`，系統資訊框直接印在輸出區裡（不是彈出視窗，不用關）。停 2 秒讓觀眾
   看清楚。因為 alice 已經讓板子停在 `=>` 提示字元，沒有新的輸出，資訊框不會被捲走。
7. 兩邊各按 `Ctrl-]` `q` 離開（可以剪掉）。

**旁白**

> 現在開兩個 client：alice 和 bob。比較晚連上的 bob 會先收到歷史紀錄回放，所以畫面跟 alice 一樣。
> alice 按 Ctrl-右中括號、t 取得控制權，輸入 version，板子的回應兩邊同時看到。bob 這時候打字會
> 被擋下，因為同一時間只有一個人能控制，避免兩個人的指令混在一起。按 Ctrl-右中括號、i，可以跟
> server 要系統資訊：主機、序列埠設定、連線數，還有控制權在誰手上。

**為什麼**

- 這段一次展示三件事：廣播（兩人看到一樣的輸出）、控制權互斥（bob 唯讀）、
  「client 向 server 要系統資訊」這項要求（`INFO_REQ` / `INFO`）。
- bob 晚 2–3 秒再連，是為了展示歷史回放；同時開就看不出差別。
- 先按 `Enter` 是因為 benchsim 正在播放開機 log，任何按鍵會像真的 U-Boot 一樣中斷 autoboot、
  進入 `=>` 提示字元，之後 `version` 才會有回應。
- 讓 bob 故意打字，是用畫面證明「唯讀」不是口頭說說。

---

## 1:00–1:15 20 個 client 並行

**操作**（T1，已設定 `PORT=9100 DEV=/tmp/benchsim1`）

```bash
make stress
```

- `t=5s snapshot` 出現時，游標指 `pstree` 裡一排 `benchd-child` 和一個 `benchd-reader`，
  再指 `20 connections`。
- 中間等待 20 秒的部分剪掉，直接接到最後的 sha256 表格和綠色 `PASS`。
- 想縮短可以改用 `DUR=8 make stress`。

**旁白**

> make stress 同時開 20 個 client。pstree 看到 master 底下 20 個 child 加一個 reader，ss 也有 20
> 條連線。最後 20 個 client 收到的資料，sha256 完全相同，證明每個人收到的廣播都一致。

**為什麼**

- 對應「fork()、至少 10 個 client 並行」的 runtime proof。pstree 證明真的是 fork 出來的行程
  （不是 thread），ss 證明連線同時存在，sha256 證明資料沒有漏或亂序。
- 一定要改 `PORT`/`DEV`，否則腳本會連到 T0 那台 server，結果判成 FAIL。

---

## 1:15–1:30 封包擷取與 Wireshark

**操作**（T1）

```bash
wireshark -X lua_script:tools/benchd.lua captures/session.pcap &
```

1. 上方 display filter 輸入 `benchd`，按 Enter，只留下自訂協定的封包。
2. 游標從上往下掃 Info 欄：`HELLO` → `WELCOME` → `REPLAY_BEGIN` … → `BYE`。
3. 點一個 `INFO` 封包，在下方展開 benchd 那層，指出 Magic `0x424E`、Version、Type、Length、
   `CRC32 valid`，以及裡面的系統資訊內容。
4. （時間夠再做）開 `captures/rst.pcap`，filter 改成 `tcp.flags.reset == 1`，指出紅色的 RST。

**旁白**

> 封包用 tcpdump 在 loopback 上擷取。我寫了一個 Lua dissector，讓 Wireshark 能解析我自訂的
> 二進位協定：magic、版本、型別、長度，還有 CRC32 驗證。點開這個 INFO 封包，就是剛才 bob 看到的
> 系統資訊。

**為什麼**

- 對應「封包擷取與分析」。只秀 tcpdump 的十六進位不算「分析」，有 dissector 把欄位解開、
  驗 CRC，才能證明你真的定義並理解了自己的協定。
- 封包檔是錄影前先抓好的，原因見「錄影前準備」第 1 點；旁白照實說是用 tcpdump 擷取即可。
- 把 INFO 封包和上一段的系統資訊框連起來講，讓觀眾知道網路上傳的就是畫面上看到的東西。

---

## 1:30–1:50 強健性：有保護 vs 沒保護

**操作**（T1，同樣在 `PORT=9100 DEV=/tmp/benchsim1` 底下）

```bash
scripts/demo_r2.sh unsafe
```

結尾出現紅字 `NO — master exited with status 141 (PIPE)`，游標指它。接著：

```bash
scripts/demo_r2.sh safe
```

結尾出現綠字 `yes — master pid ... still listening`，游標指它。

**旁白**

> 強健性示範：server 已經滿載時，再有 40 個 client 連上來馬上送 RST。關掉保護的 unsafe 版本，
> master 回應時收到 SIGPIPE，直接被殺掉，整個服務消失。預設的 safe 版本忽略 SIGPIPE，只記錄錯誤，
> 繼續服務。這樣的有無對照我一共做了 12 組。

**為什麼**

- 對應「強健性：有/無對照」。同一個攻擊、同一支程式，只差一個開關，結果天差地別，
  是最有說服力的拍法。
- 先跑 unsafe 再跑 safe：先讓觀眾看到問題，再看到解法，順序反過來就沒有張力。
- 挑 r2 是因為它最致命（master 死掉＝所有人斷線），而且跑得快（各約 5 秒）。
  最後一句帶出「12 組」，讓老師知道其他的在報告裡。

---

## 1:50–2:00 兩層 debug log

**操作**（T1，可以先把指令貼好，按 Enter 就好）

```bash
bench info --raw localhost | grep log_level
kill -USR1 $(pgrep -x benchd)
bench info --raw localhost | grep log_level
strings build/bin/benchd | grep -c 'ring append'
strings build/debug/bin/benchd | grep -c 'ring append'
```

依序看到 `INFO` → `DEBUG`，接著 `0` 和 `1`。

**旁白**

> log 分兩層：執行期用 kill USR1，不用重啟就從 INFO 切到 DEBUG；編譯期由 LOG_COMPILE_LEVEL 決定，
> release 版裡 TRACE 的字串根本不存在。謝謝觀看。

**為什麼**

- 對應「兩層 debug log」。執行期那層的重點是「不用重啟」，所以要用 `bench info` 前後各查一次，
  證明是同一個在跑的 server 改了等級。
- 編譯期那層用 `strings` 數字串：release 是 `0`、debug 是 `1`，證明 TRACE 程式碼在編譯時就被拿掉，
  不只是執行時不印。
- `pgrep -x benchd` 只會抓到 master（child 和 reader 都改了名字叫 `benchd-child` / `benchd-reader`），
  所以訊號只送給 master 一個。
- 這段只有 10 秒，指令建議事先貼在剪貼簿或用 shell history 叫出來，不要現場打。

---

## 錄完後收尾

```bash
kill %1 %2      # 在 T0 停掉 benchsim 和 benchd
```

## 時間檢查表

| 段落 | 旁白字數（約） | 念完時間（約） |
|---|---|---|
| 架構 | 90 | 15–17 秒 |
| 建置 / .so | 75 | 13–15 秒 |
| alice / bob | 160 | 28–30 秒 |
| stress | 75 | 13–15 秒 |
| Wireshark | 80 | 14–16 秒 |
| 強健性 | 105 | 19–20 秒 |
| log | 60 | 10–12 秒 |

如果超過 2 分鐘，優先刪掉：Wireshark 的 rst.pcap、alice/bob 最後離開的畫面、架構段最後一句。
