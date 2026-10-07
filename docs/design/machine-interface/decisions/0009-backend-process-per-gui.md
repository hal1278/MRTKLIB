# 0009. backend process は GUI ごとに専用に起動する

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)
- **Resolves:** [unresolved.md](../unresolved.md) U-15

## Context

「複数 frontend に対応する」は, 異なる frontend の実装が同じ interface を使えることを指す. 1 つの稼働中の backend を同時に共有することは必須ではない. #326 は 1 つの backend に複数 client が同時に接続することを許している. #326 の hal1278 の comment では, daemon / supervisor は範囲外とした.

## Decision

- GUI は自分専用の `mrtk` を子 process として起動し, GUI の寿命に従わせる.
- 既に動いている backend に別の client が接続する経路は, 設定で固定した port による接続として残す ([rtkrcv-control.md](../rtkrcv-control.md) D-5 の制約 3).
- system で 1 つの常駐 backend を複数 frontend で共有する形 (daemon / service) は, 第一目標に含めない.

## Alternatives considered

- **system で共有する常駐 backend.** backend の探索, 誰が起動・停止するか, user 間の認証を設計する必要がある. 採用しない (第一目標の範囲外).
- **両方を第一目標で実装する.** 範囲が広がる. 採用しない.

## Consequences

- RTKLIB で RTKNAVI の各 window が自分の server を持つのと同じ形になる. 受信機が複数なら `mrtk` の process も複数になる.
- token の受け渡しは「起動した GUI が作って子 process に渡す」形で済む (D-25 の検討に影響する).
- 常駐 backend を後から加える場合も, frontend は同じ protocol で接続でき, 違いは backend の起動と探索だけである.
