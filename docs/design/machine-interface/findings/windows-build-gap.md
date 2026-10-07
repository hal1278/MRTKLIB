# `mrtk` の Windows native build を阻むもの

- **対象:** `h-shiono/MRTKLIB` `develop` `8dc1fc6`
- **調査日:** 2026-10-07
- **方法:** code reading と grep. Windows 向けの compile は行っていない (この環境に MinGW cross compiler も MSVC もない). 呼び出し箇所の数は comment 行を除いた grep による概数 (±1–2) である.
- ✔ を付けた項目は本文書の作成時に原文と照合した.

## 0. upstream の方針

- ✔ upstream は Windows 対応を意図的に外している.
  - `README.md:21`: "**POSIX & C11 Pure:** Purged all Win32 API and legacy `#ifdef` macros."
  - `docs/index.md:50`: "**POSIX & C11 Pure** — No Win32 API"
  - `docs/releases/release-notes-v0.5.7.md:36-39`: すべての `#ifdef WIN32` block と `winsock2.h`, `WSAStartup` などを削除し, POSIX の経路だけを残した.
- Windows 向けの分岐は `src/stream/ntrip_chunk.h:19-25` (`strncasecmp` の置き換え) と vendored の `src/core/tomlc99/toml.h:28-30` だけである.

**Inference:** MRTKLIB に Win32 実装を再導入して Windows 対応する場合は, upstream の明示的な方針の変更を伴う. POSIX 互換層 (Cygwin) を使う経路は既存方針に整合する可能性がある. どちらの経路でも, 技術的な作業より先に upstream maintainer との合意が必要である ([unresolved.md](../unresolved.md) U-02).

## 1. POSIX 依存の分布

### 全 translation unit に及ぶ header

- ✔ `include/mrtklib/mrtk_foundation.h:327` が `<pthread.h>` を include する. `mrtk_time.h` 経由でほぼすべての library source と app に届く.
- ✔ `include/mrtklib/rtklib.h:21` が `<pthread.h>`, `:27` が `<sys/select.h>` を include する.

### Socket — `src/stream/mrtk_stream.c` (約 61 箇所)

- ✔ upstream の Winsock 対応の名残が POSIX 側だけ残っている: `#define socket_t int`, `#define closesocket close` (`:103-105`), 中身が空の `strinitcom()` (`:3038`, upstream では `WSAStartup` を呼んでいた).
- 非 blocking 化は `fcntl(O_NONBLOCK)` (`:984-985`), timeout は `struct timeval` の `SO_RCVTIMEO` / `SO_SNDTIMEO` (`:943-947`). Winsock では `ioctlsocket` と DWORD (ms) が必要である.
- `apps/rtkrcv/rtkrcv.c` の telnet 待ち受けは `int` の socket を直接使う (約 14 箇所, `:1934-1996`).
- ✔ `apps/rtkrcv/vt.c` は telnet socket に対して `read()` / `write()` を直接使う (10 箇所). Winsock の socket には CRT の `read` / `write` を使えない.

### Serial — termios

- `mrtk_stream.c` の `openserial` (`:250-385`): `/dev/%s` の path, `speed_t` の baud rate 表, `tcgetattr` / `tcsetattr` / `tcflush`. 抽象化はない.
- `vt.c` の local console は `/dev/tty` と termios を使う.

### Thread

- lock だけは抽象化されている (`mrtk_foundation.h:327-333` の `rtk_lock_t` など). これらの型は public struct に含まれる (`mrtk_stream.h`, `mrtk_rtksvr.h`, `mrtk_madoca_local_comb.h`).
- thread の生成と join は `pthread_*` を直接呼ぶ (14 箇所). ✔ `rtksvrstart` は stack size を 8 MB に指定する (`mrtk_rtksvr.c:1380-1385`). Windows の既定 stack は 1 MB である.
- condition variable, `<threads.h>`, atomics は使っていない.

### Signal (app のみ, 約 22 箇所)

- `rtkrcv.c`: crash handler (`sigaction`, `sigprocmask`, `kill`), SIGINT / SIGTERM / SIGUSR2, SIGHUP / SIGPIPE の無視.
- `str2str.c`, `cssr2rtcm3.c`: SIGTERM / SIGINT / SIGHUP / SIGPIPE.

### Process

- `src/core/mrtk_sys.c:83-87` の `execcmd()` が `system()` の唯一の窓口である. 圧縮 file の展開 (gzip, tar, crx2rnx) で使われ, post と convert からも到達する.
- `rtkrcv.c`: `system()` (startcmd / stopcmd), `popen` (`!command`).

### 時刻

- `timeget`, `tickget`, `sleepms` に抽象化されている (`src/core/mrtk_time.c`). 直接の迂回は `apps/dumpcssr/cssr_parse.c` だけである.
- socket 以外への `select()` が 2 箇所ある: `vt.c:336` (tty), `mrtk_stream.c:700-706` (stdin). Winsock の `select` は socket 専用である.

### Filesystem

- `mrtk_sys.c`: `<dirent.h>`, ✔ 2 引数の `mkdir(dir, 0777)` (`:175`), `strtok_r`.
- `FILEPATHSEP '/'` が 4 箇所で定義されている. `'/'` の直書きが 3 箇所.
- `strcasecmp` (`l6extract.c`, `cssr2rtcm3.c`).

## 2. Build system

- ✔ `CMakeLists.txt:46`: `add_compile_options(-Wall -O3 -ansi -pedantic -g)` をすべての compiler に適用する. MSVC はこれらを解釈しない.
- ✔ `:145` で `m`, ✔ `:227` で `pthread` を無条件に link する. MSVC にはどちらもない (MinGW にはある).
- `ws2_32` などの Windows library の link, `if(WIN32)` / MSVC / MinGW の分岐, Windows 用 preset はない.
- ctest の 22 件は `bash` で実行する. Windows で test を回すには bash が要る.

## 3. Subcommand ごとの依存

| Subcommand | 必要な POSIX 依存 |
|---|---|
| `run` | stream layer 全体 (socket, serial, NTRIP, ftp thread), rtksvr thread, signal, `system` / `popen`, console (termios, raw fd 上の telnet) |
| `relay` | streamsvr, stream layer, signal |
| `cssr2rtcm3` | stream layer, signal, `strcasecmp` |
| `post`, `convert`, `ssr2obs`, `ssr2osr`, `bias`, `dump`, `l6extract` | header 経由の `pthread.h` / `sys/select.h`, `mrtk_sys.c`, `mrtk_time.c`, CMake の flag と library のみ. socket, serial, signal, thread は使わない |

link 上の結合:

- ✔ `formatstrs[]` は `mrtk_stream.c:80` で定義され, `src/data/mrtk_convrnx.c:323` と `apps/convbin/convbin.c:300` が使う. `convert` だけを build しても `mrtk_stream.c` が必要になる.
- `apps/mrtk/mrtk_main.c` は全 subcommand を静的に dispatch する. post / convert だけを先に Windows 対応するには, run / relay / cssr2rtcm3 を除外する CMake option が要る.

## 4. 移植の参考になる code

- この repository の `upstream/` は空 (`.gitkeep` のみ) である.
- 手元の別 checkout に WIN32 実装が残っている.
  - MALIB `src/stream.c` (serial の `CreateFile`, ftp の `CreateThread`, `strinitcom` の `WSAStartup`), `src/rtkcmn.c` (`timeGetTime`, `Sleep`, `CreateProcess`, `FindFirstFile`, `CreateDirectory`), `src/rtksvr.c` (`CreateThread`)
  - CLASLIB, RTKLIB 2.4.3 の `stream.c` / `rtkcmn.c` / `rtksvr.c`
- upstream RTKLIB でも `rtkrcv` (console app) は Windows に移植されていなかった (MALIB `app/consapp/rtkrcv/rtkrcv.c` に WIN32 分岐はない). RTKLIB の Windows 版の real-time 測位は GUI app (RTKNAVI) が担っていた.

## Inference (未検証)

1. **CMake を先に直す.** GNU 専用 flag を compiler 判定で囲み, `m` と `pthread` を条件付きにし, Windows では `ws2_32` を link する. 最初の対象は MinGW-w64 (MSYS2 UCRT64) が安い. winpthreads, `dirent.h`, `strcasecmp` を持つためである. それでも socket, termios, signal, 2 引数 `mkdir` は直す必要がある.
2. **Phase A: post / convert 系.** 約 6–8 file, 30 箇所程度. platform header を 1 つ設けて pthread, `FILEPATHSEP`, `strcasecmp` / `strtok_r` を引き受け, `rtklib.h` から `sys/select.h` を外し, `mrtk_sys.c` / `mrtk_time.c` の Windows 版を MALIB から移す. `formatstrs` を `mrtk_stream.c` から移し, run / relay / cssr2rtcm3 を除外する option を設ける.
3. **Phase B: relay と cssr2rtcm3.** stream layer の Winsock 化 (`strinitcom` での `WSAStartup`, `SOCKET`, `ioctlsocket`, DWORD timeout), serial の `CreateFile` / DCB 化, stack size を取る thread 生成 macro, stdin の `select` の置き換え, `SetConsoleCtrlHandler`.
4. **Phase C: run.** `vt.c` の書き直しが最大の作業である. machine interface (RPC) を持つ `mrtk run` が GUI から操作されるなら, Windows では interactive console を持たない `mrtk run` (起動時 start と RPC のみ) から始められる可能性がある.
5. **MSVC を選ぶ場合.** vcpkg の pthreads4w で thread の変更を避けられるが, socket, termios, console の移植は同じく必要である.
