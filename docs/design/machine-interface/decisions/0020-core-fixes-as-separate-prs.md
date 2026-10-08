# 0020. command layer で保証できない箇所は core を自前で直し, 単独で閉じられる単位ごとに別の PR にする

- **Status:** Accepted
- **Date:** 2026-10-08
- **Decided by:** hal1278 (作業 session での回答)
- **Related:** [0016](0016-reporting-existing-issues.md) (core の修正を RPC の PR に混ぜない), [0003](0003-work-order.md) (作業順序)

## Context

#326 本文は, 既存の code 経路を変えるのは command layer の導入だけだと見積もっている. [rtkrcv-control.md](../rtkrcv-control.md) の D-17 などは, この見積もりに収めるために core (`src/`, `include/`) を変えない案を選んでいた.

#326 に提示する前の review (2026-10-08) で, command layer だけでは保証が弱くなる箇所が見つかった. source で確認した事実 (`h-shiono/MRTKLIB` `develop` `8dc1fc6`):

- `rtksvrstart` は thread を作った時点で成功を返す (`src/stream/mrtk_rtksvr.c:1384-1394`). thread は作業領域の確保に失敗すると `state` を 1 にせずに終わり (`:829`, `:839-842`), `rtksvrstart` が開いた stream (`:1338`) と buffer を片付けない. 片付けは正常に終了するときだけ行われる (`:945-961`).
- `rtksvrstop` は `state` を確かめずに 0 にして join する (`:1405-1424`). `state` を 1 にするのは thread 自身である (`:842`).
- thread は基準局の平均 (`:879-890`) と `cputime` (`:938-939`) を lock なしで書く.
- `writesol` は `solbuf` が満杯なら黙って捨て, 捨てた件数を残さない (`:115-118`).

command layer でも回避はできる. ただし起動の失敗は時間切れでしか分からず, 開いたままの stream は layer からは片付けられない. 解の欠落は「可能性がある」としか言えない. 書き換え途中の値を読むことは防げない.
修正はいずれも数値処理を変えない. 起動に失敗したときの後始末は, 今の telnet の `start` にもある既存の問題である.

## Decision

- command layer だけで保証できない箇所は, layer の回避と限界の明記で済ませず, core を自前で修正する.
- core の修正は RPC の PR に混ぜない ([0016](0016-reporting-existing-issues.md)). 同じ関数を触り, 一緒に試験し, 単独で閉じられる単位ごとに別の PR にする. RPC の PR はそれらの上に積む.
- 単位 (2026-10-08 時点):

  | PR | 中身 | RPC の PR の依存 |
  |---|---|---|
  | P1 rtksvr の lifecycle | 起動の成否の受け渡しと起動失敗時の片付け. 同じ関数にある既存の不具合 (二重 stop の join, 二重起動の防止が thread の起動まで効かない, `rtksvrstart` の失敗の経路で buffer と CLAS / HAS context を解放しない. [findings/rtksvr-runtime.md](../findings/rtksvr-runtime.md) §1) | する |
  | P2 rtksvr の lock なしの書き込み | 基準局の平均, `cputime` など | しない (表示の正確さだけ) |
  | P3 解の欠落の件数 | `writesol` に捨てた件数の counter を足す (`rtksvr_t` の欄の追加) | する |
  | P4 trace の sink | [rtkrcv-control.md](../rtkrcv-control.md) D-20 | する |
  | P5 TOML の保存 | [h-shiono/MRTKLIB#342](https://github.com/h-shiono/MRTKLIB/issues/342). 型と escape の扱いが決まればそれも含める | する (saveConfig, D-11) |
  | P6 外部 process の起動 | D-8 の, 引数の配列による起動. Windows の platform layer ([0018](0018-windows-porting-plan.md)) と一緒に出す | Windows 対応の時期による |
  | 独立 | serial の不具合 ([h-shiono/MRTKLIB#343](https://github.com/h-shiono/MRTKLIB/issues/343) ほか), `rtksvrmark` の deadlock | しない |

- 後回しにするもの: `strread` / `strwrite` / `stropen` の race. stream を実行中に開け閉めする機能を公開するまで顕在化しない.
- 未決の論点 (D-17 の契約, 入力の終端, 衛星の情報の epoch, TOML の型, 設定の読み込みの原子性, byte 数の幅) で core の変更が要ると決まったら, 同じ基準で単位を決めて加える.
- 既存の issue と PR の確認: 各 PR について, 実装に着手する前と投稿の直前の 2 回, upstream の issue と PR を open と closed の両方で, 関数名, 症状, file 名など複数の語で検索し, 近い候補と監査の issue (例: #301) を読む. 着手前の確認は upstream で進んでいる作業との重複を避けるため, 投稿直前の確認はその間に出た issue と PR を拾うためである.
- upstream の `CONTRIBUTING.md` に従う. `src/stream/` を触る P1 – P3 は, 測位の回帰の確認として PR の template の「該当なし」を理由付きで選ぶか, 基準 data の結果を書く. 公開 API を追加する P3 と P4 は, #326 への comment で理由を添えて先に示す.

## Alternatives considered

- **command layer の回避と限界の明記だけにする.** #326 本文の見積もりに収まるが, 起動失敗の後始末が残り, 契約の一部が推定に頼る. 採用しない.
- **upstream の maintainer に修正を依頼して待つ.** 時期が読めず, RPC の作業が止まる. 採用しない.
- **core の修正を RPC の PR に別の commit として含める.** 0016 を置き換える必要があり, maintainer が core の変更を RPC と切り離して判断できない. 採用しない.
- **不具合 1 件ごとに PR にする.** 同じ関数を触る PR どうしが conflict する. 採用しない.
- **すべてを 1 つの PR にする.** 性質の違う変更が混ざり, 直した分だけ閉じられない (0016 で 1 つの issue にまとめる案を退けたのと同じ理由). 採用しない.

## Consequences

- #326 に D 項目を提示するときに, 本文の見積もりを越える core の変更 (P1 – P4) と理由を示す.
- RPC の PR は P1, P3, P4, P5 の merge に依存する. いずれかが止まっている間は, command layer の回避 (起動は `svr->state == 1` の確認と時間切れ, 欠落は「可能性がある」) で v1 を成り立たせる. 契約はどちらの場合でも成り立つ書き方にする.
- [rtkrcv-control.md](../rtkrcv-control.md) の D-17, D-20, §7 をこの判断に合わせて更新した.
