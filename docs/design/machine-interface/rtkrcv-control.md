# `rtkrcv` Control Semantics

> **Status:** Draft — 判断事項 (D-n) は未合意 ·
> **Tracking:** [h-shiono/MRTKLIB#326](https://github.com/h-shiono/MRTKLIB/issues/326) ·
> **Owner of:** `rtkrcv` (`mrtk run`) の operation と state の意味

この文書は, `rtkrcv` の command layer と external interface (telnet, JSON-RPC) が共有する operation と state の意味を定義する.
method 名, payload schema, transport は定義しない. それらはこの文書の合意後に `docs/rpc/` で扱う.

現状の挙動は [findings/rtkrcv-console.md](findings/rtkrcv-console.md) と [findings/rtksvr-runtime.md](findings/rtksvr-runtime.md) に基づく.
`案` と書いた箇所は提案であり, 合意するまで確定扱いしない.

## 0. 範囲

対象:

- 単一の `mrtk run` process
- #326 comment で確認を求めた 5 項目と, docker-ui の調査で追加した 4 項目

| 確認項目 | 節 |
|---|---|
| `start` / `stop` / `restart` の state transition | §3 |
| `getConfig` / `setConfig` が表す configuration と, 稼働中の変更の扱い | §4 |
| `getStatus` / `getStreams` が返す current state の範囲 | §5 |
| `solution` / `satellites` / `streams` / `status` notification の発生条件と頻度 | §6 |
| client 接続時の current state の取得 | §6.3 |
| (追加) 起動完了と待ち受け port の通知 | §3.4 |
| (追加) `stop` と `shutdown` の区別 | §3.4 |
| (追加) 衛星情報と SNR の統合 | §5.3 |
| (追加) telnet 出力の互換性固定 | §8 |

対象外 (hal1278 が #326 の comment (2026-09-07) で提案した範囲. upstream maintainer とは未合意): MRTKLIB 全体の RPC namespace, post-processing 用 protocol, daemon / supervisor, stable C ABI. (2026-10-08 訂正: 以前は「#326 comment の合意どおり」と書いていた.)

## 1. 用語

| 用語 | 意味 |
|---|---|
| command layer | `rtkrcv` 固有の operation と state handling を持つ内部 layer. interface 非依存 |
| interface adapter | telnet console, JSON-RPC handler など. 入出力の parse, format, serialization, 接続管理を持つ |
| staged config | `set` / `load` で編集される設定. 次の start で使われる |
| active config | 最後に成功した start で server に渡された設定 |
| snapshot | ある時点の runtime state を 1 回の取得でまとめたもの |
| topic | client が購読する event の種類 |

## 2. 責務分割

command layer が持つもの:

- server lifecycle (start, stop, restart, shutdown) と独自の lifecycle state
- configuration の取得, 検証, 適用, 永続化
- runtime state の snapshot
- event の生成と購読者への配布
- operation の直列化

interface adapter が持つもの:

- telnet: command 解析, text 整形, 対話 prompt, console 表示設定 (`console-timetype`, `console-soltype`)
- JSON-RPC: JSON serialization, request / response / notification, WebSocket 接続管理, 認証

telnet だけに残すもの: `!command`, `log`, `help`, `exit`, 対話的な確認.

## 3. Lifecycle

### 3.1 現状

- `svr->state` は 0 / 1 の 2 値で, 1 にするのは server thread である. 起動直後と停止処理中を区別できない.
- `start` は block しうる (受信機 command の送信, `!WAIT`, serial / file の open).
- server thread は自分では止まらない. file 入力の終端でも稼働を続ける.
- `stop` は未起動時に何もしないが, telnet は `rtk server stop` と表示する.

### 3.2 Server state

**Fork (2026-10-07):** command layer が次の state を持ち, `svr->state` を直接公開しない (D-1–D-3 の前提として合意).

```text
            start               rtksvrstart OK
 stopped ─────────► starting ─────────────────► running
    ▲                  │ rtksvrstart NG             │
    │                  ▼                            │ stop
    └──────────── stopped (lastError を設定) ◄── stopping
```

| 現在 | 操作 | 結果 |
|---|---|---|
| `stopped` | start | `starting` → `running`, 失敗時は `stopped` と `lastError` |
| `running` | stop | `stopping` → `stopped` |
| `running` | restart | stop → start. 途中失敗は `stopped` と `lastError` |
| `starting` / `stopping` | 任意の lifecycle 操作 | D-3 |
| `running` | start | D-2 |
| `stopped` | stop | D-2 |

- **D-1.** 失敗を state (`error`) にするか, `stopped` + `lastError` にするか. **Fork (2026-10-07):** `stopped` + `lastError`. 理由: 失敗後に取れる操作は `stopped` と同じであり, 状態を増やすと遷移と client の分岐が増える. `lastError` は次に start が成功したときに消える. 退けた案: `error` 状態を設ける (GUI で失敗を目立たせやすいが, 状態と遷移が増える).
- **D-2.** 冪等性. **Fork (2026-10-07):** `running` での start は error (`alreadyRunning`). 理由: 設定を変えた後に restart のつもりで start した client が, 変更が反映されたと誤解するのを防ぐ. `stopped` での stop は成功 (no-op). 理由: 「止まっていてほしい」という意図は満たされており, GUI の終了処理などで事前に状態を確かめる手間を省ける. telnet の表示は変えない. 退けた案: どちらも成功 (設定の反映を誤解させる), どちらも error (client の手間が増える).
- **D-3.** 遷移中の lifecycle 操作. **Fork (2026-10-07):** `busy` で reject. 理由: 結果がすぐに分かり, 複数 client がいても誰の操作が効いたかが明確である. client は status を取得して遷移の完了を確かめてから再試行する (D-28 により通知ではなく取得. 2026-10-08 訂正). 退けた案: 遷移の完了まで待たせて順に実行する (応答が数秒遅れることがあり, 複数 client の操作順が分かりにくい).

### 3.3 入力の終端

file 入力の再生が終わっても server は `running` のままである.

- **D-4.** 入力終端で自動停止するか.
  背景 (2026-10-07 確認):
  - 終端は stream の `msg` に `"end"` を書くことでしか表されない (`src/stream/mrtk_stream.c:734`, `:748`). file stream の状態値は終端後も 2 のままである (`:656`). 終端の検出を UI に任せると, UI が MRTKLIB 内部の自由文を解釈することになる (P2, P5). よって終端を構造化して伝えるのは backend の責務である.
  - `clearerr` を呼ばないため, 終端後に file が伸びても読み進めない (C の EOF 指示子の性質による推論. 未実行).
  - 最後の solution が NONE の場合, 終端後も 1 Hz で NONE の solution を出力に書き続ける (`src/stream/mrtk_rtksvr.c:924-927`).
  - 出力 file は stop で close される. 止めずにいる間に process が強制終了されると出力が不完全に残りうる.
  - MRTKLIB 自身の real-time 回帰 test も, 終端を出力 file の大きさが増えなくなったことから推測している (`tests/cmake/run_rtkrcv_test.sh:129` 以降).
  - 止めないことの利点は, 入力が混在するとき (補正だけが終わり rover が続くなど) の曖昧さを避けることと, telnet の既存の挙動を保つことである. 前者は「全入力が file で全て終端」という条件で避けられる. 後者で変わるのは server の状態だけで, process を終了させる既存の script はそのまま動く.
  **Fork (2026-10-07):** 全入力が file で全て終端に達したら, backend が自動で `stopped` に遷移する. 停止の理由 (利用者, 入力終端, error) を状態に含め, 入力終端は正常な完了として `lastError` にしない. 一部の入力だけが終わった場合は streams の状態として構造化して通知する. 理由: 終端後の server は読み進めることができず, 止めないと出力 file が開いたままになり, NONE の solution を書き続けることがある. 終端を client が推測する必要もなくなる. 条件を「全入力が file で全て終端」とすることで, 入力が混在する場合に誤って止めない. 退けた案: 止めない (当初の案. 根拠が現状維持に偏っていた), rover 入力の終端だけで止める (他の入力が続く場合に早く止めすぎる).
  終端の検出 (2026-10-08 追加). 背景 (2026-10-08 確認): 通常の file は, 最後の byte を読んだ呼び出しで `feof` が立った時点で `"end"` になる (`src/stream/mrtk_stream.c:747-748`). その呼び出しで読んだ data は同じ cycle で decode と測位まで済む. timetag 付きの file は, tag file が尽きた時点で `"end"` になる (`:720-734`). 同じ呼び出しで data file の残りを読むが, 1 回に読むのは buffer の大きさ (既定 32768 byte, `apps/rtkrcv/rtkrcv.c:137`) までなので, 残りがそれより大きいと `"end"` の時点で読み残しがある. stdin からの入力は `"end"` を出さない (`:699-709`). 受信機の format によっては decoder が最後の epoch を次の epoch の到着まで出さない可能性があるが, 未確認であり, 今の rtkrcv と同じ挙動なのでこの判断の範囲外とする.
  **Fork (2026-10-08, hal1278 の作業 session での回答):** command layer は stream の message (自由文) を読まず, core に「data を全部読んだ」ことを構造化して返す関数を足して使う. timetag の終端も全部読んだ時点に揃える. この core の変更は [decisions/0020](decisions/0020-core-fixes-as-separate-prs.md) の基準による新しい単位 P7 (stream の終端) とする. stdin からの入力は終端を検出できないので, stdin の入力が 1 つでもあれば自動停止しない. 理由: MRTKLIB 内部の自由文に依存せず, timetag の末尾を読み残さない. 退けた案: layer が message の `"end"` を読む (core の変更は要らないが, 自由文への依存と timetag の読み残しが残る).
  未決: 使っていない入力 (type が none) を「全入力」の条件にどう数えるか.

### 3.4 Process lifecycle

- **起動完了の通知 (D-5).** docker-ui は固定 5 秒待って接続している. 背景: 設定で固定した port は他の software や 2 つ目の `mrtk run` と衝突しうる. port 0 で空きを得て閉じてから渡す方法は, 閉じてから bind するまでに他に取られうる. 固定の待ち時間は遅い PC では足りず速い PC では無駄で, 接続できない理由が「起動中」か「失敗」かを区別できない.
  **Fork (2026-10-07):** port は設定で固定でき, 0 なら OS が空き port を割り当てる. 待ち受けを開始したら, 確定した接続先を機械可読な 1 行で出力する. bind に失敗したら非 0 で終了する. 理由: 空き port の競合と固定の待ち時間をなくし, 1 行が出たことが準備完了の合図になる. GUI は子 process の出力を log として既に読んでいる. 出力先 (stdout / stderr) と行の形式は `docs/rpc/` で決める.
  将来の拡張のための制約: (1) local console で起動した場合 stdout は console なので, この 1 行を stdout に混ぜない. (2) 出力を読めない起動形態 (Windows service など) のために ready file を後から追加できるよう, 接続先の記述形式を出力先に依存させない. (3) 起動した親 process 以外の client が接続するには, 設定で port を固定する.
  退けた案: 固定 port に client が再試行する (競合と「起動中か失敗か」の判別が client に残る), ready file だけにする (file の後片付けが要り, GUI からの起動では冗長).
- **`-s` 起動の失敗.** 現状は process が起動したまま失敗理由の一部が捨てられる. D-1 により `stopped` + `lastError` として取得できるようにする.
- **shutdown (D-6).** #326 の初期 method set に process 終了がない. **Fork (2026-10-07):** `stop` (測位の停止) と別に `shutdown` (stop, navigation data 保存, process 終了) を operation として持つ. signal (SIGINT / SIGTERM) と Windows の console 制御 event も同じ経路に入れる. 理由: GUI が OS ごとの signal の違い (Windows には SIGUSR2 がなく, 子 process を正常終了させる手段が限られる) を気にせず, navigation data の保存と出力 file の close を伴う正常終了を行える (P6). 退けた案: signal だけで終了させる (Windows で正常終了を保証しにくい).

- **console なし起動 (D-27).** 現状の `mrtk run` は local console (`-p` なし) か telnet listener (`-p` あり) を必ず開く (`apps/rtkrcv/rtkrcv.c:2210` 付近). **Fork (2026-10-07):** RPC だけを開く起動方法を設ける. option 名などは `docs/rpc/` で決める. 理由: GUI から起動する場合 console は不要であり, telnet を開くと不要な待ち受けが増える. Windows では `vt.c` (console) を build 対象から外せる可能性がある (U-02). 退けた案: 常に console を開く (GUI は使わない telnet を開いたままにすることになり, Windows でも `vt.c` の移植が要る).

### 3.5 対話的な確認

- **出力 file の上書き (D-7).** `start` は file 出力 stream の既存 file について y/n を尋ねる. `-s` 起動では確認せず上書きする (`confwrite` が `vt == NULL` で 1 を返す).
  **Fork (2026-10-07):** start に上書きの方針を渡す. 既定は「既存 file があれば失敗し, 該当 path を返す」. GUI は user に確認してから上書きを指定して再実行する. telnet は現状どおり尋ねる. 理由: 対話できない interface で過去の記録を黙って失わない. 退けた案: 既定で上書き (現状の `-s` 起動と同じだが記録を黙って失う), 自動で別名にする (出力先が user の指定と変わる).
  **D-7a. Fork (2026-10-07):** 方針は「上書きを許す path の一覧」とする. 失敗応答で返った path のうち user が同意したものを列挙して再実行する. 一覧にない既存 file があれば再び失敗する. 理由: telnet が file ごとに尋ねる現状と意味が一致し, 確認後に設定が変わって新しい file が対象に加わっても黙って上書きしない. 退けた案: 上書き可の真偽値 (確認後に増えた対象も上書きする), 列挙値 `fail` / `overwrite` (真偽値と同じ問題).
- `save` の上書き確認も同じ扱いとする.

### 3.6 Shell に届く設定

背景 (2026-10-07 確認):

- `misc-startcmd`, `misc-stopcmd` は, start の直前・stop の直後に `system()` で実行される任意の command である (`apps/rtkrcv/rtkrcv.c:625`, `:701`). 例えば受信機の電源を入れる外部 script を呼ぶ用途を想定した機能である.
- 設定値が shell の command 文字列に組み込まれる箇所が他にもある. ftp / http の入力 stream は stream の path と `misc-proxyaddr` を組み込んだ download command を shell で実行する (`src/stream/mrtk_stream.c:2774-2792`). 圧縮された入力 file の展開も shell を経由する (`src/core/mrtk_sys.c:385-422`).
- `file-cmdfile1..3` は shell ではなく受信機に送る command を書いた file の path である. 内容は input stream に送られる (`!WAIT`, `!UBX` などは送信用の記法). 任意の file を読んで stream に送るので, path を変えられる key 全般の扱い (D-13) に関係する.
- これまでこれらを変えられたのは, configuration file を編集できる者と telnet console に login できる者だけだった. RPC の setConfig で変えられるようにすると, RPC に接続できる者が PC 上で任意の command を実行できる. loopback 限定でも, 同じ PC の他の process や, browser で開いた web page (WebSocket は同一 origin の制約を受けない) が接続しうる (認証と Origin の検査は D-25).

- 許可した場合の危険 (2026-10-07 の分析): RPC から shell に届く設定を変更できると, RPC に接続できる者が PC 上で任意の command を実行できる.
  - #326 の案では loopback での待ち受けに認証を要求しない. このとき同じ PC の他の process に加え, browser で開いた web page も接続できる. browser がどの site からでも `ws://127.0.0.1:...` への WebSocket 接続を許すなら (一般知識による. browser の仕様と実装での確認は未了. 2026-10-08 注記), server が Origin header を検査しない限り, 悪意のある page が設定を書き換えて start を送れる (cross-site WebSocket hijacking).
  - loopback 以外では token を知る者が shell を得る. 初版は TLS なしの案であり, 同じ network 上で token を盗聴されうる.
  - 既存の telnet console も `!command` と `set misc-startcmd` を持ち同種の危険がある. ただし browser は telnet (生の TCP) に接続できない. WebSocket は web page からの攻撃という新しい経路を加える.

- **D-8.** RPC から shell に届く設定を変更できるようにするか. **Fork (2026-10-07):**
  1. setConfig は `misc-startcmd` と `misc-stopcmd` の 2 つの key だけを拒否する. これらは configuration file でのみ設定できる. `loadConfig` で読み込む file に含まれる場合は許可する (file を置ける者は既に process と同じ権限を持つ).
  2. 設定値が組み込まれる外部 command (ftp / http の download, 圧縮 file の展開) は, shell の文字列ではなく引数の配列で起動する (POSIX の `exec` 系, Windows の `CreateProcess`). この process 起動は platform layer の部品とする ([decisions/0004](decisions/0004-windows-native-platform-layer.md)).
  理由: 固定の規則 1 つで済み, 起動 option や状態による切り替えが要らない. 組み込まれる値は escape ではなく shell を介さない実行で解決するので, 値が何であっても注入が起きず, RPC 用の特別な規則 (stream の type による拒否など) も要らない. Windows の `system()` は `cmd.exe` を経由し quote の規則も異なるため, process 起動は Windows 移植でも作り直す必要があり, 追加の作業はほとんどない.
  退けた案: 認証済みの client を信頼してすべて許可する (token の漏洩がそのまま shell の取得になる), RPC だけの起動では startcmd / stopcmd の実行自体を無効にする (起動 option と状態の組み合わせが増える).
  限界: これだけでは RPC の安全は完結しない. 出力 file の path を書き換えた任意 file の上書きや, `file-cmdfile` に任意の file を指定して内容を stream に送ることが残る. 「RPC を操作できるのは起動した GUI だけ」という土台は D-25 で決める.
  **Fork (2026-10-08 更新, hal1278 の作業 session での回答):** 1. を廃止し, setConfig でも `misc-startcmd` と `misc-stopcmd` を受け付ける. 2. は残す. 背景 (2026-10-08 確認): startcmd は start のときに `system()` で shell を通して実行される (`apps/rtkrcv/rtkrcv.c:625`). server は入力 stream で受け取った byte 列をそのまま log stream に複製し (`src/stream/mrtk_rtksvr.c:859`), D-13 により log の path は任意である. よって token を持つ client は, 入力 stream を自分の server への接続にし, log stream で startcmd を含む `.toml` を任意の path に書かせ, それを loadConfig して start すれば shell を得られる (推論. 未実行). 1. は security の境界になっていなかった. 理由: token を持つ client は backend を動かす OS の user と同等に信頼する (D-39) ので, shell に届く key だけを拒否しても守れるものがなく, 規則が 1 つ増えるだけである. 退けた案: 1. を誤操作の防止として残し, security の境界ではないと明記する (GUI の汎用の option 画面からの誤設定は防げるが, 規則が増える). 2. を残す理由は, 設定値を command 文字列に組み込むときの注入を防ぐためであり, 信頼の範囲とは独立している.

## 4. Configuration

### 4.1 Staged と active

- `set` / `load` は staged config を変える. 稼働中の server には restart まで反映しない (`console-*` を除く).
- `set` で変更した key には `modflgr` / `modflgs` の印が付く (`apps/rtkrcv/rtkrcv.c:1659`, `cmd_set`). `load` は印を付けない (`cmd_load`, `:1665-1687`). start は起動の成否が分かる前に印を消す (`:601-606`. `rtksvrstart` の呼び出しは `:633`). したがって印からは staged と active の差は分からない. D-9 の restart の要否は staged と active の値を比べて求める. (2026-10-08 訂正: 以前は「変更済みの key は `modflgr` / `modflgs` で分かる」と書いていた.)
- active config は現状どこにも独立して保持されていない (`svr` 内の `rtk.opt` 等に分散).

- **D-9.** 稼働中の設定変更の扱い. **Fork (2026-10-07, 当面の簡易な形. 将来変わりうる):**
  - 稼働中に即時反映する key は設けない (現状の意味を維持).
  - getConfig は既定で staged config を返す. 指定により active config (最後に成功した start 時点の copy) を返す.
  - 各 key について, staged と active が異なる (restart が必要) かを返す. setConfig は稼働中なら restart が必要であることを返す.
  理由: 現状の rtkrcv と同じ意味であり, 即時反映できる key の調査と検証を v1 から外せる.
  将来の拡張のための制約: 即時反映できる key を後から加えられるよう, setConfig の応答は key ごとの適用状態 (例: 適用済み / restart が必要) を返す形にする. 全体で 1 つの真偽値にしない.
  退けた案: 一部の key を即時反映する (対象の調査と検証が要る).

### 4.2 表現

- **D-10.** key の形. **Fork (2026-10-07):** TOML の section / key の木 (`conf/*.toml` と config reference と同じ名前) を正とし, legacy の平坦な名前 (`pos1-posmode`) は受け付けるが返さない. 値は TOML と同じ表現 (enum は文字列) とする. 理由: MRTKLIB の正式な設定形式に揃え, GUI が post 用の設定と同じ知識を使えるようにする. 退けた案: legacy の平坦な名前 (telnet の `option` / `set` と揃うが, 正式な設定形式と異なる).
  値の表現 (2026-10-08 追加). 背景 (2026-10-08 確認): 同梱の設定例は, on / off の option を boolean (`dynamics = true`), list の option を array (`signals = ["G1C", ...]`, `L1 = [10, 10, ...]`), 衛星系を名前の array (`systems = ["GPS", "Galileo", "QZSS"]`) で書く (`conf/claslib/rtkrcv.toml:4-22`). 今の TOML writer は legacy の option の型のとおりに書くので, `dynamics = "on"`, `L1 = "10,10,..."`, `signals = "G1C,G2W,..."`, `constellations = 25` (bitmask) のようになる (`src/core/mrtk_toml.c:820-834`. option の型は `src/pos/mrtk_options.c:95`, `:98`, `:116`, `:126`). loader は文字列, boolean, 数値, array のどれでも受け付ける (`src/core/mrtk_toml.c:488-574`) ので値の往復はできるが, 設定例や文書と表現が食い違う.
  **Fork (2026-10-08, hal1278 の作業 session での回答):** canonical な表現は同梱の設定例に合わせる (boolean, array, 名前で書く `systems`, enum の文字列). key ごとの型は [decisions/0011](decisions/0011-mrtklib-provides-config-handling.md) の schema (`loadopts` と同じ option 表から作る) に持たせ, writer, 文書, 検証がそれを共有する. key ごとの型の表は schema の段階で作る. 理由: user が手で書く file, 文書, GUI が受け取る値の表現が 1 つになる. 退けた案: 今の writer の表現のまま (loader は読めるが, 設定例や文書と食い違う).

### 4.3 永続化

- 現状の `save` は TOML で測位 option を落とす ([findings/rtkrcv-toml-save.md](findings/rtkrcv-toml-save.md)). saveConfig の前提として修正が必要である.
- 現状の TOML 保存は読み込んだ file の未知 key と comment を保持しない.
- **D-11.** saveConfig の意味. **Fork (2026-10-07):** rcvopts と sysopts を 1 つの TOML document として全 key 書き出す (canonical な export). 未知の key と comment は失われることを明記する. 上書きには D-7a の同意が要る. 既存の TOML 保存の欠落の修正が前提である. 理由: 設定の永続化と共有を RTKLIB と同じ形にしたので ([decisions/0012](decisions/0012-config-persistence-like-rtklib.md)), saveConfig は export であり, 部分的な書き換えは要らない (U-18 の回答に含めて確認). 退けた案: 元 file を部分的に書き換える (comment を保てるが, 整形を保つ TOML の書き換えの仕組みが要る).
  保存先と文字列 (2026-10-08 追加). 背景 (2026-10-08 確認): load と save はどちらも拡張子が `.toml` かどうかで形式を選ぶ (`src/pos/mrtk_options.c:460-463`, `:518-525`). 今の TOML writer は文字列を escape せずに `"` で囲むだけである (`src/core/mrtk_toml.c:829-834`). TOML の basic string では backslash が escape を始める (TOML 1.0 の仕様. 一般知識による) ので, `C:\data\a.obs` のような Windows の path や `"` を含む文字列を書くと, 壊れた TOML になる (推論).
  **Fork (2026-10-08, hal1278 の作業 session での回答):** (1) saveConfig に `.toml` 以外の path が渡されたら error とし, `.toml` を要求する. 理由: D-10 で legacy の名前は入力専用である. 退けた案: 拡張子で形式を選び `.conf` なら legacy で書く (telnet の `save` と同じだが D-10 と矛盾する), 拡張子によらず TOML で書く (読み戻すと拡張子で legacy として解釈され失敗する). (2) writer は TOML の規則どおりに文字列を常に escape し, 書き込みの失敗を確かめる. これは bug fix として P5 (TOML の保存, [decisions/0020](decisions/0020-core-fixes-as-separate-prs.md)) に含める. path の区切りを `/` にそろえる規約は推奨の書き方にとどめ, escape の代わりにしない. 理由: 値は user, GUI, setConfig から来るので `\` を含みうる. path 以外の文字列 (command の `"`, NTRIP の password の記号, 制御文字) も escape が要る. `\` を `/` に黙って書き換えると user の値が変わる.

### 4.4 Credential と path

- stream path は NTRIP の user / password を含みうる. console password も configuration に含まれる.
- **D-12.** getConfig / getStreams での credential の扱い. **Fork (2026-10-07):** 認証済みの client には getConfig で平文を返す. schema で秘密を含む key (stream path, console password, RPC token など) に印を付け, frontend はそれを見て log や画面で伏せる. 実行中の状態 (streams, log, status) は backend が常に伏せて出す. setConfig は平文を受け付ける. 理由: D-25 により認証済みの client は既定で起動した本人だけであり, 本人は disk 上の設定 file も読める. stream path は `user:pass@host:port/mount` という 1 つの文字列なので, getConfig で伏せると一部だけを直す編集が難しくなる. 印があれば frontend は設定の意味を知らずに伏せられる (P2). 退けた案: getConfig でも常に伏せる (伏せ字が送り返されたときに元の値を保つ仕組みが要る).
- **D-13.** loadConfig / saveConfig と出力 file の path. **Fork (2026-10-07):** backend 側 filesystem の任意の path を受け付け, 制限を設けない. GUI は絶対 path で送る. 理由: D-25 により操作できるのは本人だけであり, 任意の場所の file を扱う RTKLIB の使い方と合う. 退けた案: 許可した directory の下に限る (安全側だが RTKLIB の使い方と合わない).

### 4.5 検証

- 現状 `set` の検証は値の parse (`str2opt`) だけで, 補正源と測位 mode の整合 (`resolve_correction`) は start 時に検査される.
- **D-14.** 設定の検証. **Fork (2026-10-07):** setConfig は schema で key 単位の検査 (型, 範囲, 選択肢) を行う. key 同士の整合の検査 (補正源と測位 mode の整合など) は validate 操作として別に呼べるようにし, config 系 subcommand ([decisions/0011](decisions/0011-mrtklib-provides-config-handling.md)) と共有する. start のときにも同じ検査を行い, 失敗は `lastError` になる. 理由: 複数の key を順に変える途中の一時的な不整合で setConfig を拒否せずに済み, 検査の実装は 1 つで済む. 退けた案: setConfig で整合まで検査する (途中の不整合の扱いが要る), 解析できるかだけ (現状. 誤りが start の失敗で初めて分かる).
- 設定の読み込みの現状 (2026-10-08 確認): telnet の `load` は測位の option (sysopts) を既定値に戻してから file を読み, rtkrcv 固有の option (rcvopts) は既定値に戻さずに読む (`apps/rtkrcv/rtkrcv.c:1675-1682`). file を読めないと「no options file」と表示して終わるが, そのときには sysopts が既定値に戻っている ([findings/rtkrcv-console.md](findings/rtkrcv-console.md)). TOML の loader は不正な値を stderr に表示してその key を飛ばし (`src/core/mrtk_toml.c:653-657`), 最後は成功を返す (`:777`). 知らない key は警告して無視する (`:472`). 改名した key の古い path は対応表 (`:338-369`, 2026-10-08 時点で 27 行) で受け付け, 新しい path があればそれを優先して非推奨の警告を出す (`:326-328`, `:661-690`). 診断は stderr に出すだけなので, command layer はどの key に問題があったかを知る手段がない. sysopts は core の static 変数に読み込まれる (`src/pos/mrtk_options.c:37-39`) が, `getsysopts` / `setsysopts` (`:718`, `:740`) で退避と復元ができる.
- **D-36.** load の意味. **Fork (2026-10-08, hal1278 の作業 session での回答):** 置き換えとする. loadConfig (と command layer を通る telnet の `load`) は rcvopts も sysopts も既定値に戻してから file の値を当てる. file にない key は既定値になる. 理由: saveConfig は全 key を書く (D-11) ので保存した file は完全であり, 結果が load の前の状態に左右されない. `mrtk run -o` の起動時の読み込みとも同じ意味になる. 退けた案: 合成 (file にない key は今の staged の値を保つ. 今の rcvopts の扱い).
- **D-37.** load と setConfig が失敗したときの扱い. **Fork (2026-10-08, hal1278 の作業 session での回答):** 原子的とする. command layer は設定の mutex の下で, 今の staged を退避し, 一時の領域に読み込み (または値を当て) 検証し, error がなければ staged を一度に入れ替える. error があれば staged を元のまま保ち, すべての error を返す. 複数の key を一度に変える setConfig も同じ扱いとする. telnet の `load` も command layer を通れば, file を読めないときに sysopts が既定値に戻る今の不具合が直る. 理由: 失敗した操作が staged を中途半端に変えない. 退けた案: 当てられた分だけ当て, 結果を key ごとに返す.
- **D-38.** 問題のある key と値の扱い. 背景 (2026-10-08 の整理): file の load では, 版の違い (新しい版の key や, 新しい版で広がった値域) と user の打ち間違いを区別できない. 一方, 対話的な setConfig では, client は同じ backend の schema ([decisions/0011](decisions/0011-mrtklib-provides-config-handling.md)) を見て値を作るので, 版の違いは起きない.
  **Fork (2026-10-08, hal1278 の作業 session での回答):**

  | 操作 | 問題 | 扱い |
  |---|---|---|
  | loadConfig (file) | TOML の構文の誤り, file を読めない | error. staged は変えない (D-37) |
  | loadConfig (file) | 知らない key | 警告. 無視する |
  | loadConfig (file) | 値域の外の値 | 警告. その key は既定値とする (D-36 の置き換えと一貫する) |
  | loadConfig (file) | 古い path (新しい path と重複する場合を含む) | 警告. loader の既存の規則 (新しい path を優先) に従う |
  | setConfig | 知らない key, 値域の外の値 | error. 要求全体を拒否する (D-37) |

  警告の種類は 3 つ (知らない key, 不正な値, 非推奨の path) に固定し, key と使った値を付けて構造化して返す. GUI は警告を目立つ形で示す. これには loader が診断を stderr に出すだけでなく構造化して返す core の変更が要る. config 系 subcommand の `validate` (0011) も同じものを使う. この core の変更は [decisions/0020](decisions/0020-core-fixes-as-separate-prs.md) の基準による新しい単位 P8 (設定の読み込みの診断) とする.
  保守の工数: 版を重ねて増えるのは, key を改名するたびに loader の対応表に 1 行足す upstream の既存の作業だけである. 優先の規則は汎用であり, 組ごとの規則や文書は設けない. 警告の種類も版によって増えない.
  理由: file の load は版の違いを許し, 1 つの key の問題で全体を拒否しない. RTKLIB の legacy の loader も MRTKLIB の TOML の loader も, 不正な値を表示して飛ばす. 対話的な setConfig では誤りをすぐに client に返し, GUI の入力欄と実際の値の食い違いを防ぐ.
  退けた案: file の load で不正な値を error にして全体を拒否する (新しい版で値域が広がった file を古い版で読めない), 知らない key も error にする (新しい版で保存した file を古い版で読めない), 重複する key の組ごとに優先順位を文書に書く (改名の履歴に比例して文書が増える).
  ありうる問題点 (2026-10-08): 値の打ち間違いが既定値に化けても警告しか出ない. 例えば測位 mode の打ち間違いで, user は既定値 (single) で測位することになる. 防ぐ手段は警告を必ず見せることであり, GUI と CLI の両方で警告を見落とさない表示が要る.

## 5. Runtime state

snapshot は 1 回の `svr->lock` 取得で copy し, lock の外で整形する.
lock は `rtkpos` / `decoderaw` の間保持されるため, snapshot の取得は最大でその時間待つ (#299).

表示用の変換 (時刻系, 座標系の選択) は adapter の責務とし, snapshot は表示設定から独立した値を持つ.

- **D-15.** 時刻と座標の表現. **Fork (2026-10-07):** 表示設定 (`console-timetype`, `console-soltype`) と独立した値を返す. 時刻は GPST と UTC を併記し, 位置は ECEF と緯度経度高度を併記する. 基線の ENU などは必要になったら追加する (追加的な変更は minor 版, P8). 理由: GPST と UTC の換算に要るうるう秒の表や楕円体の座標変換を frontend ごとに持たせない (P2). 退けた案: 最小限 (GPST と ECEF のみ. frontend ごとにうるう秒の表を持つことになる), client が要求時に表現を指定する (契約と実装が複雑になる).
  **D-15a.** GPST と UTC の書式 (例: GPST を週と週秒にするか GPS epoch からの秒にするか, UTC を ISO 8601 の文字列にするか数値にするか, 小数部の精度) は未決である. hal1278 の指摘により議論の余地を残す.

### 5.1 Status

`prstatus` が表示する項目から表示書式を除いたものを基本とする.

- server: lifecycle state, `lastError`, 稼働時間, cycle, cycle あたり CPU 時間, 欠落観測数, version
- positioning: mode, 周波数数, solution status, solution 時刻, 位置 (single / float / fixed) と標準偏差, 速度, age, ratio, 衛星数 (rover / base / valid), DOP, 推定状態数, base 位置, baseline 長
- 入力ごとの message 数 (`nmsg`)

- **D-31.** status の各項目をどこから返すか.
  背景 (2026-10-08 の整理): status の解の項目を server 内部 (`rtk.sol`) から直接 copy すると, `solbuf` を定期的に読む solution の event (D-17) より先の解が入り, 初期同期 (D-22) の境界と整合しない. 一方 `sol_t` (`include/mrtklib/mrtk_sol.h:44-61`) が持つのは時刻, 位置と速度, 共分散, 受信機時計, 状態, valid の衛星数, age, ratio などであり, DOP, CPU 時間, 内部の float / fixed の推定値などはないので, status 全体を最後の解から作ることもできない.
  **Fork (2026-10-08, hal1278 の作業 session での回答):** 項目を出どころで分ける.

  | 項目 | 出どころ |
  |---|---|
  | lifecycle state, 停止の理由, `lastError` | command layer. 対応する event と同時に更新する |
  | `sol_t` にある項目 (solution 時刻, solution status, 解の位置と標準偏差, 速度, valid の衛星数, age, ratio) | 最後に publish した solution の eventと同じ snapshot |
  | mode, 周波数数, cycle, version | active config または固定の値 |
  | 稼働時間, CPU 時間, 欠落観測数, `nmsg` | 取得時刻を付けた値. event から再現できるとは約束しない |
  | DOP, rover / base の衛星数, 内部の float / fixed の推定値, 推定状態数, base 位置, baseline 長 | 取得時刻を付けた別の snapshot. 解と同じ epoch であることは約束しない |

  satellites (D-26) も独立した状態であり, 初期同期の応答全体が同じ測位 epoch を表すことは約束しない. 内部の値 (解の項目, 取得時刻を付けた値, satellites) は `running` の間だけ返す (D-30).
  理由: event を持つ項目は event と同じ snapshot から返すことで初期同期の境界と整合する. それ以外の項目は, 出どころと取得時刻を示せば解と同じ時点の値だと誤解されない.
  退けた案: すべてを server 内部から直接 copy する (単純だが, 解の項目が event の列より先行し, 初期同期の境界と整合しない).

### 5.2 Streams

9 本 (input 1-3, output 1-2, log 1-3, monitor) について:

- 役割, type, format
- 接続状態: `strstat` の 5 値 (`error`, `close`, `wait`, `connect`, `active`)
- 入出力 byte 数と bps
- path (credential は D-12 に従う)
- message: stream が最後に書いた自由文をそのまま渡す. code への写像はしない

`rtksvrsstat` 経由では `svr->lock` を待つため, command layer は各 stream の `strstat` / `strsum` を直接呼ぶ.

### 5.3 Satellites

- **D-29.** 衛星の情報と SNR をまとめるか. 背景 (2026-10-07): satellites に入れるのは衛星ごとの処理状況 (測位処理の内部状態 `rtk.ssat`) と C/N0 であり, 疑似距離, 搬送波位相, ドップラー, LLI, code の種類などの観測値 (`svr->obs`. RTKNAVI の monitor の Obs Data, rtkrcv の `observ`) とは別である. 観測値は D-16 により初版に含めない. 現状 telnet では `satellite` (方位角・仰角, 使用の有無, fix, 残差, slip, lock) と `observ` (SNR) に分かれており, docker-ui は両方を解析している.
  **Fork (2026-10-07):** satellites を 1 つの snapshot とし, 衛星ごとに ID, system, 方位角・仰角, 使用の有無を持つ. 周波数ごとの項目は「信号ごとの処理状況」とし, 使った code の種類, C/N0, 使用の有無, fix の状態, 擬似距離残差, 搬送波残差, slip 数, lock 数, reject 数を持つ. rover と base を区別する. `rtksvrostat` (方位角・仰角, SNR, 使用の有無を 1 回の lock で返す) を実装の出発点にする.
  理由: C/N0 は表示の都合だけでなく, 測位処理が信号を使うかの判断材料である (SNR mask `[positioning.snr_mask]`, v0.6.10 以降の C/N0 による重み付け). 使用の有無と並べて C/N0 がなければ, 信号が使われない理由を説明できない. telnet の `satellite` に C/N0 がなく docker-ui が `observ` と組み合わせていたことも, GUI 以外の client にこの組み合わせが要ることを示す. 当初挙げた理由 (RTKNAVI の主画面に常に表示される) は GUI の都合であり, hal1278 の指摘を受けて理由を改めた.
  重複の定義: satellites の C/N0 は測位処理が最後に使った epoch の各信号の値, 観測値 (将来追加) の SNR は受信した観測 data そのものの値とする. 出どころは同じで, 同じ epoch なら同じ値である.
  退けた案: C/N0 を観測値だけに置き, 観測値を初版に含める (D-16 の範囲が広がる), 2 つの取得 method に分ける (telnet と同じ分け方. client が組み合わせる必要が残る).
  epoch の揃え方と engine の差 (2026-10-08 追加).
  背景 (2026-10-08 確認): (1) `rtksvrostat` は, 衛星の一覧, 時刻, C/N0 を直近の decode で得た rover の最初の epoch (`svr->obs[rcv][0]`) から取り, 方位角・仰角と使用の flag を最後に測位処理した epoch の結果 (`rtk.ssat`) から取る (`src/stream/mrtk_rtksvr.c:1506-1520`). decode と測位は別の lock 区間で行われるので, その間に取得すると新しい epoch の C/N0 と前の epoch の処理状況が並ぶ. 1 cycle に 2 つ以上の epoch があると `obs[0][0]` は古い方である (code の順序からの推論). (2) `pntpos` は処理する epoch ごとに全衛星の `vs`, 方位角・仰角, `resp[0]`, `resc[0]`, `snr[0]` を消し, その epoch の rover の衛星について書き直す (`src/pos/mrtk_spp.c:950-977`). 衛星位置が求まらない衛星の方位角・仰角は 0 のままである (`:894`). 同時に, その epoch の rover の全衛星の `ssat.spt[0]` に観測時刻を無条件で書く (`:970`). (3) 周波数ごとの残りの項目は engine ごとに書く場所が異なる. C/N0 (`ssat.snr`) は PPP (`src/pos/mrtk_ppp.c:1731`), VRS (`src/pos/mrtk_vrs.c:2506`), CLAS (`src/clas/mrtk_clas_osr.c:1722`) が書くことを確認した. 使った信号の code (`ssat.code`) を書くのは CLAS だけだった (`src/clas/mrtk_clas_osr.c:1413`). 他の engine は未確認である. 書かない engine では, 値が 0 ではなく昔の epoch の値のまま残りうる.
  **Fork (2026-10-08, hal1278 の作業 session での回答):**
  - satellites の snapshot は最後に処理した epoch に揃える. 衛星の一覧, 方位角・仰角, C/N0, 使用の有無などをすべて `rtk.ssat` から `svr->lock` の下で copy し, `rtksvrostat` は使わない. 一覧は `ssat.spt[0]` が最新の時刻に等しい衛星とし, その時刻を snapshot の時刻とする. 工数: command layer の中で全衛星分の `ssat` を走査して copy するだけであり, core の変更は要らない. 受信したままの観測値の SNR は D-16 の観測データ (§10) の側に置く.
  - engine によって書かれない項目は, 項目ごとに「この engine では提供しない」を値なしで表し, 値から推測しない. engine ごとの提供表は `docs/rpc/` で engine ごとに source を確かめて作る. v1 では engine を変えない.
  理由: D-29 の目的 (信号が使われない理由を説明する) には, 1 つの処理 epoch の結果がそろっていることが要る. 提供しない項目を値なしで表せば, client が古い値や 0 を処理状況と誤解しない.
  退けた案: 観測と処理状況を別の部分として返す (client が衛星ごとに突き合わせる必要がある), `rtksvrostat` 相当のまま epoch の混在を明記する (D-29 の目的に合わない), 全 engine が全項目を書くよう engine を直す (`src/pos/` などの変更と測位の回帰の確認が要る), 全 engine に共通の項目だけに絞る (信号ごとの処理状況が失われる).
  ありうる問題点 (2026-10-08): (1) `spt` は `pntpos` の内部用の欄 (TDCP 用, #318) であり, 意味が変わると一覧が黙って壊れる. contract test で一覧を確かめる. 必要になれば, 処理した epoch の衛星の一覧を core が明示的に持つ (§10). (2) 提供表が正しくないと, 書かない engine の古い値を返す. `pntpos` が消すのは第 1 周波数の一部の項目だけである. (3) 単独測位に失敗した epoch でも一覧と第 1 周波数の C/N0 は書かれるが, 衛星位置が求まらない衛星の方位角・仰角は 0 になる. 0 を有効な角度と誤解させない表現が要る.

### 5.4 Solution

epoch ごとの `sol_t` 相当: 時刻, solution status, 位置, 共分散, 速度, 衛星数, age, ratio.

### 5.5 初期 set に含めないもの

背景 (2026-10-07 確認):

- RTKLIB 2.4.3 (b34) の RTKNAVI の monitor 画面は 19 種類の表示を持つ (`app/winapp/rtknavi/mondlg.dfm:119-138`): RTK, Obs Data, Nav Data, Time/Iono, Streams, Sat Status, States, Covariance, SBAS 4 種, RTCM 3 種, Station Info, Input, Output, Error/Warning. process 内の data 構造を timer で読み出して表示する. Error/Warning は `rtk.errbuf` を読み出して空にする (`mondlg.cpp:207-211`). D-20 の `errbuf` に当たる. (2026-10-08 訂正: 以前は Error/Warning を落として 18 種類と書いていた.)
- STRSVR の monitor 画面は stream の生の byte 列を HEX / ASCII で表示する (`app/winapp/strsvr/mondlg.dfm:176-179`). peek buffer から取得する (`app/winapp/strsvr/svrmain.cpp:574`).
- data の性質による JSON-RPC との相性:
  - 構造化された一覧 (観測, 航法, 衛星の状態, RTCM の受信数など) は載る. 画面を開いている間だけ 1 秒ごとに要求すれば足り, 大きさは数 KB から数十 KB である.
  - 大きなもの (推定状態, 共分散) は範囲の指定が要る. PPP-AR では状態数が約 1,100 (#330) で, 共分散は double で約 10 MB になる. RTKNAVI は lock の下で状態 vector と共分散行列の全体を copy し, 表に全要素を並べる (`mondlg.cpp:872-925`, `ShowCov`). 範囲の指定は RTKNAVI の前例ではなく, process 間で受け渡す量を抑えるための fork の案である. (2026-10-08 訂正: 以前は「RTKNAVI も画面に見える範囲だけを表示する」と書いていた.)
  - 生の byte 列 (RTKNAVI の Input / Output, STRSVR の monitor) は JSON-RPC に向かない. JSON は文字なので binary を base64 にすると約 33% 膨らむ. WebSocket の binary frame を使う別の経路の方が素直である. rtkrcv の log stream (入力の生 data を tcpsvr などに複製する) を使えば backend の変更なしで表示できる可能性もある (P3).
  - peek buffer (`pbuf`, `sbuf`) は単一 consumer 前提の破壊的 queue であり, 複数の画面に配るには D-17 と同様の配り直しが要る.
- 19 の表示の 1 つ 1 つが契約 (schema) の追加と保守になる.

- **D-16.** navidata, ssr, 生の観測値と monitor 画面相当の data を初版に含めるか. **Fork (2026-10-07):** 初版には含めない. 将来の追加で設計を壊さないよう, 次の枠だけを決めておく. (1) monitor の data は画面を開いている間だけ要求する on-demand の取得とする. (2) 大きなものは範囲を指定して取る. (3) 生の byte 列は JSON-RPC と別の経路 (binary frame, または既存の log stream) とする. 理由: 19 の表示の 1 つ 1 つが契約の追加と保守になり, 初版に含めると重い. 構造化された data は後から追加的 (minor 版) に加えられ, 枠を先に決めておけば設計を壊さない. 退けた案: 初版に観測データと航法データなど一部を含める (契約の範囲が広がる), 枠も決めない (後で生の byte 列を JSON-RPC に無理に載せることになりうる).

## 6. Events

### 6.1 取得の対象と発生源

D-28 (pull を基本とする) により, 各対象は「状態」(取得の method で現在の値を返す) か「event」(seq 付きの ring buffer に積み, client が seq 以降を取得する) のどちらか, または両方として扱う.

| 対象 | 種類 | 発生源 | 決定 |
|---|---|---|---|
| `solution` | event | command layer が `solbuf` を定期的に読む | D-17. server が出力した解 (解がある epoch は epoch ごとに 1 件, 解がない間は NONE を最大 1 Hz) |
| `satellites` | 状態 | 要求時に server から snapshot を取り, 短時間使い回す | D-26 |
| `streams` | 状態と event | command layer の定期処理が `strstat` / `strsum` を読む | 現在の状態は取得. 接続状態と message の変化は event として積む (D-18) |
| `status` | 状態と event | command layer | 現在の状態は取得. 状態の遷移と `lastError` は event (D-19) |
| `log` | event | command layer の message, `errbuf`, `trace` の level 1 | D-20 |

- **D-17.** solution の取得方法. 現状 `solbuf` は単一 consumer 前提の破壊的 queue で, 満杯 (256) になると以後を捨てる. telnet の `solution` が読むと他の consumer から消える.
  背景 (2026-10-07 の整理, 2026-10-08 訂正): `writesol` (`src/stream/mrtk_rtksvr.c:80-120`) は, 出力 stream への書き込み, monitor port への書き込み, `solbuf` への追加を行う. 呼ばれるのは, 解がある epoch では epoch ごと (`:910-917`), 解がない間は NONE を最大 1 Hz (`:924-927`) である ([findings/rtksvr-runtime.md](findings/rtksvr-runtime.md) §2). RTKLIB 2.4.3 b34 も同じである (`src/rtksvr.c:643-660`). NONE のときは `outsols` が 0 byte を返すので, 出力 stream と monitor port には書かれず, `solbuf` にだけ入る (findings の同じ節). (2026-10-08 訂正: 以前は「epoch ごとに」と書いていた.)
  core から command layer の ring buffer (D-28) へ解を運ぶ選択肢は 2 つある. client から見える ring buffer はどちらでも同じである.
  - A (hook): `rtksvr_t` に関数 pointer の欄を加え, `writesol` から呼ぶ. command layer が登録した関数が解を ring buffer に積んですぐ戻る. 遅れはほぼなく, `solbuf` を通らないので `solbuf` の満杯による欠落はない. ただし `src/stream/mrtk_rtksvr.c` と公開 header `include/mrtklib/mrtk_rtksvr.h` を変える. layer の関数は測位の thread の中で動くので待たない作りが必須であり, 詰まれば測位が止まる. #326 本文は, 既存の code 経路を変えるのは command layer の導入だけで, 測位 logic は触らないと述べており (Internal refactoring の節), A はこれを越える. ただし本文は変更の見積もりであり, 変更してよい file を限る規則ではない. 越える場合は理由を添えて提示する. (2026-10-08 補足: 以前は「#326 は既存 code の変更を rtkrcv の command layer に限るとしており」と書いていた.)
  - B (定期的な読み出し): command layer が唯一の consumer として `solbuf` を定期的に (例: 50 ms ごと) 読み切り, ring buffer に積む. RTKLIB 2.4.3 の RTKNAVI が 100 ms ごとに `solbuf` を読み切るのと同じ方式である (`app/winapp/rtknavi/navimain.cpp:1370-1376`). 遅れは読み出しの間隔以内で, 読み出しが長く止まると満杯で捨てる (10 Hz なら 25.6 秒分までは捨てない).
  どちらでも, client が ring buffer の容量を超える間取りに来なければ欠落する (D-21). (2026-10-08 訂正: 以前は A を「満杯で捨てることもない」とし, 欠落がないかのように書いていた.)
  command layer には streams の監視 (D-18) のための定期処理の thread がどのみち要り, 同じ thread で読めば構成が単純になる. (2026-10-08 訂正: satellites は D-26 で要求時の取得になったため, 定期処理の用途から外した.)
  **Fork (2026-10-08, hal1278 の作業 session での回答. 2026-10-07 の B を契約とあわせて決め直した):**
  - 契約: `solution` の event は server が出力した解の列とする. 解がある epoch は epoch ごとに 1 件, 解がない間は NONE を最大 1 Hz であり, `solbuf` に入る列 (RTKNAVI と telnet の `solution` が読む列) と同じである. solution file と出力 stream には NONE が書かれない (`outsols` は NONE で 0 byte を返す. `src/pos/mrtk_sol.c:1792-1793`) ので, file と一致するのは解がある分である. 失敗した epoch ごとの event は出さない. 理由: RTKLIB の利用者が見慣れた列と一致し, 解がある分は file とそろう. NONE を出すので, 解を失ったことを client に伝えられる (D-19 は status に解の品質を含めず, 品質の変化を solution の event で伝える). RTKNAVI も NONE を表示・記録はしないが, その到着を server が処理を続けている合図として使う (RTKLIB 2.4.3 b34 `app/winapp/rtknavi/navimain.cpp:1370-1385`, `:1435`). core の変更も要らない. (2026-10-08 訂正: 以前は「solution file, 出力 stream, RTKNAVI が読む列と同じ」「file と RPC で解の数がそろう」と書いていた.) 失敗した epoch の数が要れば, status の counter として後から追加的に加えられる. 退けた案: 処理した epoch ごとに 1 件 (失敗を含む. epoch ごとの成否は分かるが, 測位の loop の変更が要り, file の列と食い違い, 失敗が続くと受信の rate で NONE が出る).
  - 取り出し方: B. 読み出した解は D-28 の seq 付き ring buffer に積む (当初は購読者ごとの queue としていた). telnet の `solution` も command layer 経由の読み出しに直し, `solbuf` の取り合いをなくす. 理由: GUI の表示と記録には読み出しの間隔 (50 ms) の遅れで足りる (RTKNAVI の画面の更新も 100 ms 間隔である). RTKNAVI で長く使われてきた方式である. 測位の thread で layer の code を動かさずに済み, core の公開 API の追加は欠落の件数 (P3) だけで済む. 定期処理の thread を streams と共有できる. 遅れの小ささが要る機械の用途は, RPC ではなく既存の出力 stream (TCP の NMEA など) で解を受け取る. 退けた案: A (遅れはないが, 公開 header に callback を足し, layer の code が測位の thread の中で動く. 欠落は ring buffer の容量で B と同じく起きうる). 2026-10-07 以前の当初の案は A だった. 2026-10-07 の B の理由は「変更が #326 本文の範囲に収まる」だったが, [decisions/0020](decisions/0020-core-fixes-as-separate-prs.md) で core を自前で直すことにしたため, 上の理由に置き換えた.
  - 未決: 観測が途切れた後も, 最後の NONE が同じ時刻のまま 1 Hz で繰り返されうる (`:924-927` は新しい epoch の有無によらず `svr->rtk.sol` を書くことからの推論). 繰り返しを契約に書くか layer で除くかは `docs/rpc/` で決める.
  - 未決 (2026-10-08 追加): NONE の event の座標は解ではない. `out-outsingle` が off のとき, `rtkpos` は SPP の解を `rtk->sol` に入れてから状態だけを NONE にする (`src/pos/mrtk_rtkpos.c:2756`, `:2779-2780`). その後の PPP などが失敗したときに座標が SPP の値のまま残るかは, MRTKLIB の engine ごとには未確認である (MALIB 1.2.0 `d491842` では PPP の更新が成功しなければ状態も座標も更新しない. `src/ppp.c:1229-1245`). layer で NONE の座標を落とすか, 契約に「NONE では状態と時刻だけを見る」と書くかは `docs/rpc/` で決める.
  制約: command layer が `solbuf` の唯一の consumer である. `nsol` の読み出しと 0 への書き戻しは `svr->lock` の下で行う. telnet を含め他の箇所は `solbuf` を直接読まない.
  限界: (1) 通知の遅れは読み出しの間隔以内 (例: 50 ms) である. (2) 読み出しが `MAXSOLBUF` (256) epoch 分止まると以後の解を捨てる. 解の rate によって猶予は変わる (10 Hz で 25.6 秒, 100 Hz で 2.56 秒). 捨てた件数は core が数え (P3, [decisions/0020](decisions/0020-core-fixes-as-separate-prs.md)), layer はそれを欠落の件数として ring buffer に積む. P3 が merge されるまでは, 満杯を見たときに欠落の可能性を積む. (3) 読み出しは `svr->lock` を取るため, `rtkpos` / `decoderaw` の間は待たされる (#299).
  A に移る条件: 表示に要る遅れが読み出しの間隔より短くなったとき, または満杯による欠落が実際に観測されたとき.
- **D-18.** streams の変化検出. stream の状態は read / write 時にしか更新されないため, 出力の無い stream の切断は次の書き込みまで検出されない. **Fork (2026-10-07):** この遅れを仕様として明記する. 理由: 能動的に確かめる仕組みは stream 層の変更を要し, 初版の範囲を越える. 退けた案: 定期的に接続を確かめる仕組みを作る.
- **D-19.** `status` に solution status (fix / float など) の変化を含めるか. **Fork (2026-10-07):** 含めない. `status` の event は server の状態の遷移と `lastError` だけとする. 理由: 解の品質は solution の通知で分かり, 同じ情報を 2 つの topic に載せない. 退けた案: 含める (solution を購読しない client でも品質の変化が分かるが, 情報が重複する).
- **D-20.** `log` の発生源.
  背景 (2026-10-07 確認):
  - `rtk.errbuf` (telnet の `error` の内容) に書くのは RTK (`src/pos/mrtk_rtkpos.c`, 22 箇所) と VRS (`src/pos/mrtk_vrs.c`, 5 箇所) だけである (`errmsg(rtk` を含む行から各 file の関数定義 1 行を除いた呼び出しの数. 2026-10-08 に 23 / 6 から訂正). 内容は outlier の除外, ambiguity の検定失敗, 基準局の観測なしなど. PPP, PPP-AR, CLAS (PPP-RTK), MADOCA の処理は `errbuf` に書かず, 診断を `trace()` にだけ出す. buffer は 4096 byte (`MAXERRMSG`) で, 読むと消える.
  - `trace()` / `tracet()` は trace file が開いていて level が trace level 以下のときだけ書き, それ以外では何もしない (`src/core/mrtk_trace.c:78-105`). level 1 の呼び出しは約 90 箇所, level 2 は約 800 箇所ある. level 2 には epoch ごとの slip 検出や outlier の除外と, 開発者向けの値の表示が混在する.
  - `mrtk_ctx_t` には表示用の callback の欄 `cb_showmsg` (`include/mrtklib/mrtk_context.h:89`) があるが, どこからも呼ばれていない.
  - GUI は子 process の stderr を読める. 解析せずに表示するだけなら P5 に反しない. ただし rtkrcv が stderr に出すのは起動失敗などに限られる.
  - どの案でも log の文面は人間向けの自由文であり, 契約にしない. client は表示するだけで解析しない (P5). 機械が判断に使う状態 (`lastError`, stream の状態, 解の品質) は別の method で返す.
  候補: A. command layer 自身の message と `errbuf` (変更は `apps/rtkrcv` だけ. RTK では有用だが PPP / CLAS / MADOCA ではほぼ空). B. `trace()` に出力先を足し, 指定 level 以下を流す (全 engine の診断が得られる唯一の経路. `src/core/mrtk_trace.c` の変更で #326 の範囲を越えるが, 未使用の `cb_showmsg` を使えば数行で済む. 複数 thread から呼ばれるので受け側は thread-safe にする). C. stderr を不透明な text として GUI が表示する (変更なし. 量が少ない). D. 初版は log なし.
  **Fork (2026-10-07):** A に加え, B を level 1 (error) を既定として入れる. level 2 は設定で選べるようにする. 理由: MRTKLIB の主な用途である PPP / CLAS では A だけでは engine の診断がほぼ得られない. level 1 は約 90 箇所で量が少なく user に意味がある. level 2 は量が多く開発者向けの表示を含むので既定にしない. B は `src/core/mrtk_trace.c` の変更で #326 の範囲 (rtkrcv の command layer) を越えるため, 理由を添えて例外として #326 に提示する. B の core の変更は RPC の PR に混ぜず, 別の PR (P4) として出す (2026-10-08, [decisions/0020](decisions/0020-core-fixes-as-separate-prs.md)). 退けた案: A のみ (PPP / CLAS で engine の診断がほぼ得られない), 初版は log なし (stderr の表示だけになる).

- **D-26.** `satellites` の取得. #326 の open question (頻度) である. **Fork (2026-10-07):** satellites は状態として扱い, 取得の method で返す. 求められたときに server から現在の値を copy して返し, snapshot には観測の時刻を付ける. 直前の snapshot が十分新しければ (例: 0.5 秒以内) それを返し, 古ければ `svr->lock` を取って新たに copy する. 取得の間隔は画面の必要を知る client が決める.
  仕組みの補足 (hal1278 の質問への回答): event (解など) は command layer の定期処理が 1 件ずつ seq と GPS 時刻を付けて ring buffer に積み, client は seq 以降をすべて受け取る. 「時刻を指定して最も近い 1 件」ではない. 状態 (satellites) は ring buffer に積まず, 最新の値だけを返す. RTKLIB の server の loop 周期 (`misc-svrcycle`, 既定 10 ms) と衛星の情報が変わる周期 (受信機の epoch) は異なり, snapshot を取る時機は loop 周期に結び付けない.
  理由: 誰も問い合わせなければ負荷がなく, 複数 client が来ても lock の回数は増えない. 退けた案: layer が定期的に copy して保持する (応答は速いが, 誰も使わなくても lock を取る), epoch ごとに ring buffer に積み時刻や seq で取得する (解との突き合わせや巻き戻し表示に使えるが, 量が多く表示には不要. 必要になれば追加する).

### 6.1a 受け渡しの方式 (push と pull)

背景 (2026-10-07 の整理):

- push (購読): client が topic を購読し, backend が以後送り続ける. 送る相手は購読した client だけである. #326 の原案はこの形である. event をすぐ届けられるが, backend は client ごとの送信 queue と遅い client への対処 (D-21) を持つ.
- pull (取得): client が必要なときに取りに行く. 最も単純で, client の受け取れる速さに合う. ただし単純な状態の取得では poll の間に起きた event を取りこぼす.
- RTKLIB 2.4.3 の RTKNAVI は 100 ms 間隔の timer で画面を更新する (`app/winapp/rtknavi/navimain.dfm:1694`). timer のたびに, 解の buffer を読み切って空にし (`navimain.cpp:1370-1376`), stream の状態を `UpdateStr` (`:1399`) から `rtksvrsstat` で取得する (`:1605`). 衛星の C/N0 などは timer 5 回ごと (500 ms) の `UpdatePlot` (`:1398`) から `DrawPlot` を経て `rtksvrostat` で取得する (`:1638-1639`). すべてを自分から取りに行く pull であり, 解は溜まった分をまとめて取る. ただし server 側の buffer は満杯なら以後の解を追加しない (`src/rtksvr.c:110-113`, `MAXSOLBUF` 256) ので, 取りこぼさないのは満杯にならない間だけである. (2026-10-08 訂正: 以前は衛星の情報も timer のたびに取得し, 解を取りこぼさないと書いていた.)
- rtkrcv の telnet は user が求めたときに表示する (pull). 将来 CLI / TUI を client として作る場合もこれに近い. 現在の docker-ui も 1 秒ごとの pull である. RTKNAVI の既定画面は解と C/N0 を常時表示するが, 将来の frontend では変わりうる (hal1278 の指摘).
- 解の取りこぼしは pull でも防げる. backend が event に通し番号 (seq) を付けて一定数を ring buffer に保持し, client は「前回受け取った番号より後」を取りに行く. RTKNAVI の「溜まった分を読み切る」を複数 client で使える形にしたものである.

- **D-28.** 受け渡しの方式. **Fork (2026-10-07):** pull を基本とする. 状態 (status, streams, satellites, monitor の表示) は取得の method で返す. event (解, 状態の遷移, stream の状態の変化, log) は seq 付きの ring buffer に保持し, client は seq を指定してそれ以降を取得する. buffer から消えた分を要求されたら, 欠落があったことと保持している最古の seq を返す. push (購読と通知) は将来の最適化として追加的に加えられる.
  この案を採ると次が単純になる: D-21 (backend は client ごとの送信 queue を持たない), D-22 (現在の状態と最新の seq を取り, 以後はそれより後を取る). 失うのは遅れの小ささである (poll の間隔以内. RTKNAVI と同じ 100 ms なら表示には十分).
  #326 の原案 (subscribe / unsubscribe と notification) からの大きな変更になるため, 理由を添えて #326 に提示する.
  理由: 知られている client (RTKNAVI 型の GUI, rtkrcv / 将来の CLI / TUI, 現在の docker-ui) はすべて pull の形であり, seq 付きの ring buffer で event の取りこぼしも防げる. backend は client ごとの状態を持たずに済む. 退けた案: 状態は pull で event は push (遅れは小さいが client ごとの送信 queue と遅い client への対処が要る), #326 の原案 (satellites も含め購読で push).
  影響範囲: D-17 (配り先が ring buffer になる), D-18 / D-19 / D-20 (状態の変化, 遷移, log を event として ring buffer に積む), D-21 / D-22 (下記のとおり単純になる), D-26 (間隔は client が決める), #326 の method 一覧 (`docs/rpc/` の schema). D-16 (on-demand) とは整合する. lifecycle, 設定, 起動と認証には影響しない.
  未決: ring buffer の容量 (client がどれだけ取りに来なくても欠落しないか). `docs/rpc/` で決める.

### 6.2 配送

- **D-21.** 遅い client の扱い. **Fork (2026-10-07, D-28 から導出):** backend は client ごとの送信 queue を持たない. ring buffer から既に消えた範囲を要求した client には, 欠落があったことと保持している最古の seq を返す. 理由: D-28 により backend は client ごとの状態を持たない. 退けた案: client ごとの上限付き queue で solution は古いものから捨てる (push が前提).
- **D-32.** seq を event の全種類で 1 本にするか. **Fork (2026-10-08, hal1278 の作業 session での回答):** 1 本とする. solution, status の遷移, stream の変化, log のすべてに共通の seq を付ける. seq の順序は command layer が publish した順序であり, 測位の時刻の順序や物理的に起きた順序は約束しない. 理由: D-22 の初期同期の境界を 1 つの seq で表せる. 退けた案: 種類ごとの seq (境界が seq の組になり, D-22 を作り直すことになる).
  ありうる問題点 (2026-10-08): (1) seq の順序は測位の時刻の順序ではないので, 例えば solution の event の解の時刻が, その前の seq の event より古いことがある. client は時刻で並べ直す必要があるときは解の時刻を使う. (2) seq は 64 bit の整数とし, JSON の数値として扱う client (JavaScript など) のために 2^53 未満に収まることを契約に書く (1000 件/秒でも約 28 万年かかる). 表現は `docs/rpc/` で決める.
- **D-33.** 状態を持つ対象の event の中身. **Fork (2026-10-08, hal1278 の作業 session での回答):** 差分ではなく, 変化後のその対象の状態全体を持つ. lifecycle の遷移は新しい lifecycle state, 停止の理由, `lastError` を持ち, start の成功で `lastError` が消えることもこれで伝わる. stream の変化はその stream の状態全体 (D-18 の接続状態と message, その時点の byte 数と bps) を持つ. solution の event は解そのものであり, NONE の event で status の解の品質も NONE になる (D-31). client は初期同期の snapshot にその後の event を順に当てはめれば状態を保てる.
  通信量の見積もり (2026-10-08. 項目の数からの推論であり, 未測定): 状態全体を持つのは stream の変化と lifecycle の遷移であり, どちらもまれにしか起きない (stream は接続状態か message が変わったとき, lifecycle は start と stop のとき). 1 件は数百 byte である. 量の大部分は solution の event (1 件 400 – 600 byte, 受信機の rate. 100 Hz でも約 50 KB/s) であり, 差分にする余地がない. よって状態全体を持たせても通信量は問題にならない.
  理由: client が状態を取り直さずに済み, 取り直しと次の event の間の境界の問題 (D-22) が起きない. 退けた案: 変化したことだけを伝え, client が状態を取り直す (event は小さいが, 取り直しの間の境界の問題が戻る).
  ありうる問題点 (2026-10-08): (1) stream の状態全体には path が含まれるので, event にも D-12 の credential の扱い (状態や log では伏せる) を同じように当てる. (2) timetag 付きの file を再生している間, stream の message は読むたびに再生時刻 (`"T+12.3s"` の形, 0.1 秒刻み) に書き換わる (`src/stream/mrtk_stream.c:736`. `strread` は `stream->msg` に直接書く, `:3321`). message の変化を event にすると (D-18), 再生中の stream ごとに約 10 件/秒の event が出る. 量は小さい (1 件数百 byte) が, ring buffer を他の event より速く消費する. 進捗を表す message を変化の検出から除くかは, D-18 の詳細として `docs/rpc/` の前に決める. (3) 状態全体の大きさは対象の項目が増えるほど大きくなる. 項目を足すときは頻度とあわせて見直す.
- **D-34.** event の種類による絞り込み. **Fork (2026-10-08, hal1278 の作業 session での回答):** v1 では設けない. client はすべての種類の event を受け取り, 要らないものを捨てる. 1 回の取得に上限がある場合, 次の cursor は最後に返した event の seq とする. 理由: 既定 (log は level 1, D-20) では量の大部分が GUI の必要とする solution であり, 絞り込まなくても実用上の問題がない. log の level 2 は量が多くなりうるが, 開発者が設定で選んだときだけ出る. 絞り込みは §10 の将来の拡張とする. その場合, 返さなかった種類の seq を欠落と誤解しないよう, cursor は server が走査し終えた seq とし, 欠落は server が明示する (D-21). 退けた案: v1 から絞り込みを設ける (cursor の意味を今決める必要があり, v1 の client には要らない).
  ありうる問題点 (2026-10-08): (1) 絞り込みがないので, minor 版で新しい種類の event を足すと, 古い client にもその event が届く. client は知らない種類の event と知らない項目を無視するという規則を契約に書く必要がある (P8 の「追加的な変更は minor 版」と整合する). (2) log の level 2 を選ぶと, すべての client に大量の log が届き, ring buffer も速く消費される (D-35).
- **D-35.** ring buffer の数と保持. **Fork (2026-10-08, hal1278 の作業 session での回答):** ring buffer は全種類で 1 つとする. 頻度の高い種類 (solution, log) が頻度の低い種類 (lifecycle の遷移など) を消しうるが, 欠落を受けた client は初期同期をやり直し (D-22), lifecycle と stream の状態は snapshot で戻る. 失うのは solution の列と log の履歴である.
  通常の接続状態での見積もり (2026-10-08. 推論であり, 未測定): 容量を例えば 4096 件とすると, 保持できる時間は 100 Hz の solution と 10 件/秒の log で約 37 秒, 10 Hz なら約 6 分である. client が 0.1 – 1 秒ごとに取りに来る限り, 未取得の event は消えない. ただし 1 回の取得の上限 (例: 1000 件) が「生成の速さ × 取得の間隔」を上回る必要がある. 消えうるのは, client が保持できる時間より長く取りに来ない場合 (GUI の固まり, PC の sleep, 切断の検知の遅れ) と, log の level 2 で event が急増し保持できる時間が数秒になる場合である. memory は, log の message 長に上限を設ければ件数 × 最大の大きさで抑えられる (1 件 1 KB なら 4 MB). 容量と取得の上限は `docs/rpc/` で実際の大きさを測って決める.
  理由: 通常の接続状態では消えることがなく, 消えた場合も状態は snapshot で戻る. 作りが単純である. 退けた案: 種類ごとに ring buffer を分け, seq は共通のまま頻度の低い種類を長く保持する (作りが複雑になる. §10 の将来の拡張).
  ありうる問題点 (2026-10-08): (1) 欠落の後に初期同期をやり直すと, GUI の軌跡や log の表示に途切れが出る. (2) 1 回の取得の上限が生成の速さに対して小さすぎると, client が追いつけず欠落と初期同期を繰り返す. 上限は生成の最大の速さから決める. (3) timetag の再生中の stream の event (D-33 の (2)) や log の level 2 が, solution の保持できる時間を縮める. (4) log の message 長に上限がないと memory の上限が決まらないので, 上限を設けて超えた分は切り詰める.

### 6.3 接続時の初期同期

- **D-22.** 接続時の初期同期.
  背景 (2026-10-08 の整理): 2026-10-07 の案は「client は接続時に現在の状態と event の最新の seq を取得し, 以後はその seq より後の event を取得する. seq で前後関係が決まり, 取りこぼしも重複もない」だった. しかし状態と seq を別々に取ると, 間に起きた変化が抜ける. 例えば streams の状態 (接続中) を取得した後に切断の event (seq=101) が積まれ, その後に最新の seq (101) を取得すると, client は切断を受け取れない. 順序を逆にすると重複し, 正しさが client の取得の順序に頼る. 状態ごとに seq を付けても, 境界が状態ごとに異なると 1 つの cursor で再開できない.
  **Fork (2026-10-08, hal1278 の作業 session での回答):**
  - 境界: 初期同期では, client が使う状態のすべてを同じ境界 (seq `w` までの event を publish した時点) の snapshot として, `w` と一緒に返す. snapshot は `w` までの event を反映し, `w` より後の event を反映しない. command layer は, event を積むこと, 対応する状態の snapshot を更新すること, publish 済みの seq を進めることを同じ短い lock (publish lock) の中で行い, 初期同期の snapshot も同じ lock の下で取る. 各項目の出どころは D-31 による. この意味を契約とし, method の形は `docs/rpc/` で決める.
  - lock の順序: server や stream の値は, それぞれの lock (`svr->lock`, stream の lock) の下で局所変数に copy してから lock を解き, その後で publish lock を取って反映する. publish lock を持ったまま `svr->lock` や stream の lock を待たない. 測位は `svr->lock` の中で `trace()` を呼ぶ (`src/stream/mrtk_rtksvr.c:900-908`) ので, D-20 の出力先が publish lock を取る場合も, 順序は `svr->lock` → publish lock の一方向に限られる. 送信, 直列化, join は publish lock の外で行う.
  - 保証の範囲: 同じ接続の中で, snapshot とその後の取得の境界に欠落も重複も生じない. 発生源での欠落 (D-17 の `solbuf` の満杯), ring buffer からの消失 (D-21), 再試行による再受信は別に扱う. 欠落を受けた client は初期同期をやり直して状態を回復できるが, 失われた解の列や log は戻らない.
  - cursor の有効範囲: seq による cursor は 1 本の WebSocket 接続の中でだけ有効とする. client は接続のたびに初期同期からやり直し, 前の接続の cursor から再開しない. backend の process を識別する instance id は v1 に含めない. 理由: 接続は process より長く生きられないので, cursor が別の process に渡ることはない. GUI が起動した backend では token も起動ごとに作られるので, 前の process の token では新しい process に接続できない (D-25). 前提: reverse proxy 越しでも, backend 側の接続が切れれば client 側の接続も切れる (一般的な挙動による. 未確認). 失うもの: 切断していた間の event (解の列, log) は取り戻せない. 状態は初期同期で戻る. 解の記録が要る用途は, RPC ではなく server の既存の出力 stream (solution file, TCP などの出力) で受け取る.
  - stop / start を跨ぐ履歴: ring buffer は start で消す. seq は process の中で増え続け, start でも戻さないので, 同じ接続の cursor はそのまま使える. 停止時の最後の読み切り (D-30) を含め, event は次の start まで取得できる.
  退けた案: 状態ごとに整合する seq を付けて client が対象ごとに適用する (共通の cursor を作れない), 取得の順序 (先に seq, 後で状態) を契約に書く (重複の吸収を client に任せ, 古い event で表示が巻き戻りうる), instance id を v1 に入れて再接続後に cursor から再開する (第一目標の client には要らない. §10), stop / start を跨いで履歴を残す (停止後は内部の値を返さない (D-30) ので跨ぐ必要がない), 購読開始の応答に snapshot と seq を含める (2026-10-07. push が前提).

## 7. 並行実行

- command layer は 1 つの mutex で lifecycle 操作と configuration 操作を直列化する. telnet console の各 thread と RPC handler はこの layer を通す.
- snapshot の取得は lifecycle 操作と並行してよい.
- **D-30.** 起動・停止の前後に server 内部の値 (解, 衛星など) をどう返すか. 背景: start は server 内部の測位の状態を破棄して作り直すので, 起動中に内部の値を読むのは安全でない. 停止後は最後の状態が次の start まで残る. D-4 によりファイル再生の終了で自動停止するので, 停止後も最終結果を表示できることに価値がある. **Fork (2026-10-07):** `starting` の間は server の状態だけを返し, 内部の値は空とする. 停止処理を始める直前に command layer が最後の snapshot を保存し, `stopping` / `stopped` ではそれを停止中であることと合わせて返す. 次の start で消す. 理由: 再生の終了後も最終結果を表示でき, 停止処理と競合せずに済む. 退けた案: すべて空にする (停止後に最終結果が見えない), 停止後は server の内部を直接読む (停止処理との競合に注意が要る).
  **Fork (2026-10-08 更新, hal1278 の作業 session での回答):** `stopped` で返す最終結果の snapshot は, server thread が終わった後に取る. 停止の手順は, stop の要求 → thread の終了 (join) → `solbuf` の最後の読み切り (event として ring buffer に積む) → snapshot を取る → `stopped` とする. `stopping` の間は, 停止処理を始める直前の snapshot を返す. 最後の読み切りが終わるまで次の start は受け付けない (`busy`, D-3). 背景 (2026-10-08 確認): stop の要求後も実行中の cycle は最後まで処理され (`src/stream/mrtk_rtksvr.c:848`), 解を出しうる. thread の終了時の片付け (`:945-961`) は測位の状態 (`svr->rtk`) と `solbuf` に触れず, これらは次の start まで残る (`:1198-1200` で消す). join の後は書く thread がいないので競合しない. 理由: 停止処理の直前の snapshot では, 最後の cycle の結果が snapshot と event の両方から漏れうる. 2026-10-07 に退けた「停止後は server の内部を直接読む」の理由 (停止処理との競合) は, join の後に一度だけ読むことでなくなる. 退けた案: 停止処理の直前の snapshot を `stopped` でも返す (2026-10-07 の Fork. 最後の cycle の結果が漏れうる).
  **Fork (2026-10-08 再更新, hal1278 の作業 session での回答. 同日の上の更新を置き換える):** 内部の値 (解の項目, satellites, D-31 の取得時刻を付けた値) は `running` の間だけ返す. `starting`, `stopping`, `stopped` では lifecycle の状態 (停止の理由, `lastError` を含む) だけを返す. 停止の手順のうち, join の後に `solbuf` を最後に読み切って event として積むことは残す. 最後の cycle の解は `stopped` への遷移の event より前の seq で列に入り, 次の start まで取得できる (D-22). 最後の読み切りが終わるまで次の start は受け付けない (`busy`, D-3). 理由: client は停止までに受け取った event で最終結果を表示し続けられる. RTKNAVI も解を自分の配列に記録し (RTKLIB 2.4.3 b34 `app/winapp/rtknavi/navimain.cpp:1430-1456`), 表示はその配列から行う (`:1499` 以降). 停止後の snapshot を持たないので, snapshot の保存と消去の規則が要らない. 停止後に接続した client は最終結果を見られないが, solution file には残る. 退けた案: `stopped` で thread の終了後の snapshot を返す (同日の上の更新. 停止後に接続した client も最終結果を見られるが, 第一目標の client には要らない. §10).
  関係する範囲外の話: この snapshot は表示用であり, 共分散や ambiguity を含まないので, 停止時の状態を次の開始に使って収束を早める warm start には使えない. warm start は測位 engine の機能であり, この文書の範囲外とする ([unresolved.md](unresolved.md) U-19).
- **D-23.** 長時間かかる start の間の他 client の要求. **Fork (2026-10-07):** lifecycle 操作は `busy` で拒否する (D-3). 取得の操作は並行して受け付け, status は `starting` / `stopping` を返す (D-28). start は開始時点で staged config を copy してから起動処理に入る. 起動中・停止処理中の setConfig / loadConfig は受け付けて staged config だけを変え, key ごとに restart が必要であることを返す. これは稼働中の設定変更の扱い (D-9) と同じである. 理由: `busy` で拒否する場面が増えず, GUI の user が起動中に option 画面を編集してもエラーにならない. 退けた案: 設定の変更も `busy` で拒否する (実装は単純だが起動中の編集がエラーになる), すべての要求を待たせて順に処理する (応答が数秒遅れる).
- 現状のまま公開すると問題になる core 側の挙動 ([findings/rtksvr-runtime.md](findings/rtksvr-runtime.md)). core の修正は自前で書き, RPC の PR とは別の PR にする (2026-10-08, [decisions/0020](decisions/0020-core-fixes-as-separate-prs.md). P1 などはそこでの PR の単位):
  - start の二重起動 guard が thread の起動まで効かない. command layer の state で防ぎ, core でも P1 で直す.
  - stop の二重 join. command layer の state で防ぎ, core でも P1 で直す.
  - start は thread の起動の成否を待たずに成功を返し, 起動に失敗した thread は開いた stream と buffer を片付けない (2026-10-08 追記). P1 で直す. P1 が merge されるまで, command layer は `svr->state == 1` を確かめてから `running` にし, 一定時間 1 にならなければ失敗とする.
  - thread が lock なしで書く値 (基準局の平均, `cputime`) を, 書き換えの途中で読みうる (2026-10-08 追記). 表示用の値である. P2 で直す.
  - `strread` / `strwrite` が lock の前に `mode` / `port` を読む data race と, `stropen` が stream の lock を取らずに field を書き換える点 ([findings/rtksvr-runtime.md](findings/rtksvr-runtime.md) §4). 実行時の stream open / close を公開しない限り顕在化しない. v1 では公開しない. (2026-10-08 訂正: 以前は「port 解放 race」とし, 解放済み領域への参照を含意していたが, 確認できていない.)
  - `prssr` の `static` buffer. telnet adapter を layer 経由にする際に直す.

## 7.5 RPC endpoint の公開範囲と認証

#326 の Security defaults は次を提案している.

- telnet console と RPC endpoint は既定で `127.0.0.1` に bind する. それ以外への bind は明示的な option を要する.
- loopback 以外に bind した場合, configuration に設定した共有 token を接続時に要求する.
- 初版では TLS を持たず, loopback 以外での利用は reverse proxy や VPN の背後に置くよう文書化する.

- 検討事項 (D-8 の分析から): loopback でも, browser で開いた web page から接続されうる. Origin header の検査と, loopback でも token を必須にすること (例: GUI が起動時に乱数の token を作り, 環境変数で `mrtk` に渡す) を検討する. docker-ui のように browser から接続する frontend を許す場合は, 許可する Origin の一覧が要る.

- 背景 (2026-10-07 の整理):
  - loopback で待ち受けても, 接続できるのは GUI だけではない. 同じ PC の他の program, 共有 PC の他の user account (loopback は OS の全 user で共通), browser で開いた任意の web page が接続できる.
  - (一般知識による. browser の仕様と実装での確認は未了. 2026-10-08 注記) browser は, どの site の page からでも `ws://127.0.0.1:<port>` への WebSocket 接続を止めない. 接続時に `Origin` header で page の出所を伝えるので, server はこれを見て断れる. native の client は通常 `Origin` を送らない.
  - 接続されると RPC でできることはすべてできる. D-8 で shell command の設定は塞いだが, 出力 file の path による任意 file の上書きや, command file の path による任意 file の読み出しと送出は残る.
  - token は接続時に提示させる乱数の合言葉である. (2026-10-08: 渡し方は D-25 の更新で, 親が作って環境変数で子に渡す形に変えた. 以下は 2026-10-07 の整理.) backend が作り, 起動した GUI にだけ渡せば, 操作できるのはその GUI だけになる. これは, 親が子 process の出力を親だけが読む pipe で受け取り, その行を log などに転送しないことを前提とする (設計上の前提. 2026-10-08 注記: 以前は「子 process の出力は親しか読めない」と事実のように書いていた). 例えば docker-ui は現在, 子 process の stdout / stderr の各行を log に転送している (`mrtklib-docker-ui` `4fcaf45` `src/mrtklib_web_ui/services/mrtk_run_service.py:603-632`).
  - TLS は通信の暗号化である. loopback の通信は PC の外に出ないので不要である. LAN 越しでは暗号化がないと token を盗聴されうる.
  - 各案で操作できる者: #326 の原案 (loopback は認証なし) では同じ PC の全 program, 他の user, 全 web page. 原案に Origin の検査を加えると web page は防げるが, 他の program と他の user は防げない. token を常に必須にすると, token を持つ者 (既定では起動した GUI) だけになる.
  - 既に user の権限で動いている malware は token がなくても file を直接読み書きできる. token が主に守るのは web page と共有 PC の他の user である.

- **D-25.** RPC endpoint の公開範囲, 認証, 設定方法. **Fork (2026-10-07):**
  - token は loopback を含め常に必須とする. 起動時に token が与えられなければ backend が乱数で作り, D-5 の準備完了の 1 行で接続先と一緒に起動した親 process に渡す. 固定 port で手動接続する用途には, 環境変数または設定で token を与えられる. token は schema で秘密の印を付け, 実行中の状態や log には出さない (D-12).
  - 検証用に, 起動時の CLI option でだけ token を無効にできる (option 名は `docs/rpc/` で決める). 設定 file と RPC からは無効にできない. loopback 以外での待ち受けとの組み合わせは起動を拒否する. 無効のときは stderr に警告を出し, `getVersion` などで認証なしであることが分かるようにする.
  - `Origin` header 付きの接続 (browser からの接続) は, 許可 list にある Origin だけを受け付ける. token を無効にしても Origin の検査は残す. browser から直接接続する frontend (docker-ui など) は許可 list に加える.
  - 既定の待ち受けは `127.0.0.1`. それ以外への bind は明示的な option を要する. 初版は TLS を持たず, loopback 以外での利用は reverse proxy や VPN の背後に置くよう文書化する (#326 の原案どおり).
  - 設定は TOML の独立した section (port, bind address, 許可する Origin) と CLI option で与え, CLI を優先する.
  理由: 操作できる者を token の保持者 (既定では起動した GUI) だけにでき, web page と共有 PC の他の user を防げる. GUI ごとに専用の backend を起動するので ([decisions/0009](decisions/0009-backend-process-per-gui.md)), token は準備完了の 1 行で受け渡せ, GUI の追加の手間はほとんどない. loopback を例外にしないので code の経路が 1 つで済む. 検証の手軽さは CLI 限定の無効化で確保し, 無効のままの設定 file が配られる危険を避ける. contract test は準備完了の 1 行から token を読めるので無効化を要さず, token がないと拒否されることも test する.
  退けた案: #326 の原案 (loopback は認証なし. web page も防げない), 原案に Origin の検査を加える (同じ PC の他の program と他の user を防げない), 設定 file でも無効化できる (無効のままの設定が配られうる), 無効化を設けず固定 token だけにする (分岐は最少だが検証の手間が残る).
  **Fork (2026-10-08 更新, hal1278 の作業 session での回答):** token の渡し方を変える. 親 (GUI) が乱数の token を作り, 環境変数で子の `mrtk` に渡す. backend は起動の直後にそれを読んで自分の環境から消し, startcmd や外部 command などの孫の process に引き継がせない. 準備完了の 1 行 (D-5) には接続先だけを書き, token は書かない. token を渡されなかったときだけ backend が作り, 手動で起動した user のために表示する. 環境変数の名前は `docs/rpc/` で決める. 背景: 2026-10-07 の案は backend が作った token を準備完了の 1 行 (stdout) で親に渡すものだったが, frontend が子の stdout を log に流すと token が漏れる. docker-ui は今, 子の stdout と stderr の各行を log に転送している (`mrtklib-docker-ui` `4fcaf45` `src/mrtklib_web_ui/services/mrtk_run_service.py:603-632`). 理由: 秘密を stdout に出さないので, frontend の log の扱いに頼らない. どの OS でも同じ方法で渡せる. 環境変数は同じ OS の user から読める (Linux では消した後も起動時の環境が同じ user から読める. 一般知識による) が, その user は D-39 により信頼の範囲内である. 退けた案: 準備完了の 1 行で渡し, 親はその行を log に出さないことを契約にする (2026-10-07 の案. frontend の扱いに頼る), stdin に 1 行書く (`mrtk run` は stdin を入力 stream や local の console に使いうる), 権限を絞った file で渡す (Windows では ACL の扱いが要り, 異常終了で file が残る), 継承した file descriptor / handle で渡す (親以外に見えないが, Windows では handle の継承を platform layer で扱う必要があり実装が多い), 引数で渡す (他の user から `ps` で見える). 親以外に一切見せない受け渡しは §10 の将来の拡張とする.
- **D-39.** token を持つ client をどこまで信頼するか. 背景 (2026-10-08 確認): D-8 の背景のとおり, token を持つ client は RPC だけで任意の path に任意の内容を書かせ, shell を得られる (推論. 未実行). telnet console も, password を通れば `!command` で shell を実行できる (`apps/rtkrcv/rtkrcv.c:1754`). **Fork (2026-10-08, hal1278 の作業 session での回答):** token を持つ client は, backend を動かしている OS の user と同等に信頼する. 守る境界は token (D-25), `Origin` の検査, loopback での待ち受けであり, その内側に境界は設けない. D-12 (getConfig で平文を返す) と D-13 (任意の path) はこの前提で一貫する. 理由: rtkrcv の機能 (任意の path への出力と log, 外部 command) を保ったまま token の内側に境界を作るには, path, stream の種類, command file, load での startcmd などを広く制限することになり, RTKLIB の使い方と大きく食い違う. 退けた案: token を持つ client を制限付きで信頼する (上の理由). ありうる問題点 (2026-10-08): token の漏洩は, その OS の user の権限での任意の操作に直結する. token は schema で秘密の印を付け, 実行中の状態と log に出さない (D-12). loopback 以外での待ち受けは TLS がないので, reverse proxy や VPN の背後に置く (#326 の原案どおり).

## 8. 互換性

telnet console は凍結する (P7). command layer の抽出は表示を変えない.

- 抽出の前に, telnet 出力の characterization test を追加する. 少なくとも docker-ui が依存する次を固定する ([findings/docker-ui-integration.md](findings/docker-ui-integration.md)):
  - `status`: label `rtk server state`, `solution status`, `pos llh single (deg,m) rover`, `# of valid satellites`, `ratio for ar validation`, `age of differential (s)`, `time of receiver clock rover` と `label : value` の形
  - `satellite`: 列の順序 (SAT, C1, Az, El, 周波数ごとの使用 flag と fix)
  - `observ`: 列の順序 (TIME, SAT, R, code, SNR)
  - prompt 文字列と login の挙動
- 値は replay data で変わりうるため, label と列構造を検査し, 値そのものは検査しない.
- D-24 の背景 (2026-10-07 確認): telnet console は `mrtk run -p <port>` で開く人間向けの操作画面であり, 受信機を置いた PC を遠隔から操作する用途がある (RTKLIB の rtkrcv から引き継いだ機能). 現状は全 interface で待ち受け ([findings/rtkrcv-console.md](findings/rtkrcv-console.md)), password が空なら login を求めない. password は `-w` で与えるが configuration file の `console-passwd` に上書きされ, 同梱の設定例には空のものがある. telnet は通信を暗号化しない. これは文書化された機能の組み合わせであり, 非公開の報告はせず, この D-24 と #326 の Security defaults で公開の場で扱う ([decisions/0019](decisions/0019-console-risks-handled-publicly.md)). docker-ui は同じ container 内から接続するので, 既定を loopback にしても影響を受けない.
- **D-24.** #326 の security defaults は telnet の bind address を `127.0.0.1` 既定にすると提案している. 現状は全 interface で待ち受ける. これは「凍結」(P7) の例外となる挙動変更であり, 既存の遠隔 telnet 利用者に影響する. **Fork (2026-10-07):** 凍結の例外として受け入れる. 既定の待ち受けを loopback にし, 他の address で待ち受けるには明示的な option を要する. loopback 以外で待ち受けるときに password が空なら起動を拒否する. release notes に明記する. 理由: 既定の設定のまま `-p` で起動しても, network 上の他者から操作されないようにする. 遠隔の利用者は option の追加と password の設定で従来どおり使える. 退けた案: telnet は変えない (凍結を優先するが, 既定の設定の危険が残る), upstream に委ねる (fork としての案を持たない). 最終判断は upstream maintainer との合意による.

## 9. 判断事項一覧

状態の意味:

| 状態 | 意味 |
|---|---|
| `Open` | 未決 |
| `Fork` | hal1278 が fork の案として確定した. #326 に提示する内容である. upstream とは未合意 |
| `Agreed` | #326 で upstream maintainer と合意した |

§5.1 status, §5.2 streams, §5.4 solution に並べた項目の一覧は, `docs/rpc/` で schema を書くときに確定する.

状態を変えるときは, 本文の該当箇所の「案」を確定した内容に書き換え, 日付と根拠 (会話・issue comment) を付ける. 確定した内容には理由と退けた案を必ず書く. 判断の前提になった背景 (事実の確認, 責務の分析) も本文に残す.

| ID | 論点 | 案 (要約) | 状態 |
|---|---|---|---|
| D-1 | 起動失敗の表し方 | `stopped` + `lastError` | Fork |
| D-2 | lifecycle 操作の冪等性 | 二重 start は error, 停止中の stop は no-op | Fork |
| D-3 | 遷移中の lifecycle 操作 | `busy` で reject | Fork |
| D-4 | 入力終端での自動停止 | 全入力が file で全て終端なら自動停止 (停止理由付き). 終端は core の構造化した関数で検出 (P7). stdin があれば自動停止しない. 一部の終端は streams で通知 | Fork |
| D-5 | 起動完了と port の通知 | port 0 可 + 確定した接続先を機械可読 1 行で出力. 拡張の制約あり | Fork |
| D-6 | `shutdown` operation | `stop` と別に持つ | Fork |
| D-7 | 出力 file の上書き | start / save に方針を渡す. 既定は既存なら失敗 | Fork |
| D-7a | start / save に渡す上書き方針の形 | 上書きを許す path の一覧 | Fork |
| D-8 | shell に届く設定 | startcmd / stopcmd も setConfig で受け付ける (D-39). 組み込まれる値は shell を介さない process 起動で解決 | Fork |
| D-9 | 稼働中の設定変更 | 即時反映なし. staged / active と key ごとの restart 要否. 応答は key ごとの適用状態 | Fork |
| D-10 | configuration の key の形 | TOML の木. legacy 名は入力のみ. 値は同梱の設定例と同じ表現で, 型は schema が持つ | Fork |
| D-11 | saveConfig の意味 | 全 key の canonical TOML (export). 前提として既存 bug を修正. `.toml` 以外の path は error. 文字列は常に escape (P5) | Fork |
| D-12 | credential | 本人には平文. schema で秘密の印. 状態は backend が伏せる | Fork |
| D-13 | load / save と出力の path | backend 側の任意 path | Fork |
| D-14 | 設定の検証 | setConfig は key 単位. 整合は validate 操作と start 時 | Fork |
| D-36 | load の意味 | 置き換え (既定値に戻してから file を当てる) | Fork |
| D-37 | load と setConfig の失敗時 | 原子的. 失敗なら staged は元のまま, 全 error を返す | Fork |
| D-38 | 問題のある key と値 | file の load は警告 (知らない key は無視, 不正な値は既定値, 古い path は既存の規則). 構文の誤りと setConfig の誤りは error. 診断は構造化 (P8) | Fork |
| D-39 | token を持つ client の信頼の範囲 | backend を動かす OS の user と同等. 境界は token, Origin, loopback | Fork |
| D-15 | 時刻と座標の表現 | GPST と UTC, ECEF と LLH を併記 | Fork |
| D-15a | GPST と UTC の書式 | 未決 | Open |
| D-16 | navidata / ssr / 生観測 / monitor 相当 | v1 に含めない. on-demand, 範囲指定, 生 byte 列は別経路という枠を決める | Fork |
| D-17 | solution の取得 | 契約は server が出力した解の列. layer が solbuf を定期的に読み seq 付き ring buffer に積む (B, D-28). 制約と限界を明記. 条件を満たせば hook (A) | Fork |
| D-18 | stream 変化の検出遅延 | 仕様として明記 | Fork |
| D-19 | status に solution status を含めるか | 含めない | Fork |
| D-20 | log の発生源 | layer の message と `errbuf`, および trace の level 1 (既定) を流す. 文面は契約にしない | Fork |
| D-21 | 遅い client | client ごとの queue なし. 消えた範囲は欠落と最古の seq を返す (D-28) | Fork |
| D-22 | 接続時の初期同期 | 1 つの境界で状態の snapshot と seq を返す. cursor は接続の中だけ有効 (再接続は初期同期から). instance id は v1 になし. ring buffer は start で消し seq は続ける | Fork |
| D-23 | 長い start の間の他要求 | lifecycle は busy. 取得は並行. 設定変更は staged のみ変え restart 要と返す (D-9 と同じ) | Fork |
| D-24 | telnet bind address の変更 | 凍結の例外. loopback 既定, 他は明示 option, loopback 以外で空 password は起動拒否 | Fork |
| D-25 | RPC endpoint の公開範囲と認証 | token 常に必須 (親が作り環境変数で渡す. 渡されなければ backend が作り表示). CLI でのみ検証用に無効化. Origin 許可 list. 既定 127.0.0.1, TLS なし | Fork |
| D-26 | `satellites` の取得 | 要求時に snapshot を取り短時間使い回す. 間隔は client (D-28) | Fork |
| D-27 | console なし起動 | RPC だけを開く起動方法を設ける | Fork |
| D-28 | 受け渡しの方式 | pull を基本. event は seq 付き ring buffer から「seq 以降」を取得. push は将来の追加 | Fork |
| D-29 | 衛星の情報と SNR | satellites に信号ごとの処理状況として C/N0 を含める. 最後に処理した epoch に揃え rtk.ssat から copy. engine が書かない項目は値なし. P / L / D などの観測値は別 (D-16) | Fork |
| D-30 | 起動・停止の前後の内部の値 | 内部の値は running の間だけ. 他の状態では lifecycle だけ. 停止時は join の後に solbuf を読み切り event として積む | Fork |
| D-31 | status の項目の出どころ | event を持つ項目は event と同じ snapshot. 他は取得時刻付きの値で, 解と同じ epoch は約束しない | Fork |
| D-32 | seq の本数 | 全種類で 1 本. 順序は publish の順序 | Fork |
| D-33 | 状態を持つ対象の event の中身 | 変化後の状態全体 | Fork |
| D-34 | event の種類による絞り込み | v1 ではなし. 絞り込みは将来の拡張 (cursor は走査し終えた seq) | Fork |
| D-35 | ring buffer の数と保持 | 1 つ. 消えたら初期同期をやり直す. 容量は docs/rpc で測って決める | Fork |

## 10. 将来の拡張

第一目標の client (RTKNAVI 相当の GUI, [decisions/0007](decisions/0007-gui-suite-composition.md)) には要らないが, 後から追加的 (minor 版) に加えられるよう, 今の設計で妨げないもの. 加えるかは必要になった時点で判断する. 各項目の定義は「関係」の欄の owner に従う.

| 拡張 | 必要になる場面 | 妨げないための今の設計 | 関係 |
|---|---|---|---|
| backend の instance id と, 再接続後の cursor からの再開 | 切断中の event も落とせない client, 自分が起動していない backend に接続し直す client | seq は process の中で単調に増やし, start でも戻さない. instance id は応答と要求に任意の項目として足せる | D-22 |
| 停止後の最終結果の snapshot | 停止後に接続した client が最終結果を見る | 停止時は join の後で `solbuf` を読み切る. 停止後も `svr->rtk` は次の start まで残る | D-30 |
| push (購読と通知) | 遅れの小ささが要る client | event に seq を付けて ring buffer に保持する | D-28 |
| `writesol` の hook | 表示に要る遅れが読み出しの間隔より短くなる, または `solbuf` の満杯による欠落が観測される | D-17 の「A に移る条件」 | D-17 |
| 失敗した epoch の数 | epoch ごとの成否を知りたい client | status の counter として加える | D-17 |
| monitor 画面相当の data | RTKNAVI の monitor 画面の再現 | on-demand, 範囲の指定, 生の byte 列は別経路という枠 | D-16 |
| event の種類による絞り込み | log の level 2 などを受け取りたくない client | cursor の意味を「server が走査し終えた seq」に広げれば足せる | D-34 |
| 種類ごとの保持 | 頻度の低い event を長く保持したい client | seq を全種類で 1 本にする (D-32) | D-35 |
| 処理した epoch の衛星の一覧を core が明示的に持つ | `ssat.spt` の意味が変わったとき | contract test で一覧の求め方の破綻を検出する | D-29 |
| token を親以外に一切見せない受け渡し (継承した file descriptor / handle, stdin) | 同じ OS の user にも token を見せたくないとき | token の渡し方を D-25 の 1 か所に閉じる | D-25 |
| token の代わりに OS の仕組みで相手を確かめる (Unix domain socket, Windows の named pipe) | native の client だけの構成で認証を OS に任せたいとき (browser はつなげない) | token は接続時に示すもので (D-25), method の定義に含まれないので, 転送路を足しても method は変わらない | D-25 |
| 設定 file に書いた schema の版を持たせる | 警告で版の違いと打ち間違いを見分けたいとき | 警告の文面を補うだけに使い, 版によって挙動を変えない (変えると改名の履歴に比例して作業が増える) | D-38 |

解の記録は, 拡張を待たずに server の既存の出力 stream (solution file, TCP などの出力) で行える (D-22). RPC で記録の欠落をなくすことが必要になったら, 1 行目の拡張で対応する.
