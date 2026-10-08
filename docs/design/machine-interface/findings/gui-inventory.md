# 実時間測位 GUI の画面要素

- **対象:** RTKLIB 2.4.3 b34 (`v2.4.3-b34`) の RTKNAVI, MALIB 1.2.0 (`d491842`) と `JAXA-SNU/MALIB` の pre-release `v1.2.0_win_pre`, CLASLIB と MADOCALIB の手元の checkout, `mrtklib-docker-ui` `4fcaf45` の RT tab
- **調査日:** 2026-10-08
- **方法:** code reading (調査用の agent による). ✔ を付けた項目は本文書の作成者が原文と照合した.
- **略号:** NM = RTKLIB b34 `app/winapp/rtknavi/navimain.cpp`, RS = RTKLIB b34 `src/rtksvr.c`, S = docker-ui `src/mrtklib_web_ui/services/mrtk_run_service.py`, RT = docker-ui `frontend/src/components/RealTimeProcessing.tsx`

## 1. 各 GUI の有無

- MALIB の repository には GUI の source がない. ✔ 手元の `d491842` の `app/` は `consapp/rnx2rtkp` と `consapp/rtkrcv` だけである. ✔ GitHub の `JAXA-SNU/MALIB` の tag `v1.2.0_win_pre` (commit `f25dac1`) と branch `feature/1.2.0`, `main`, `draft_issue-9` の tree にも, GUI らしい path (`winapp`, `rtknavi`, `navimain`, `*.dfm`, `*.cbproj`, `*.groupproj`, `*.cpp`) はない (2026-10-08, GitHub API の tree で確認. 途中で切れていない). release に自動で付く source の archive は tag の tree から作られる (一般知識による) ので, これにも含まれない. archive の実物は確かめていない. (2026-10-08 訂正: 以前は「MALIB には RTKNAVI も他の GUI もない」と書いていた.)
- ただし MALIB は, Windows 向けの実行 file を pre-release `v1.2.0_win_pre` (2025-11-16) の asset `malib_win_x64.exe` として配っている (✔). release の説明によれば, RTKLIB b34 の C++ の application に MALIB の PPP engine を組み込み, RAD Studio 12 で build した実験的なものである. ✔ release の説明の screenshot (2026-10-08 に閲覧) は RTKNAVI 型の画面である. title "MALIB-OSS demo feature 1.2.0", GPST の時刻, 解の状態 "PPP-FIX", LLH, σ (N/E/U), Age / Ratio / #Sat, Rover:Base の SNR の plot (凡例 GREJ), 地上の軌跡, stream の message "(1) T+61.2s (3) T+61.2s", Stop / Mark... / Plot / Options... / Exit の button が見える.
- **Inference:** MALIB の library には "PPP-FIX" という独立した解の状態はない (✔ `src/rtklib.h:378-385` は RTKLIB と同じ定義). "PPP-FIX" は, GUI が解の状態 FIX と PPP の mode を組み合わせた表示名と考えられる. GUI の source がないので確かめられない.
- CLASLIB の手元の checkout (`qzss/claslib/claslib` `23cfd36` ほか) は `src` と `util` (rnx2rtkp, ssr2obs, ssr2osr) だけで, GUI はない.
- MADOCALIB (`qzss/madocalib/madocalib` `0089f7d`) の `app/guiapp/madocalib_gui` は rnx2rtkp の後処理の Python/Tk の frontend であり, 実時間の表示はない. 進捗は rnx2rtkp の stderr の行を解析して出す (`adapters/madocalib_runner.py:387-400`).

## 2. RTKLIB 2.4.3 b34 RTKNAVI の main 画面 (monitor 画面を除く)

timer は 100 ms 周期で, plot は 5 周期ごとに描き直す (NM:1398).

| 要素 | 内容 | 読む server の data | 出典 |
|---|---|---|---|
| 時刻 | 選んだ解の時刻を GPST, UTC, LT, 週と秒で表示. button で時刻系を巡回 | `solbuf` の `time` | NM:1474-1497, 858-864 |
| server の表示 | 開始時は橙, 直近 10 秒以内に解があれば緑系, 停止中は地色 | `rtksvr.state`, `rtk.sol.stat`, 無活動の時間 | NM:1377-1397 |
| stream の表示 (8 本) と message | 状態を色で表示. 各 stream の message を連結して表示 | `rtksvrsstat` | NM:1596-1613 |
| 解の状態 | `----/FIX/FLOAT/SBAS/DGPS/SINGLE/PPP` を色付きで表示 | `solbuf` の `stat`, `rtk.opt.mode` | NM:1502-1521 |
| 位置 | LLH (度分秒, 十進の度), ECEF XYZ, ENU の基線, pitch / yaw / 長さ. 標高はジオイド高か楕円体高 | `solbuf` の `rr`, 読み切る時点の `rtk.rb` | NM:1522-1580, ✔ 1372-1373 |
| 標準偏差, age, ratio, 衛星数 | σ (N/E/U または X/Y/Z), `Age`, `Ratio`, `#Sat` | `solbuf` の `qr`, `age`, `ratio`, `ns` | NM:1446-1451, 1581 |
| plot (最大 4 面) | SNR (rover, rover と base), sky plot, sky と SNR, 基線, 地上の軌跡. 残差の plot はない | `rtksvrostat` (rover と base), GUI 内の解の履歴 | ✔ NM:1615-1757 |
| SNR の棒 | 衛星ごと, 周波数ごと. 使っていない衛星は灰色 | `rtksvrostat` | NM:1759-1858 |
| sky plot と GDOP | 方位角と仰角. GDOP は GUI が計算する | `rtksvrostat` | NM:1860-1912, 2111-2132 |
| 解の履歴 | 既定 1000 epoch. 停止中は scroll で過去の epoch を表示. file に保存できる | GUI 内の buffer (`solbuf` を読み切って作る) | NM:1430-1456, 1024-1033, 2225-2272 |

| 操作 | 内容 | 呼ぶ API | 出典 |
|---|---|---|---|
| Start / Stop | server の起動と停止 | `rtksvrstart`, `rtksvrstop` | NM:1113-1356 |
| Mark | mode を STOP / GO / FIX に切り替え (`rtk.opt.mode` を lock なしで書く), 名前と comment を出力に書く | `rtksvrmark` | NM:2890-2900, `markdlg.cpp:99-140` |
| Plot | RTKPLOT を起動し, monitor port (TCP) で解を流す | monitor stream | NM:471-486, 2168-2191 |
| 入力 stream の設定 | 3 本. 稼働中は変えられない | — | NM:616-679 |
| 出力と log の stream の設定 | 出力 2 本, log 3 本. 稼働中も close と open で即反映する | `rtksvrclosestr`, `rtksvropenstr` | NM:718-848 |
| option, 表示の切替, layout | option 画面, 解の書式, plot の種類, 画面の配置 | — | NM:488-614, 866-1012, 338-436 |

main 画面には, CPU 時間, 欠落観測数, message 数は出ない. これらは monitor 画面にある.

## 3. mrtklib-docker-ui の RT tab (`4fcaf45`)

- backend は `mrtk run` を起動し, telnet で 1 秒ごとに `status`, `satellite`, `observ` を送って表示を解析する (✔ S:573-601, S:475-565). 結果を WebSocket で browser に送る.
- 表示: 実行の状態, 時刻 (GPST の文字列), LLH, 解の品質, ratio, age, 衛星系ごとの使用 / 可視の数, 2D の散布図, 地図 (OSM), sky plot と SNR (rover だけ, 信号の最大値), 品質・衛星数・ratio の時系列, 解の text の一覧, process の stdout / stderr (RT:633-740 ほか).
- 操作: Start / Stop, stream の編集 (入力の書式に CLAS L6 / L6E がある. 稼働中の編集は次の Start で反映), 設定 (preset, TOML の import / export).
- 表示しないもの: stream の状態, error, 測位 mode, DOP, base の位置, 速度, σ, 基線, Mark, 時刻系や書式の切替.

## 4. 既存の GUI がほとんど表示しないもの (MALIB, CLASLIB, MADOCALIB 由来の機能)

- L6 の補正 stream を, 補正として見た状態 (stream としての状態は 2. と 3. の stream の表示に含まれうる)
- 補正 (SSR / CSSR) の age (差分の age とは別)
- PPP-AR の fix の種類. MALIB の Windows 版の GUI は PPP-AR の fix を "PPP-FIX" と表示する (上の screenshot). WL だけの fix を区別して表示するかは分からない. ✔ MRTKLIB では NL の fix は `SOLQ_FIX` になり, WL だけの fix は内部の `SOLQ_FIX_WL` から `SOLQ_PPP` に戻される (`src/pos/mrtk_ppp.c:186`, `:1719-1720`, `:1861-1865`). 解の状態だけでは RTK の fix と PPP-AR の fix を区別できないが, 測位 mode と組み合わせれば区別できる.
- 電離層の補正の有無, IODSSR, CLAS の network / grid の ID, 衛星ごとの SSR の有無
- ✔ MRTKLIB の測位 mode には PPP-RTK, SSR2OSR, VRS-RTK まである (`apps/rtkrcv/rtkrcv.c:834-835`).

## 未確認

- docker-ui が前提とする MRTKLIB 0.7.6 と `8dc1fc6` とで, telnet の `status` / `satellite` / `observ` の出力形式が同じか.
- RTKLIB b34 の `formatstrs` に L6 の書式があるか.
- MRTKLIB の telnet の `ssr` command の出力内容.
