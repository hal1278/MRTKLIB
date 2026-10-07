# 0001. 設計記録の正本を repository 内の docs に置く

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (fork 作業方針として)

## Context

この取り組みは MRTKLIB 本体, GUI repository, `mrtklib-docker-ui` にまたがり, 問題と設計判断の履歴が 1 回の作業 session のコンテキストに収まらない.
後続の作業者 (人間・agent) が, 履歴全体を読み直さずに現在の決定事項を把握できる必要がある.

調査時点の状況は次の通りである.

- fork `hal1278/MRTKLIB` は GitHub issue が無効である.
- upstream には, design 文書を `docs/design/` に置き, 議論を upstream issue で追う先例がある (`docs/design/configuration.md` の `Status` / `Tracking` header).
- 合意すべき相手 (upstream maintainer) との議論は #326 / #327 で既に始まっている.

## Decision

- 設計記録の正本は `hal1278/MRTKLIB` の `docs/machine-interface` branch 上の `docs/design/machine-interface/` とする.
- upstream issue #326 / #327 は議論と合意の場とし, 合意した内容は正本に書き戻す.
- 構成は `index.md` (入口), `principles.md`, owner 文書, `unresolved.md`, `decisions/`, `findings/` とする.
- agent の memory や `AGENTS.md` / `CLAUDE.md` には正本への案内だけを書き, 履歴そのものは置かない.

## Alternatives considered

- **Issue を正本にする.** issue thread では提案, 議論, 決定が混在し, 現在の決定事項を得るには thread 全体を読み直す必要がある. コンテキストに収まらないという問題が解決しない. fork では issue が無効でもある.
- **fork の issue を有効にして作業単位を管理する.** 議論の場が upstream と fork に分散する. 作業管理が必要になった場合は GitHub Projects (fork で有効) で upstream issue も含めて管理する方が分散しない.
- **agent の memory に履歴を置く.** 特定の agent からしか読めず, review も diff もできない.

## Consequences

- 判断の変更は git diff と PR review で追跡できる.
- upstream に出す範囲と言語を別途決める必要がある ([unresolved.md](../unresolved.md) U-08).
