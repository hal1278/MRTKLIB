# 0006. GUI suite は当面 egui を使い, 既存の `hal1278/mrtklib-egui` で作る

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での指示)
- **Supersedes:** [0005](0005-gui-repository-and-toolkit.md)

## Context

[0005](0005-gui-repository-and-toolkit.md) で新しい repository と Slint を選んだが, hal1278 が Slint を取り消した.

`hal1278/mrtklib-egui` (public, develop `11a58ba`, license file なし) には `AGENTS.md` と `docs/` だけがある.
その文書は MRTKLIB との境界を argv, file, stdout / stderr に限定し, RPC を含まない. 単一 application を tab で切り替える設計である.

## Decision

- toolkit は当面 egui (Rust) とする. 将来変わりうる.
- repository は既存の `hal1278/mrtklib-egui` を使う.
- 既存の内容 (`AGENTS.md`, `docs/`) は, 有用なら使ってよいが, 基本的に従う必要はない. この directory の決定 (machine interface, [principles.md](../principles.md), RTKLIB 型の構成) が優先する.

## Alternatives considered

- **新しい repository と Slint ([0005](0005-gui-repository-and-toolkit.md)).** 取り消した.
- **Qt 6, Web 技術.** 0005 で検討済み. 採用しない.

## Consequences

- 実装言語は Rust になる. egui は MIT / Apache-2.0 なので, Slint のような framework の license 選択は不要になる. GUI repository 自身の license は未決である.
- repository 名が toolkit 名 (`egui`) を含む. 0005 で決めた「名前に toolkit を入れない」とは合わないが, 当面は既存の名前を使う. 改名するかは未決である.
- **既存の `AGENTS.md` と `docs/` は agent への拘束力を持つ書き方になっている** (例: `docs/` を authority とし, 境界を `mrtk` process の argv / stdout に限定する). そのまま実装を始めると, agent が RPC を使わない古い境界に従うおそれがある. 実装を始める前に, これらを改めるか置き換える必要がある.
- 0005 で egui の懸念として挙げた点 (RTKLIB 風の見た目, 複数 window, 日本語入力 (IME)) は未検証である. 試作で確かめる.
