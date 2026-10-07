# 0005. GUI suite は hal1278 の新しい repository で Slint を使って作る

- **Status:** Superseded by [0006](0006-gui-egui-existing-repository.md) (2026-10-07, Slint を取り消し)
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)

## Context

#326 で upstream maintainer は Windows 用の Qt suite `mrtklib-win` を「計画中」と書いているが, 2026-10-07 時点で `h-shiono/mrtklib-win` は存在しない.
hal1278 には `mrtklib-egui` (Rust + egui, 文書のみ) がある. その境界の定義は argv, file, stdout に限られ, RPC を含まない.

## Decision

- RTKLIB 型 GUI suite は hal1278 の新しい repository で作る. maintainer の `mrtklib-win` 計画とは別物とし, 共有するのは MRTKLIB 側の machine interface (RPC の契約, `docs/rpc/`) だけとする.
- UI toolkit は Slint とする.
- toolkit は将来変わりうるため, repository 名に toolkit の名前を入れない.

## Alternatives considered

- **Qt 6 (C++).** RTKLIB の Qt 移植版を画面の手本にでき, maintainer の想定とも一致するが採用しない. 理由は hal1278 の選択による.
- **Rust + egui (`mrtklib-egui` を改める).** 採用しない.
- **Web 技術 (Tauri + React).** docker-ui と部品を共有できるが, native の小さなアプリ群という RTKLIB の感触から離れる. 採用しない.
- **maintainer の `mrtklib-win` として一緒に作る.** 採用しない.

## Consequences

- Slint の framework は 3 つの license から選ぶ (repository の `LICENSE.md`, 2026-10-07 確認):
  - Royalty-free License: desktop, mobile, web の application を無償で使える. Slint の使用を表示する必要がある (`AboutSlint` widget や badge). 組み込み機器は対象外.
  - GPLv3: open source として無償で使える. 自分の file は MIT や Apache-2.0 のままにできる.
  - Commercial license.
  どれを使うかと GUI repository 自身の license は未決である ([unresolved.md](../unresolved.md) U-01).
- 名前に toolkit を含まないため, 将来 toolkit を替えても repository を移さずに済む. toolkit 固有の code は repository 内で分離しておく必要がある.
- `mrtklib-egui` の文書 (process 境界, credential masking, TOML の round-trip など) のうち toolkit に依存しない部分の扱いが未決である.
- maintainer が `mrtklib-win` を別途作る場合, 2 つの GUI が同じ machine interface を使うことになる. これは複数 frontend に対応するという目的に沿う.
