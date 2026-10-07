# 0017. GUI repository の license, 名前, 既存文書, 文書の分担

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)
- **Resolves:** [unresolved.md](../unresolved.md) U-01 の残り

## Context

GUI suite は当面 egui を使い, 既存の `hal1278/mrtklib-egui` で作る ([0006](0006-gui-egui-existing-repository.md)). この repository には license file がなく, 既存の `AGENTS.md` と `docs/` は agent に従うよう指示する書き方で, MRTKLIB との境界を argv と stdout に限っている.

## Decision

- **license:** `BSD-2-Clause OR MIT OR Apache-2.0` (受け取る側が 3 つから選ぶ). 外部からの貢献は 3 つすべての下で受け入れる旨を CONTRIBUTING に書く. 同梱する `mrtk` については, GUI の license とは別に MRTKLIB の BSD-2-Clause の表示を含める.
- **名前:** 当面 `mrtklib-egui` のままとする. toolkit が固まった時点で改名を検討する (GitHub は改名後も古い URL を転送する).
- **既存の文書:** 実装を始める前に `AGENTS.md` と `docs/` を新しい方針 (RPC を含む境界, RTKLIB 型の構成, この directory への参照) で書き直す. 有用な規則 (子 process の出力を読むときの行の長さの上限, credential を伏せる処理, 表示用の文字列を実行の正本にしないことなど) は流用する.
- **文書の分担:** GUI 固有の設計 (application の構成, process の起動, 画面) は GUI repository に置く. machine interface の設計はこの directory を tag または commit で参照し, 内容を写し取らない.

## Alternatives considered

- license: `MIT OR Apache-2.0` (Rust の慣例. BSD-2 と実質ほぼ同じ), `BSD-2-Clause` のみ (最も単純). code を MRTKLIB 側へも Rust の crate 側へも動かしうるため, 3 つを並べる方を選んだ.
- 名前: 今改名する (今は文書だけなので影響は小さい). 「当面は既存の repository」([0006](0006-gui-egui-existing-repository.md)) に合わせて採用しない.
- 既存の文書: そのまま残し境界だけを直す (agent が古い前提に従うおそれが残る), 削除して一から書く (有用な規則を失う). 採用しない.
- 文書の分担: GUI の設計もこの directory に置く (MRTKLIB の repository に GUI 固有の内容が混ざる). 採用しない.

## Consequences

- 実装の最初の作業は, `mrtklib-egui` の `AGENTS.md` と `docs/` の書き直しになる.
- license は法的な助言に基づくものではない. 配布の前に `LICENSE` file 群と NOTICE の扱いを確認する.
