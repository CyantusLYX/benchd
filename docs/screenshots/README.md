# 執行截圖索引（對照課堂要求）

全部截圖都是實際執行指令後，把終端機輸出渲染成 PNG（不是手繪或後製）。每張圖第一行的
`112590023:~/benchd$ ...` 就是當時執行的指令。Wireshark 兩張是真的 Wireshark GUI 畫面。

| 課堂要求 | 截圖 | 圖中要指出的重點 |
|---|---|---|
| Makefile / CMake 建置 | `01_build.png` | `make release`、`make debug`，零 warning；`-shared -Wl,-soname`、`-fPIC` |
| 共用 `.so` | `02_shared_lib.png` | `ldd` 兩邊都連到 `libbench.so.1`；`RUNPATH $ORIGIN/../lib`；`.so` 匯出的共用函式 |
| fork()、≥10 個 client 並行（runtime proof） | `03_stress_20_clients.png` | `pstree`：1 個 master 底下 20 個 `benchd-child` + 1 個 `benchd-reader`；`ss` 20 條連線；20 個 client 收到的資料 sha256 全部相同 |
| 封包擷取與分析 | `05_capture_tcpdump.png` | `tcpdump -i lo` 擷取，tshark + `tools/benchd.lua` 解出 HELLO→WELCOME→…→BYE 整個流程 |
| 〃 | `06a_wireshark_info_packet.png` | Wireshark 解出自訂協定：Magic 0x424E、Version、Type、Length、CRC32 valid、INFO 內容 |
| 〃 | `06b_wireshark_fin_vs_rst.png` | 正常離線是 FIN/ACK 四次交握；client 當掉時是 RST（紅色那行） |
| 兩層 debug log：編譯期 | `07_log_compile_time.png` | `LOG_COMPILE_LEVEL=2/4`；release 版執行檔裡 TRACE 字串數 = 0，debug 版 = 1 |
| 兩層 debug log：執行期 | `08_log_runtime.png` | 預設 INFO → `kill -USR1` 動態切到 DEBUG（不用重啟）；`BENCH_LOG=trace` 每個封包都記錄 |
| client 向 server 要系統資訊 | `09a_bench_info.png`、`09b_tui_info_box.png` | `INFO_REQ` / `INFO`：主機、server、序列埠、歷史緩衝、控制權狀態 |
| 其他功能（TUI 主控台） | `10a`–`10f` | 歷史回放、取得控制權後輸入 U-Boot 指令、第二個使用者唯讀、說明、時間戳、hex 檢視 |
| 異常處理（裝置 / server） | `10g`–`10j` | 序列埠拔掉 → DEV LOST → 自動重連；server 重啟 → client 自動重連並標示 "server restarted" |
| 真實硬體（PYNQ 開發板） | `13a`–`13d` | 開發板重開機：U-Boot → kernel → systemd → 登入，全程經 benchd 記錄；`/dev/ttyUSB2` 的系統資訊；斷電時 device lost / is back；取得控制權後在板子上執行指令 |
| 自動化測試 | `11_make_test.png` | `make test` 33 項全部通過 |
| 強健性：有/無對照（至少 3 組） | `12_robust_r01.png` … `12_robust_r12.png` | 左邊紅色 = 關掉機制（`--unsafe`），右邊綠色 = 預設開啟 |

## 強健性 12 組（報告至少放 3 組，建議挑這 4 組）

| 圖 | 沒有機制時 | 有機制時 |
|---|---|---|
| `12_robust_r01.png` zombie | 20 個 `<defunct>` 殭屍行程 | SIGCHLD + waitpid 全部回收 |
| `12_robust_r02.png` sigpipe | master 被 SIGPIPE 殺掉（exit 141），整個服務停止 | 忽略 SIGPIPE，繼續服務 |
| `12_robust_r03.png` validate | 惡意長度欄位造成 stack overflow，child 當掉 | 回 `ERROR BAD_LENGTH` 並正常關閉 |
| `12_robust_r07.png` robust-mutex | 持鎖的行程死掉 → 整個 server 卡死 | `EOWNERDEAD` 偵測並修復，繼續運作 |

其餘：r4 readn（TCP 分段）、r5 timeout（不說話的連線）、r6 控制權釋放、r8 裝置重開、
r9 連線上限、r10 慢速 client 的 ring buffer 覆寫、r11 終端機還原、r12 client 自動重連。

## 2 分鐘影片建議腳本

| 時間 | 畫面 | 旁白重點 |
|---|---|---|
| 0:00–0:15 | `README.md` 架構圖 | 一個 master accept、每個 client 一個 fork 出來的 child、reader 獨佔序列埠、共享記憶體 ring buffer |
| 0:15–0:30 | `make` → `ldd build/bin/benchd build/bin/bench` | Makefile 建置；client/server 共用 `libbench.so` |
| 0:30–1:00 | 兩個終端機並排：`bench localhost --name alice` / `--name bob` | alice `Ctrl-] t` 取得控制、打 `version`；bob 同步看到但不能打字；`Ctrl-] i` 顯示系統資訊 |
| 1:00–1:15 | `make stress` | 20 個 client 並行，pstree 看到 20 個 child，資料完全一致 |
| 1:15–1:30 | `make capture` → 開 Wireshark | 自訂二進位協定，Lua dissector 解析、CRC 驗證 |
| 1:30–1:50 | `scripts/demo_r2.sh unsafe` 然後 `safe` | 同一個攻擊，沒保護 server 直接死掉，有保護繼續服務 |
| 1:50–2:00 | `kill -USR1 <pid>` + log | 執行期切換 log 等級；編譯期由 `LOG_COMPILE_LEVEL` 決定 |

錄影前先 `make`，並在另一個終端機啟動：

```bash
build/bin/benchsim -l sim/pynq_boot.log --loop &
build/bin/benchd -d /tmp/benchsim0 &
```
