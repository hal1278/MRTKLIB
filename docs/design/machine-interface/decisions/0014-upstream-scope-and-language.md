# 0014. upstream には契約部分を英語で提案し, 経緯は fork に残す

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答). fork として何を提案するかの決定であり, 最終的には upstream maintainer の合意による.
- **Resolves:** [unresolved.md](../unresolved.md) U-08

## Context

upstream の `docs/` は user 向けの MkDocs site の一部であり, `docs/design/` の設計文書は英語で, 冒頭に `Status` / `Tracking` の header を持つ (例: `docs/design/configuration.md`).
この directory は日本語で書いており, `decisions/` と `findings/` は fork での判断の記録と調査結果である.
#327 は日本語で書かれ, maintainer が `status:confirmed` の label を付けている.
security に関わる詳細は maintainer-local の `tasks/` (gitignore 対象) にあり, どの場合も提案に含めない.

## Decision

- upstream には, 契約に当たる部分を英語で PR する: `principles.md`, `rtkrcv-control.md` の確定した内容, 今後の `docs/rpc/`.
- `decisions/` と `findings/` は fork に残す.
- upstream で合意され取り込まれた後は, 契約の正本を upstream の英語版とし, fork の日本語の文書は判断の経緯の記録として扱う. 2 つの言語で同じ内容を保守し続けない.

## Alternatives considered

- **すべてを日本語のまま提案する.** 経緯まで見せられるが, user 向けの site に日本語の作業記録が混ざる. 採用しない.
- **契約の文書を今から英語で書き直して進める.** 翻訳のずれは起きないが, 日本語での議論と判断の速度が落ちる. 採用しない.

## Consequences

- upstream に提案する時点で, 契約の文書を英語に訳す作業が要る.
- 取り込み後に契約を変える場合は, upstream の英語版を変更し, 必要なら fork に経緯を記録する.
- `index.md` の「upstream との関係」をこの決定に合わせる.
