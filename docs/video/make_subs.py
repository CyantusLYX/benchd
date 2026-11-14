#!/usr/bin/env python3
"""make_subs.py — build subs.srt for the demo video from per-segment cue text.

The video has 7 segments (see docs/video_script.md). Give the real start time of each segment
plus the end of the video (8 numbers, seconds); cues inside a segment are spread over it in
proportion to their length.

    python3 docs/video/make_subs.py                         # planned timeline 0,15,30,...,120
    python3 docs/video/make_subs.py --bounds 0,14,31,63,80,97,121,133 -o docs/video/subs.srt
"""
import argparse

SEGMENTS = [
    # 0:00 architecture
    [
        "benchd：讓多人透過 TCP 共用同一個序列埠",
        "master 只負責 accept\n每來一個 client 就 fork 一個 child",
        "序列埠只由 reader 行程獨佔讀寫",
        "資料放進共享記憶體的 ring buffer\n每個 child 用自己的位置去讀",
    ],
    # 0:15 build + shared library
    [
        "用 Makefile 建置\nserver、client 和共用函式庫一次完成",
        "ldd：benchd 和 bench\n都連到同一個 libbench.so.1",
        "封包編解碼、CRC32、log 都在這個 .so 裡\n兩邊共用",
    ],
    # 0:30 alice / bob
    [
        "開兩個 client：左邊 alice，右邊 bob",
        "bob 比較晚連上，先收到歷史紀錄回放\n所以畫面和 alice 一樣",
        "alice 按 Ctrl-] t 取得控制權\n狀態列變成 CTRL",
        "alice 輸入 version\n板子的回應兩邊同時看到",
        "bob 打字會被擋下（read-only）\n同一時間只有一人能控制，指令不會混在一起",
        "Ctrl-] i 向 server 要系統資訊\n主機、序列埠設定、連線數、控制權在誰手上",
    ],
    # 1:00 stress
    [
        "make stress：同時開 20 個 client",
        "pstree：master 底下 20 個 benchd-child + 1 個 reader\nss：20 條連線同時存在",
        "20 份收到的資料 sha256 完全相同\n→ 每個人收到的廣播都一致",
    ],
    # 1:15 wireshark
    [
        "用 tcpdump 在 loopback 上擷取封包",
        "自己寫的 Lua dissector\n讓 Wireshark 解析自訂的二進位協定",
        "Magic、版本、型別、長度、CRC32 驗證\n這個 INFO 封包就是剛才看到的系統資訊",
    ],
    # 1:30 robustness r2
    [
        "強健性：server 滿載時\n再有 40 個 client 連上後立刻送 RST",
        "unsafe（關掉保護）\nmaster 被 SIGPIPE 殺掉，整個服務消失",
        "safe（預設）\n忽略 SIGPIPE、只記錄錯誤，繼續服務",
        "這種有／無保護的對照，一共做了 12 組",
    ],
    # 1:50 log levels
    [
        "執行期：kill -USR1\n不用重啟就從 INFO 切到 DEBUG",
        "編譯期：由 LOG_COMPILE_LEVEL 決定\nrelease 版裡的 TRACE 字串數 = 0",
        "謝謝觀看",
    ],
]

PLANNED = [0, 15, 30, 60, 75, 90, 110, 120]


def ts(t):
    ms = int(round(t * 1000))
    h, ms = divmod(ms, 3600_000)
    m, ms = divmod(ms, 60_000)
    s, ms = divmod(ms, 1000)
    return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bounds", default=",".join(map(str, PLANNED)),
                    help="8 comma-separated times in seconds: start of each segment + video end")
    ap.add_argument("-o", "--out", default="docs/video/subs.srt")
    a = ap.parse_args()
    b = [float(x) for x in a.bounds.split(",")]
    if len(b) != len(SEGMENTS) + 1 or any(y <= x for x, y in zip(b, b[1:])):
        ap.error(f"--bounds needs {len(SEGMENTS) + 1} increasing numbers")

    out, n = [], 0
    for seg, start, end in zip(SEGMENTS, b, b[1:]):
        weights = [len(c.replace("\n", "")) + 6 for c in seg]   # +6: short cues still get time
        t = start
        for cue, w in zip(seg, weights):
            dur = (end - start) * w / sum(weights)
            n += 1
            out.append(f"{n}\n{ts(t)} --> {ts(t + dur - 0.05)}\n{cue}\n")
            t += dur
    with open(a.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print(f"wrote {a.out}: {n} cues, {b[0]:.1f}s – {b[-1]:.1f}s")


if __name__ == "__main__":
    main()
