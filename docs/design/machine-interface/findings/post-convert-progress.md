# `mrtk post` / `mrtk convert` の進捗と中止の現状

- **対象:** `h-shiono/MRTKLIB` `develop` `8dc1fc6`
- **調査日:** 2026-10-07
- **方法:** code reading

## 事実

- 後処理は進捗を `checkbrk("processing : %s Q=%d", ...)` で報告し, `checkbrk` は `showmsg` を呼ぶ (`src/pos/mrtk_postpos.c:123-141`, `:623`). 変換は `showmsg` で `scanning: ...` などを報告する (`src/data/mrtk_convrnx.c:732`).
- 処理全体の時間範囲を知らせる `settspan(開始, 終了)` と, 現在の時刻を知らせる `settime(時刻)` も呼ばれる. これらは RTKLIB の GUI 向けの callback であり, RTKPOST は process 内でこれを使って進捗 bar を描いていた.
- `mrtk` では, `showmsg` は文字列を stderr に書き (`\r` で行を上書き), `settspan` / `settime` は空の関数である (`apps/mrtk/mrtk_main.c:34-45`). 構造化された進捗の情報は捨てられている.
- `showmsg` は常に 0 を返す. `checkbrk` の戻り値が 0 以外なら処理を中断する経路 (`src/pos/mrtk_postpos.c:623-626`) は使われていない. 中止するには process を強制終了するしかなく, 出力 file が中途半端に残る.
- `mrtk post` は出力 file を指定しないと解を stdout に書く (`apps/rnx2rtkp/rnx2rtkp.c:77`).
- 後処理の code は file 全体で共有する static 変数 (CLAS の context, L6 の file など) を持つ (`src/pos/mrtk_postpos.c:118-121`).
- docker-ui はこの stderr の文字列を正規表現で解析している ([docker-ui-integration.md](docker-ui-integration.md)).

## 中止の伝え方の比較 (2026-10-07 の整理)

| 方法 | 結果 | Windows |
|---|---|---|
| 強制終了 | 出力 file が中途半端に残り, GUI が後始末をする | 同じ方法で可能 |
| signal (SIGINT / SIGTERM) で中断の印を立てる | 穏便に終われる | GUI から起動した子 process に Ctrl+C 相当を送るのは扱いにくく, OS ごとに別の実装になる |
| stdin で中止を伝える | 穏便に終われる. GUI が落ちると stdin が閉じるので処理を自動で止められ, 持ち主のいない process が残らない | 同じ方法で動く. stdin を調べる部分は OS ごとに異なる |

## 退けた方向

post を RPC の server (job server) にする方向は, job の管理が要り, 上記の static 変数のために 1 process で同時に 1 つしか処理できず job 間の reset も要る. #326 でも範囲外とされている.
