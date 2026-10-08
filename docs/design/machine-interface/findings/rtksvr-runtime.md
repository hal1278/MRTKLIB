# `rtksvr` の runtime state, lock, event 発生源

- **対象:** `h-shiono/MRTKLIB` `develop` `8dc1fc6`
- **調査日:** 2026-10-07
- **略号:** RS = `src/stream/mrtk_rtksvr.c`, SS = `src/stream/mrtk_stream.c`, RH = `include/mrtklib/mrtk_rtksvr.h`, SH = `include/mrtklib/mrtk_stream.h`
- **方法:** code reading. ✔ を付けた項目は本文書の作成時に原文と照合した.

## 1. Server lifecycle

- `svr->state` は `int` で, 値は `0:stop` / `1:running` の 2 つだけである (RH:53). starting / stopping に当たる状態はない.
- ✔ `state = 1` を設定するのは `rtksvrstart` ではなく server thread である (RS:842). `rtksvrstart` の二重起動 guard は `if (svr->state)` (RS:1181) なので, thread 生成から RS:842 までの間は二重起動を防げない.
- ✔ `rtksvrstop` (RS:1405-1424) は lock 下で停止 command を送り, lock なしで `state = 0` とし (RS:1420), `pthread_join` で終了を待つ (RS:1423). 停止は同期的で, 実行中の cycle (`rtkpos` を含む) の終了まで待つ. `state` を確認せず `svr->thread` も clear しないため, 未起動や二重の stop は無効な thread を join する. `rtkrcv` の `stopsvr` は `if (!svr->state) return;` で保護している (`apps/rtkrcv/rtkrcv.c:682-684`).
- `rtksvrstart` の失敗は呼び出し側の `errmsg` に長さ無制限の `sprintf` で書かれる. 内容は `rtk server malloc error` (RS:1213, 1245), `str%d open error path=%s` (RS:1339), `thread create error` (RS:1390). stream open 失敗時, 失敗した stream の詳細は `stream[i].msg` に残る (SS:3220). 失敗経路で確保済み buffer と CLAS / HAS context は解放されない.
- `rtksvrstart` は block しうる. 受信機 start command ごとに 100 ms sleep (RS:1371), command script の `!WAIT` は最大 3 s, `!BRATE` は 500 ms (SS:3777-3790). serial / file の open は同期的である.
- ✔ server thread は自分では止まらない. loop 条件は `svr->state` だけで (RS:848), thread 内で `state` を 0 にする箇所はない. file 入力が EOF に達しても stream msg が `end` になるだけで (SS:734, 748), thread は回り続ける. thread 自身が終了するのは起動直後の `malloc` 失敗だけで, そのとき `state` は 1 にならず通知もない (RS:839-841).
- lifecycle の遷移を通知する callback は存在しない.

## 2. Server thread の 1 cycle (RS:825-963)

1. 3 入力それぞれで `strread`, log stream (5-7) への複製, peek buffer への追記 (lock 下, RS:863-867).
2. `decodefile` / `decoderaw` (RS:869-877).
3. base の単独測位平均 (RS:879-890).
4. ✔ rover epoch ごとに rover / base 観測を結合し, lock 下で `rtkpos` (RS:900-908).
5. `sol.stat != SOLQ_NONE` なら `timeset` (RS:913) と `writesol` (RS:916).
6. 解がない間 (`sol.stat == SOLQ_NONE`) は 1 Hz で NONE 状態の solution を出す (RS:924-927). 周期 command, NMEA request. したがって `writesol` (と `solbuf`) に入るのは, 解がある epoch では epoch ごとに 1 件, 解がない間は最大 1 Hz であり, 解のない epoch ごとに 1 件ではない. (2026-10-08 訂正: 以前は「fix がない間」と書いていた. float などの解は 5. で epoch ごとに出る.)
7. `svr->cputime` に cycle 所要時間を記録し (RS:938), `cycle - cputime` だけ sleep (RS:942). 既定 cycle は 10 ms (`apps/rtkrcv/rtkrcv.c:133`).

### Solution の行き先 (`writesol`, RS:80-120)

- sol1 / sol2 を `stream[3+i]` に書き, 同じ byte 列を lock 下で `sbuf[i]` に追記する.
- ✔ `svr->moni` があれば既定の LLH 書式で solution text を書く (RS:110-113). monitor port が運ぶのは solution text と `rtksvrmark` の行だけで, status / satellite / stream の情報はない.
- `outsols` は NONE の解に 0 byte を返す (`src/pos/mrtk_sol.c:1792-1793`). このため NONE は出力 stream と monitor port には書かれず, `solbuf` にだけ入る. NMEA の GSA / GSV は `outsolexs` が状態によらず書く (`src/pos/mrtk_sol.c:1859-1862`). (2026-10-08 追記)
- `out-outsingle` が off のとき, `rtkpos` は SPP の解を `rtk->sol` に入れてから状態を NONE にする (`src/pos/mrtk_rtkpos.c:2756`, `:2779-2780`). その後の処理が失敗したときに座標が SPP の値のまま残るかは engine ごとに未確認である. (2026-10-08 追記)
- telnet の `solution` は状態 0 の解を表示しない (`apps/rtkrcv/rtkrcv.c:738-739`). (2026-10-08 追記)
- ✔ `solbuf[nsol++] = rtk.sol` を lock 下で行うが, `nsol < MAXSOLBUF` (256) のときだけである (RS:115-118). ring buffer ではなく, 満杯になると consumer が `nsol` を 0 に戻すまで新しい solution は捨てられる. 唯一の consumer は `rtkrcv` の `solution` command である.
- `rtksvr_t` に関数 pointer はなく (RH:52-97), epoch / solution ごとの hook は存在しない. ✔ `mrtk_ctx_t.cb_showmsg` (`include/mrtklib/mrtk_context.h:89`) は `NULL` 初期化以外に使われていない.

## 3. Lock

- `rtk_lock_t` は属性なしで作る `pthread_mutex_t` で再帰不可である (`include/mrtklib/mrtk_foundation.h:329-332`). svr ごとに 1 つ.
- `svr->lock` を保持する主な区間:
  - ✔ `rtkpos` 全体 (RS:900-908). #299 の指摘どおり, rover epoch ごとに solve 全体の間保持する.
  - `decoderaw` 全体 (RS:373-646). その cycle に読んだ全 byte の decode (CLAS / HAS / L6D を含む).
  - `decodefile` の一部, `rtkoutstat`, `saveoutbuf`, `solbuf` 追記, `rtksvrstop` の command 送信, `rtksvropenstr` / `rtksvrclosestr` / `rtksvrostat` / `rtksvrsstat` / `rtksvrmark`.
- 保持時間の計測値は tree 内にない. 目安は cycle 全体の `svr->cputime` だけである.
- thread が lock なしで書く field: base 平均 (`rtk.opt.rb`, `nave`, `rb_ave`), `prcout`, `cputime`, `nsol` の容量確認など.
- ✔ `strstat` / `strsum` は stream 自身の lock だけを使う (SS:3458) が, `rtksvrsstat` はそれを `svr->lock` で囲む (RS:1533-1547). このため stream 状態の取得も `rtkpos` / `decoderaw` の間待たされる.
- ✔ `rtksvrmark` は lock を取った後に `saveoutbuf` を呼び, そこで同じ lock を再度取る (RS:1566, RS:71). 再帰不可 mutex なので deadlock する. `apps/` に呼び出し元はない.

## 4. Stream state

- `stream_t` (SH:69-83): `type`, `mode`, `state` (`-1:error, 0:close, 1:open`), `inb` / `inr`, `outb` / `outr`, `path[1024]`, `msg[1024]` など. `state` は open / close でしか変わらず, 接続状態を表さない.
- ✔ `int strstat(stream_t*, char* msg)` (SS:3453-3511) は `-1:error, 0:close, 1:wait, 2:connect, 3:active` を返す. 3 は「2 かつ 200 ms (`TINTACT`) 以内に data あり」である (SS:3506-3507). file では `statefile` が open 中ずっと 2 を返し (SS:656), 読み書きの直後は `strstat` が 3 にする. (2026-10-08 訂正: 以前は「file は open 中ずっと 2 である」とし, 行番号を 3504-3506 としていた.)
- 状態変化は `strread` / `strwrite` の中でしか起きない. 出力の無い stream は次の書き込みまで切断に気付かない.
- `strsum` (SS:3584-3601) は byte 数と bps を `int` で返す. bps は read / write 時に 1000 ms ごとにしか更新されない.
  - 同じ数を使う表示 (2026-10-08 追記): RTKLIB 2.4.3 b34 の STRSVR は `strsvrstat` (`src/streamsvr.c:748-765`) で得た `int` を `uint32_t` に戻して `%u` で書く (`app/winapp/strsvr/svrmain.cpp:57-67`). str2str (b34 `app/consapp/str2str/str2str.c:333-334`) と MRTKLIB の `mrtk relay` (`apps/str2str/str2str.c:400`) は `%10d` で書く. **Inference:** `mrtk relay` は 2 GiB を超えると負の byte 数を表示する. 実行での確認はしていない.
- `msg` は最後に書かれた自由文である (例: `connecting...`, `connect error (%d)`, `timeout`, `disconnected`, `%d clients`, `end`, NTRIP の応答 text). code はない.
- ✔ serial の実装 (`mrtk_stream.c`) について:
  - `openserial` は port 名の前に `/dev/` を付ける (SS:327). Cygwin では COM3 は `ttyS2` と指定することになる.
  - `tcgetattr` / `tcsetattr` / `tcflush` の戻り値を確認しない (SS:343-360).
  - `readserial` は `read` の error を 0 として返す (SS:393-395).
  - `writeserial` は書き込み byte 数 `ns` を更新せず常に 0 を返す ([h-shiono/MRTKLIB#343](https://github.com/h-shiono/MRTKLIB/issues/343) として報告, 2026-10-08) (SS:405-418). このため serial の送信 byte 数 (`outb`) は増えず (SS:3434-3436), 送信統計から送信の成否を判断できない.
  - 公開 header は `strwrite` の戻り値を `status (0:error,1:ok)` と説明するが (SH:181), 実装は書き込んだ byte 数 `ns` を返す (SS:3445). (2026-10-08 追記)
- ✔ `strread` は `stream->mode` と `stream->port` の確認を `strlock` の前に行う (SS:3326, 3330). `strwrite` も同様 (SS:3396, 3400). この `port` の読み出しは, 並行する `strclose` による `port` の書き込みとの data race である. 一方 `strclose` は同じ stream の lock の下で port を解放し, `type = 0`, `port = NULL` とする (SS:3231-3283). `strread` / `strwrite` は lock を取った後に `type` で分岐し, 0 なら `default` で lock を解いて 0 を返す (SS:3332, 3364-3366, 3402, 3430-3432). したがってこの順序では解放済み領域には触れず, 解放済み領域への参照は確認できていない. `stropen` は stream の lock を取らずに `type`, `mode`, `port` を書き換える (SS:3168-3221) ので, open と read / write の競合は別に残る. (2026-10-08 訂正: 以前は「並行する `strclose` が port を解放すると解放済み領域に触れうる」と書いていた.)

## 5. 既存の状態取得関数

- `void rtksvrsstat(rtksvr_t*, int* sstat, char* msg)` (RS:1533-1547): 8 stream の `strstat` と, 空でない message を `(%d) %s ` で連結した文字列. monitor は含まない.
- `int rtksvrostat(svr, rcv, time, sat, az, el, snr, vsat)` (RS:1497-1525): 停止中は 0. 稼働中は lock 下で `obs[rcv][0]` から衛星番号, Az/El, SNR (整数 dBHz), 使用 flag を返す. 公開 header は SNR の単位を `0.001 dBHz` と説明するが (RH:187), 実装は観測値 (`0.001 dBHz` 単位の `uint16_t`) に `SNR_UNIT` (0.001) を掛けて丸めた整数 dBHz を返す (RS:1515) (2026-10-08 追記). **衛星表示と SNR を 1 回で得られる既存の関数である.**
- 入力ごとの message 数 `nmsg[3][12]` (0:obs, 1:eph, 2:ion, 3:sbas, 4:antpos, 5:dgps, 6:geph, 7:ssr, 9:decode error, 10:stat, 11:lcl).

## 6. Process-global / static な状態

- ✔ `decoderaw` の関数内 static (RS:367-369): `init_flg` (`init_mcssr` を process で 1 回だけ実行し, restart では再実行しない), STAT parser の `buff[3][4096]` と `nbyte[3]`.
- `timeoffset_` (`src/core/mrtk_time.c:74`) は server thread と timetag 付き file 再生の双方が変更する.
- `mrtk_stream.c` の global 設定 (SS:239-247) は `strset*` (SS:3615-3679) が lock なしで書き換える.
- `solbuf`, `sbuf`, `pbuf` は単一 consumer 前提の破壊的 queue である.

## 7. Log

- server 側の診断出力は `trace()` / `tracet()` だけで, trace level に応じて trace file に書く (`src/core/mrtk_trace.c:78-105`). RS と SS に `fprintf(stderr)` はない.
- `mrtk_ctx_t` の `last_err_code`, `last_err_msg`, `cb_showmsg` (`mrtk_context.h:80-89`) は初期化 (`src/core/mrtk_context.c:39-45`) 以外で設定も呼び出しもされていない. (2026-10-08 訂正: 以前は「設定も呼び出しもされていない」と書いていた.)

## Inference (未検証)

- command layer は start / stop / load と stream の open / close を直列化する 1 つの mutex を持ち, 独自の lifecycle state (`stopped`, `starting`, `running`, `stopping`) を持つ必要がある. `svr->state` ではこれを表現できない.
- 「server が止まった」という event は thread からは来ないので, command layer が自分で生成する必要がある. file 入力の終了は stream msg `end` などから推定するしかない.
- stream event は `strstat` / `strsum` を stream ごとに polling して差分を取るのが現実的である. `rtksvrsstat` 経由にすると `rtkpos` の間待たされる.
- solution event には `writesol` への hook 追加, または command layer が `solbuf` の唯一の consumer になって配る方式が必要である. telnet と RPC が両方 `solbuf` を読んではならない.
- 構造化された `log` topic には `trace` への sink hook 追加 (または `cb_showmsg` の配線) が必要である.
- stream の実行時 open / close を machine interface に出す前に, `strread` / `strwrite` / `stropen` の race (§4 の訂正を参照), stop の二重 join, `rtksvrmark` の deadlock を直すか避ける必要がある.
