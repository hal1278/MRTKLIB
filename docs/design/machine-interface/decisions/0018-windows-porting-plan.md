# 0018. Windows 移植の段階, CI, 設計文書の時期

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)
- **Resolves:** [unresolved.md](../unresolved.md) U-02 の残りの一部

## Context

Windows 対応は MSYS2 UCRT64 による native build と platform layer 方式で行う ([0004](0004-windows-native-platform-layer.md)).
cross build の結果 ([findings/windows-cross-build.md](../findings/windows-cross-build.md)) では, Windows に近い順に l6extract, ssr2obs と dump, post 系, convert, cssr2rtcm3, relay, run だった.
第一目標の GUI が必要とするのは run (RTKNAVI 相当), post (RTKPOST 相当), convert (RTKCONV 相当) である ([0007](0007-gui-suite-composition.md)). STRSVR 相当は第一目標の外である.

## Decision

- **移植の段階:**
  - A: 共通の小さな修正 (`mrtk_time.c` の `time_t`, `mrtk_sys.c` の `mkdir`, `rtklib.h` の `sys/select.h` など) と post / convert 系.
  - B: RPC だけで起動する run ([rtkrcv-control.md](../rtkrcv-control.md) D-27). stream 層の Winsock と serial の移植が要る. console (`vt.c`) は不要である.
  - C: relay, cssr2rtcm3 (第一目標の外).
- **CI:** A の build が通った時点で, Linux 上の cross build の job を加える (この調査と同じ方法). MSYS2 上で ctest を動かす Windows runner は, test が動くようになってから加える.
- **upstream が platform layer 方式も受け入れない場合:** 今は決めず, Windows 対応の issue への反応を見てから決める.
- **platform layer の境界の設計文書:** Windows 対応の issue を出す前に, 0004 の「local で build 方法を詰める」作業の一部として, cross build の結果から草案を作る.

## Alternatives considered

- 移植の段階: run を先にする (RTKNAVI 相当を最優先にできるが, 最も遠い stream 層から始めることになる), すべてをまとめて移植する. 採用しない.
- CI: 最初から Windows runner を使う (A が通る前は失敗し続ける), upstream の合意後に決める. 採用しない.
- 不採用時: fork で維持すると今決める. 反応を見る前に決める必要はない.
- 設計文書: 今すぐ作る. Windows の issue の前で足りる.

## Consequences

- MSVC への対応は [0004](0004-windows-native-platform-layer.md) のとおり現時点の対象外であり, Windows 対応の issue では platform layer の設計で MSVC 対応の余地を残すと書く.
- CI は upstream の管理下にあるため, job の追加は upstream への PR として提案する.
