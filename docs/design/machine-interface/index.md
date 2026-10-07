# Machine Interface for Multiple Frontends

> **Status:** Draft (fork working copy, not yet proposed upstream) ·
> **Tracking:** [h-shiono/MRTKLIB#326](https://github.com/h-shiono/MRTKLIB/issues/326),
> [h-shiono/MRTKLIB#327](https://github.com/h-shiono/MRTKLIB/issues/327) ·
> **Branch:** `hal1278/MRTKLIB` `docs/machine-interface`

この directory は, MRTKLIB に複数 frontend 向けの machine interface を用意する取り組みの canonical design record である.

## 目的

`mrtklib-docker-ui` は `rtkrcv` telnet console の表示や `mrtk` の stderr を frontend 側で個別に解析している.
このままでは, GUI を追加するたびに同じ解析を再実装するか, 結局 MRTKLIB 側を変更する必要がある.

この取り組みは, MRTKLIB 側に machine-oriented interface を用意し, frontend が MRTKLIB の human-readable output を解析しなくてよい状態を作る.
最初の consumer として RTKLIB 型 GUI suite を実装し, interface が実用上十分であることを確認する.

## 第一目標

- 複数 frontend に対応する MRTKLIB 側の基盤 (`rtkrcv` command layer と machine interface, 必要と判断された他機能の machine-readable interface)
- その最初の consumer としての RTKLIB 型 GUI suite

範囲の詳細と根拠は [decisions/0002](decisions/0002-first-milestone-scope.md) を参照する.

## 文書構成と読む順序

新しい session (人間・agent とも) は, この順に必要な文書だけを読む.

| 順 | 文書 | 役割 | 状態 |
|---|---|---|---|
| 1 | `index.md` | 入口. 目的, 文書構成, 更新規則 | Draft |
| 2 | [`principles.md`](principles.md) | 判断基準. 各原則の出典と状態 | Draft |
| 3 | [`unresolved.md`](unresolved.md) | 未決事項. ここにある事項を確定扱いしない | Living |
| 4 | [`rtkrcv-control.md`](rtkrcv-control.md) | `rtkrcv` の operation / state の意味 (#326) | Draft |
| 5 | [`decisions/`](decisions/README.md) | 確定した判断の記録 (1判断1ファイル) | Append-only |
| 6 | [`findings/`](findings/README.md) | 判断の根拠となった調査結果 | Append-only |

## 更新規則

- この directory を正本とする. GitHub issue は議論と合意の場であり, 合意した内容はこの directory に書き戻す.
- 各概念は 1 つの owner 文書で定義する. 他文書は参照するだけで再定義しない.
- 未確定事項は `unresolved.md` に置く. 他文書で確定事項として書かない.
- 判断が確定したら `decisions/` に記録し, `unresolved.md` から該当項目を削除して decision へのリンクを残す.
- `rtkrcv-control.md` の判断事項 (D-n) は同文書の §9 で状態を管理する (`Open` → `Fork` → `Agreed`). decision file は作らない.
- `findings/` には観測した事実を commit hash と `file:line` 付きで書く. 推論は事実と区別して明記する.
- 本文は日本語で書く. code identifier, command name, file name, type name は英語表記を維持する.

## upstream との関係

[decisions/0014](decisions/0014-upstream-scope-and-language.md) による.

- 契約に当たる部分 (`principles.md`, `rtkrcv-control.md` の確定した内容, 将来の `docs/rpc/`) は, #326 / #327 で合意した後に英語で upstream へ PR する.
- `decisions/` と `findings/` は fork に残す.
- upstream に取り込まれた後は, 契約の正本は upstream の英語版とし, この directory は判断の経緯の記録とする.
- GUI 固有の設計は GUI の repository に置き, この directory は tag または commit で参照される.
- security に関わる詳細は maintainer-local の `tasks/` (gitignore 対象) に置き, この directory にも upstream にも含めない.
