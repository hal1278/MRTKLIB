# 0002. 第一目標の範囲

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278

## Context

`mrtklib-docker-ui` は `rtkrcv` telnet console の表示, `mrtk post` / `mrtk convert` の stderr, process lifecycle を frontend 側で個別に扱っている ([findings/docker-ui-integration.md](../findings/docker-ui-integration.md)).
GUI を追加するたびに同じ処理を再実装するか, MRTKLIB 側を変更する必要がある.

## Decision

第一目標を次の 2 つとする.

1. 複数 frontend に対応する MRTKLIB 側の基盤を作る.
2. その最初の consumer として RTKLIB 型 GUI suite を実装する.

RTKLIB 型にする理由は, 従来の RTKLIB に慣れた user を対象にするためである.
そのため Windows 対応を可能な限り達成し, cross platform であることを望ましいとする.

この決定から次が従う.

- GUI suite は MRTKLIB の human-readable output を解析しない ([principles.md](../principles.md) P5).
  GUI が必要とする情報で machine-readable に得られないものは, `rtkrcv` に限らず MRTKLIB 側 machine interface の候補となる.
- GUI は `mrtk` process と通信するため, `mrtk` 自体の Windows native build が GUI の Windows 対応の前提になる.
- Machine interface の実装 (WebSocket / JSON library, thread, socket) は最初から cross platform なものを選ぶ (P6).

## Alternatives considered

- **post / convert 系の GUI を MRTKLIB を変更せずに先行実装する.** stderr の進捗行を解析することになり, 解決したい問題を GUI suite で再生産する. 採用しない.
- **Windows 対応を第一目標から外す.** 対象 user の主要環境を外すことになり, RTKLIB 型にする理由と矛盾する. 採用しない. ただし達成順序は未決 ([unresolved.md](../unresolved.md) U-02).

## Consequences

- `mrtk` の Windows native build は critical path 上の作業になる ([findings/windows-build-gap.md](../findings/windows-build-gap.md)).
- `mrtk post` / `mrtk convert` の進捗, configuration schema などについても, machine-readable interface の要否を判断する必要がある ([unresolved.md](../unresolved.md) U-05, U-06).
