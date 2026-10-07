# MinGW-w64 (UCRT) cross build の結果

- **対象:** `h-shiono/MRTKLIB` `develop` `8dc1fc6`, target `mrtk`
- **調査日:** 2026-10-07
- **方法:** Linux 上で `nixpkgs#pkgsCross.ucrt64.stdenv.cc` (GCC 16.2.0, binutils 2.46, mingw-w64 14.0.0, 既定 CRT は UCRT) を使い out-of-tree で build した. LAPACK は無効. repository は変更していない (`git status --porcelain --ignored` が前後で同一).
- **詳細:** 全 82 translation unit の表, 分類した error 一覧, 正確な command と log は maintainer-local の `tasks/xbuild/REPORT.md` (gitignore 対象) にある. toolchain file と実行 script も同じ場所にある.
- ✔ を付けた項目は本文書の作成時に原文と照合した.

## Toolchain の前提

- nixpkgs の toolchain には winpthreads (`pthread.h`, `libpthread`) が含まれない. MSYS2 UCRT64 には含まれるため, 同じ toolchain に `pkgsCross.ucrt64.windows.pthreads` を加えた構成 (以下 B) を主な結果とする.
- nixpkgs の toolchain を `nix shell` で使う場合, thread library (mcfgthread) の `-L` を toolchain file で補う必要がある. Nix 固有の事情であり MRTKLIB の問題ではない.
- 生成された実行 file は `api-ms-win-crt-*` を import しており, UCRT を使っている.

## 結果

| 構成 | compile 成功 | 失敗 | 主因 |
|---|---|---|---|
| A: winpthreads なし | 8 | 74 | `pthread.h` (`mrtk_foundation.h:327` 経由 67, `rtklib.h:21` 経由 5) |
| **B: winpthreads あり (MSYS2 UCRT64 相当)** | **68** | **14** | 下の 6 種類の根本原因 |

B の 14 件の失敗は次の 6 種類に集約される.

| 種類 | 箇所 | 影響する file |
|---|---|---|
| header がない: `sys/select.h` | ✔ `include/mrtklib/rtklib.h:27` | 8 file. このうち 7 file は `select` を使っておらず, include を外せば compile できる (scratch での実験) |
| header がない: `arpa/inet.h` ほか socket 系 | `src/stream/mrtk_stream.c:39`, `apps/rtkrcv/rtkrcv.c:62` | stream layer, rtkrcv |
| header がない: `termios.h` | `apps/rtkrcv/vt.h:19` | rtkrcv console |
| 未定義: `SIGPIPE` | ✔ `apps/cssr2rtcm3/cssr2rtcm3.c:1542` | cssr2rtcm3 |
| 型: `gmtime(&tv.tv_sec)`. Windows では `tv_sec` が 32 bit の `long`, `time_t` が 64 bit | ✔ `src/core/mrtk_time.c:295` | ほぼ全 subcommand |
| 型: 2 引数の `mkdir(dir, 0777)`. MinGW の `mkdir` は 1 引数 | ✔ `src/core/mrtk_sys.c:175` | ほぼ全 subcommand |

- pthread は winpthreads があれば compile でも link でも問題にならなかった.
- compiler flag (`-ansi -pedantic -std=c11 -O3`) は受け付けられた. `-rdynamic` と `-lrt` は既に Linux / Unix に限定されている.
- CMake は `ws2_32` を link せず, `pthread` と `m` を無条件に link し, Windows の分岐を持たない.

## Subcommand ごとの距離

各 subcommand の app object から static library の依存閉包を `nm` で求め, B での失敗 file を数えた.

| Subcommand | B で失敗する file |
|---|---|
| `l6extract` | なし (winpthreads なしでも compile できる) |
| `ssr2obs`, `dump` | `mrtk_time.c`, `mrtk_sys.c` |
| `post`, `ssr2osr`, `bias` | 上記 + 各 app file (`rtklib.h:27`) |
| `convert` | 上記 + `mrtk_convrnx.c` + `mrtk_stream.c`. `mrtk_stream.c` から必要なのは data table `formatstrs` だけである |
| `cssr2rtcm3` | 上記 + `SIGPIPE` + stream layer |
| `relay` | 上記 + `mrtk_streamsvr.c` + `str2str.c` (`SIGHUP`, `SIGPIPE`) |
| `run` | 上記 + `rtkrcv.c` (socket, `sigaction`, `SIGUSR2`, `kill`) + `vt.c` (termios) |

`apps/mrtk/mrtk_main.c` が 10 個の subcommand をすべて参照するため, 1 つの `mrtk.exe` にはすべてが要る. 一部だけを先に build するには dispatch から外す仕組みが要る.

scratch 内での実験 (header の shim と stub を使用. 修正案ではない): 失敗する file を除き, `mrtk_time.c` と `mrtk_sys.c` を通し, `formatstrs`, `mrtk_run`, `mrtk_relay`, `mrtk_cssr2rtcm3` を仮の定義で埋めると, 6.5 MB の PE32+ 実行 file が link できた. 依存は UCRT と `libwinpthread-1.dll` だけだった. 実行はしていない.

## Compile は通るが問題になるもの

app は C90 (`-ansi`) で compile されるため, 未宣言の関数や pointer 型の不一致は warning にとどまる.

- `apps/dumpcssr/cssr_parse.c:545` に `mrtk_time.c:295` と同じ `gmtime(&tv.tv_sec)` がある (warning のみ).
- `apps/dumpcssr/cssr_parse.c` の 5 箇所が `uint64_t` を `%lx` で出力する. Windows の `long` は 32 bit である.
- `apps/rtkrcv/rtkrcv.c:115-116` の `popen` / `pclose` の再宣言が MinGW の `#define popen _popen` と衝突する.
- `src/core/mrtk_time.c:567` の `tickget` は Windows では `CLOCK_MONOTONIC_RAW` がなく `gettimeofday` に fallback する.

## `WIN32` macro

✔ project の flag (`-ansi`, `-std=c11`) では MinGW GCC は `_WIN32` / `_WIN64` だけを定義し, `WIN32` は定義しない (`-std=gnu11` では定義する). RTKLIB 由来の `#ifdef WIN32` は, 移植しても黙って無効になる. tree 内唯一の Windows 分岐 `src/stream/ntrip_chunk.h:19` は `_WIN32` を使っており正しい.

## Inference (source の読解による. build / 実行していない)

- `mrtk_stream.c:104-105` は `socket_t` を `int`, `closesocket` を `close` とし, `errsock()` は `errno` を返す. `WSAStartup` がどこにもないため, Windows ではすべての `socket()` が失敗する.
- ✔ `mrtk_stream.c:700-705` は stdin (fd 0) に `select()` を使う. Winsock の `select()` は socket 専用である.
- `mrtk_stream.c:327` は serial を `/dev/<port>` で開く. Windows では `\\.\COMn` を `CreateFile` で開く必要がある.
- `FILEPATHSEP '/'` (`mrtk_foundation.h:333`) のため, `mrtk_sys.c` の path 分解は `\` を区切りと認識しない.
- `rtk_uncompress()` は `gzip`, `tar`, `crx2rnx` を `system()` で呼ぶ. Windows では `cmd.exe` 経由になり, これらの tool が PATH に要る.
- `readfile()` の `long` による file 位置は Windows では 32 bit であり, 2 GiB を超える file を扱えない.
- library は Windows に近い. B で失敗する library の file は 69 中 5 であり, そのうち 2 件 (`mrtk_time.c:295`, `mrtk_sys.c:175`) は 1 箇所ずつの局所的な修正で済みそうに見える.
