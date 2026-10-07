# `rtkrcv` console の構造と command 一覧

- **対象:** `h-shiono/MRTKLIB` `develop` `8dc1fc6`, `apps/rtkrcv/rtkrcv.c` (以下 `:行` はこの file)
- **調査日:** 2026-10-07
- **方法:** code reading. 実行確認したものは明記する.

## 構造

- **状態はすべて file-scope static である** (`:119-155`). `rtksvr_t* svr`, monitor stream, 終了 flag `intflg`, console 表示設定, stream の type / path / format, server 設定 (`svrcycle` など), 測位 option (`prcopt`, `solopt`, `filopt`), 変更 flag (`modflgr`, `modflgs`).
- **console は thread ごとに並行して command を実行する.** local console 1 個, または telnet console 最大 `MAXCON = 32` 個 (`:89`). 各 console は `con_thread` (`:1775`) を pthread で実行する. 上記の static 変数を守る lock はない. `rtksvrlock` が守るのは `svr` 内の field だけである.
- **telnet の待ち受けは全 interface である.** `open_sock` は `sockaddr_in` を 0 で初期化して bind する (`:1932-1954`). 受け付けは main thread が 100 ms ごとに行う (`accept_sock` `:1956`, main loop `:2269-2273`).
- **login** は password が空でなく, かつ telnet のときだけ要求する (`:415-440`). `-w` の値は configuration file の `console-passwd` に上書きされる ([rtkrcv-toml-save.md](rtkrcv-toml-save.md)).
- **process の終了経路:** SIGINT / SIGTERM / SIGUSR2 (`sigshut` `:307`), または `shutdown` command (1 秒 sleep 後に `intflg = 1`). main loop を抜けると `stopsvr(NULL)` (`:2275`), console を閉じ, navigation data を `rtkrcv.nav` に保存する (`:2294`).
- **`-s` 指定時**は起動直後に `startsvr(NULL)` を呼ぶ. 失敗しても process は起動したままになる. 失敗理由の一部は stderr に出るが, `vt_printf(vt, ...)` で出す message は `vt == NULL` のとき黙って捨てられる (`vt_putchar` `vt.c:415-418`). 例: command file が無い場合 (`:562`, `:567`).

## Command 一覧

列の意味: **対話** = 実行中に console からの入力を待つことがある. **破壊的読み出し** = 読んだ data を共有 buffer から消す.

| Command | 処理 | 状態への作用 | 対話 | 破壊的読み出し |
|---|---|---|---|---|
| `start` | `startsvr` (`:508-674`): 補正源の検証, obsdef の再設定, 受信機 command file の読み込み, file 出力 stream の上書き確認, antenna / DCB / geoid の読み込み, stream option 設定, `misc-startcmd` の `system()` 実行, `rtksvrstart`, CLAS 補助 file の読み込み | server 起動. 変更 flag を clear | あり (出力 file が既存なら y/n, `confwrite` `:387`) | — |
| `stop` | `stopsvr` (`:676-710`): 停止 command 送信, `rtksvrstop`, `misc-stopcmd` の実行, geoid を閉じる | server 停止. 停止中なら何もしないが `rtk server stop` は表示する (`:1374`) | — | — |
| `restart` | `stopsvr` → `startsvr` | 同上 | `start` と同じ | — |
| `solution [cycle]` | `svr->solbuf[0..nsol)` を表示し `nsol = 0` | — | — | **あり** (`:1397-1405`) |
| `status [cycle]` | `prstatus` (`:830-1023`): lock 下で copy し表示 | — | — | — |
| `satellite [-n] [-sys] [cycle]` | `prsatellite`: `rtk_t` 全体を lock 下で copy. 衛星ごとに Az/El, valid, fix, 残差, slip, lock, reject. **SNR を含まない** | — | — | — |
| `observ [-n] [-sys] [cycle]` | `probserv`: rover と base の最新観測 (SNR, P, L, D, LLI) | — | — | — |
| `navidata [-sys] [cycle]` | `prnavidata` | — | — | — |
| `stream [cycle]` | `prstream` (`:1271-1306`): 8 stream と monitor の type, format, state (E/C/-), byte 数, bps, path, message | — | — | — |
| `ssr [cycle]` | `prssr`: SSR 補正. 関数内 `static` buffer を使う (`:1309`) | — | — | — |
| `error` | `rtk.errbuf` を表示し `neb = 0` を繰り返す. break まで終わらない | — | — | **あり** (`:1520-1532`, `prerror` `:1257-1269`) |
| `option [filter]` | rcvopts と sysopts の現在値. 変更済みは `*` | — | — | — |
| `set opt [val]` | `str2opt` で static 変数を更新. `console-*` 以外は変更 flag を立て, restart まで反映しない (`:1618-1663`) | 編集中の設定 | あり (値省略時) | — |
| `load [file]` | sysopts を既定値に戻してから file を読む. restart まで反映しない (`:1665-1687`) | 編集中の設定 | — | — |
| `save [file]` | rcvopts と sysopts を保存. TOML では sysopts が欠落する | file | あり (既存 file の上書き確認) | — |
| `log [file\|off]` | この console の入出力を file に記録 | console 単位 | あり (上書き確認) | — |
| `help` / `?` | 固定 text | — | — | — |
| `exit` | telnet console の logout | console 単位 | — | — |
| `shutdown` | process 終了 | process | — | — |
| `!command` | `popen` で shell command を実行 (`cmd_exec` `:1754`) | 任意 | — | — |

command 名は前方一致で判定し, 複数一致したときは表の後ろの方が選ばれる (`:1817-1821`). `shutdown` だけは完全一致を要求する.

## Command layer 抽出時の論点

以下は code から読み取った事実に基づく. 判断は [rtkrcv-control.md](../rtkrcv-control.md) で行う.

1. **対話的な確認が制御処理の中にある.** `start` / `restart` の出力 file 上書き確認, `save` / `log` の上書き確認, `set` の値入力. machine interface では対話できないため, 方針を引数か設定で受け取る必要がある.
2. **破壊的読み出しがある.** `solution` は `nsol = 0`, `error` は `neb = 0` とする. 複数の consumer (複数の telnet console, telnet と RPC) が同じ data を奪い合う. `solbuf` は満杯 (256) になると以後の solution を捨てる ([rtksvr-runtime.md](rtksvr-runtime.md)).
3. **設定は 2 段階である.** `set` / `load` は static 変数 (編集中の設定) を変えるだけで, `startsvr` の時点で `rtksvrstart` に渡されたものが稼働中の設定になる. `set` で変えた option には `modflgr` / `modflgs` の印が付く (`:1659`). `load` は印を付けず (`:1665-1687`), start は起動の成否が分かる前に印を消す (`:601-606`). (2026-10-08 訂正: 以前は「どの option を変えたかは `modflgr` / `modflgs` に残る」と書いていた.)
4. **並行実行の保護がない.** ある console の `set` / `load` と別 console の `start` が同時に static 変数に触れうる. `prssr` の `static` buffer も同時実行で壊れうる.
5. **server state は 2 値しか表示しない.** `prstatus` は `svr->state` を `stop` / `run` に写すだけである (`:832`, `:921`).
6. **人間向けの単位・書式変換が表示関数の中にある.** 時刻系 (`console-timetype`), 座標表示 (`console-soltype`) は console 設定に依存する. machine interface では表示設定と独立した値を返す必要がある.
7. **`stream` の path 表示は stream path をそのまま 24 文字まで出す.** NTRIP の path は user / password を含みうる. machine interface で stream path や configuration を返す場合は credential の扱いを決める必要がある.

## 付随して観測した事実

- `navidata` の cycle 引数は `args[i]` ではなく `args[1]` を読む (`:1503`).
