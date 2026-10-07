# 0019. rtkrcv console の危うさは非公開の報告にせず, 公開の設計と通常の bug 報告で扱う

- **Status:** Accepted
- **Date:** 2026-10-08
- **Decided by:** hal1278 (作業 session での判断)
- **Supersedes:** [0016](0016-reporting-existing-issues.md) の security の行

## Context

[0016](0016-reporting-existing-issues.md) では, rtkrcv の telnet console の危うさと, 設定値が shell の command 文字列に組み込まれる箇所を, upstream の `SECURITY.md` に従い private vulnerability reporting で報告するとした.
送信前の見直しで, hal1278 は報告するほどの security issue ではないと判断した. 確認した事実 (`h-shiono/MRTKLIB` `develop` `8dc1fc6`):

- console の password を空にする運用は文書化された機能である. usage に `-w PWD ... ("": none)` とある (`apps/rtkrcv/rtkrcv.c:181`). code の既定値は `"admin"` である (`:124`). 認証なしになるのは user か設定 file が空を選んだときである.
- shell の実行も文書化された機能である. help に `!command [arg...] : execute command in shell` とある (`:213`).
- したがって console は「認証が任意の管理用 console」という設計であり, 不具合ではない. `SECURITY.md` が対象として挙げるのは stream 層の protocol や認証の不具合, memory 安全性, path traversal などであり, console は含まれない.
- 設定値が shell に組み込まれる箇所は, 今は設定を変えられる人がもともと `misc-startcmd` で shell を実行できるため, 信頼の境界を越えない.
- 対策の方向は既に公開されている: #326 の Security defaults, [rtkrcv-control.md](../rtkrcv-control.md) の D-24 と D-8.

## Decision

- 非公開の報告 (private vulnerability reporting) はしない.
- console の危うさは, #326 の Security defaults と D-24 / D-8 の設計として公開の場で扱う.
- `-w` で与えた password が設定 file の `console-passwd` に黙って上書きされる不具合 ([findings/rtkrcv-toml-save.md](../findings/rtkrcv-toml-save.md)) は, 公開の bug として扱う. 0016 の「rtkrcv console の小さな不具合」に含め, #326 に D 項目を提示するときの背景として示す. user の意図と逆に認証なしの側に倒れる不具合であり, 同梱の設定例は空である.

## Alternatives considered

- **private advisory を送る (0016 の当初の決定).** 文書化された機能の組み合わせであり, 対策の方向も公開済みなので, 非公開の経路を使う利点が小さい. 採用しない.
- **`-w` の上書きを単独の公開 issue にする.** command layer の作業でこの部分自体を作り直すため, 0016 の扱いに揃える. 採用しない.

## Consequences

- maintainer-local の advisory の草案は削除した.
- この directory の「security に関わる詳細は公開しない」という規則は, 実際の脆弱性を見つけた場合の一般的な方針として残す.
