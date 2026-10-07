# 0015. 作業単位は当面 0003, upstream issue, `tasks/todo.md` で管理する

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)
- **Resolves:** [unresolved.md](../unresolved.md) U-10

## Context

fork `hal1278/MRTKLIB` は GitHub issue が無効で, GitHub Projects は有効である. 判断の正本はこの directory ([0001](0001-design-record-location.md)), 作業順序は [0003](0003-work-order.md) にある.
MRTKLIB の `CLAUDE.md` は session の開始時に `tasks/todo.md` と `tasks/lessons.md` を読むことを agent に求める. `tasks/` は gitignore 対象の maintainer-local 領域である.

## Decision

- 計画は [0003](0003-work-order.md), 合意は upstream の issue (#326, #327 など), session 間の引き継ぎは worktree の `tasks/todo.md` に置く.
- GUI の repository などをまたいで作業が増えたら, その時点で GitHub Projects を使う.

## Alternatives considered

- **今から GitHub Projects を使う.** 作業が少ない現時点では手間に見合わない. 採用しない.
- **fork の issue を有効にする.** 議論の場が upstream と分かれる ([0001](0001-design-record-location.md) で退けた案). 採用しない.

## Consequences

- `tasks/todo.md` は local にしかなく, 他の機械や他の人からは見えない. 判断と計画の正本は引き続きこの directory であり, `tasks/todo.md` には現在の状態と次の作業だけを書く.
