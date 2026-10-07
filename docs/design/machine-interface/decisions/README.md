# Decisions

確定した設計判断を 1 判断 1 ファイルで記録する.
議論の経緯は issue に残し, ここには判断の結論と理由だけを書く.

## 規則

- file 名は `NNNN-short-title.md`. 番号は再利用しない.
- 一度 `Accepted` になった decision は書き換えない. 変更する場合は新しい decision を作り, 古い方の状態を `Superseded by NNNN` にする.
- 1 ファイルは 1 画面程度に収める. 根拠となる調査は `findings/` に置いてリンクする.

## 状態

| 状態 | 意味 |
|---|---|
| `Proposed` | fork 内で提案中. 確定扱いしない |
| `Accepted` | 確定. 他文書はこれに従う |
| `Superseded by NNNN` | 後続の decision に置き換えられた |
| `Rejected` | 検討の結果採用しなかった. 再提案を防ぐために残す |

## Template

```markdown
# NNNN. Title

- **Status:** Proposed | Accepted | Superseded by NNNN | Rejected
- **Date:** YYYY-MM-DD
- **Decided by:** (誰が, どこで. issue comment があればリンク)

## Context

## Decision

## Alternatives considered

## Consequences
```

## 一覧

| No. | Title | Status |
|---|---|---|
| [0001](0001-design-record-location.md) | 設計記録の正本を repository 内の docs に置く | Accepted |
| [0002](0002-first-milestone-scope.md) | 第一目標の範囲 | Accepted |
| [0003](0003-work-order.md) | 作業順序 | Accepted |
| [0004](0004-windows-native-platform-layer.md) | Windows 対応は UCRT64 native build と platform layer 方式で行う | Accepted |
| [0005](0005-gui-repository-and-toolkit.md) | GUI suite は hal1278 の新しい repository で Slint を使って作る | Superseded by 0006 |
| [0006](0006-gui-egui-existing-repository.md) | GUI suite は当面 egui を使い, 既存の `hal1278/mrtklib-egui` で作る | Accepted |
| [0007](0007-gui-suite-composition.md) | 第一目標の GUI suite の構成 | Accepted |
| [0008](0008-gui-backend-coupling.md) | GUI と MRTKLIB は別 process で結合する | Accepted |
| [0009](0009-backend-process-per-gui.md) | backend process は GUI ごとに専用に起動する | Accepted |
| [0010](0010-bundle-mrtk-with-gui.md) | `mrtk` を GUI に同梱する | Accepted |
| [0011](0011-mrtklib-provides-config-handling.md) | 設定処理は MRTKLIB が提供する | Accepted |
| [0012](0012-config-persistence-like-rtklib.md) | 設定の永続化と共有は RTKLIB と同じ形にする | Accepted |
| [0013](0013-finite-process-progress-and-cancel.md) | 終わる処理の進捗は機械可読な出力 mode で, 中止は stdin で伝える | Accepted |
| [0014](0014-upstream-scope-and-language.md) | upstream には契約部分を英語で提案し, 経緯は fork に残す | Accepted |
| [0015](0015-task-tracking.md) | 作業単位は当面 0003, upstream issue, `tasks/todo.md` で管理する | Accepted |
| [0016](0016-reporting-existing-issues.md) | 調査で見つかった既存の問題の報告の仕方 | Accepted |
| [0017](0017-gui-repository-conventions.md) | GUI repository の license, 名前, 既存文書, 文書の分担 | Accepted |
| [0018](0018-windows-porting-plan.md) | Windows 移植の段階, CI, 設計文書の時期 | Accepted |
