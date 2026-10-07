# 0016. 調査で見つかった既存の問題の報告の仕方

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)
- **Resolves:** [unresolved.md](../unresolved.md) U-11

## Context

machine interface の調査で, それとは独立した既存の問題が見つかった ([findings/](../findings/README.md)). security に関わるものは upstream の `SECURITY.md` に従い公開の issue にできない. upstream への投稿はすべて外部への公開であり, 文面は作成後, 投稿前に hal1278 が確認する.

## Decision

| 問題 | 報告の仕方 | 時期 |
|---|---|---|
| TOML への `save` で測位 option が失われる ([findings/rtkrcv-toml-save.md](../findings/rtkrcv-toml-save.md)) | 単独の公開 issue | 今. RPC と無関係に今の利用者に起きている data の欠落であり, 修正は小さく独立している. saveConfig (D-11) の前提でもある |
| rtkrcv console の小さな不具合 (`-w` の上書き, `navidata` の引数, `-s` 起動時の失敗理由の消失) | #326 に D 項目を提示するときの背景として示す | [0003](0003-work-order.md) の手順 1. command layer の作業でこの部分自体を作り直すため, 独立した issue は立てない |
| rtksvr の起動・停止の危うさ (二重起動の防止, 二重 join, `rtksvrmark` の deadlock, stream の解放との競合) | #326 に D 項目を提示するときの背景として示す. library 側を直すなら別の issue または PR とし, RPC の PR に混ぜない | 手順 1. D-1 – D-3, D-17 などの設計の根拠であり, 実装を見せる前に共有する |
| serial の不具合 (`writeserial` が常に 0 を返す, 設定の失敗を確認しない) | 単独の公開 issue | いつでもよい. RPC と独立している |
| Windows 固有の不具合 (`time_t` の型の不一致, `%lx` など. [findings/windows-cross-build.md](../findings/windows-cross-build.md)) | Windows 対応の issue の中で, 移植の調査結果の一部として報告する | [0004](0004-windows-native-platform-layer.md) の時期 (local で build 方法を詰めた後) |
| security に関わるもの (maintainer-local の `tasks/security-notes.md`) | GitHub の private vulnerability reporting | 今 |

Windows 対応の issue は, 目的 (RTKLIB 型 GUI を従来の Windows の RTKLIB user に届けるため `mrtk` が Windows で動く必要がある), "No Win32 API" 方針と #326 の "Windows native" の task との関係, 経路 (MSYS2 UCRT64 と platform layer 方式. MSVC 対応の余地を残す), 根拠 (cross build の結果), 段階的な計画の順に書く.

## Alternatives considered

- **rtkrcv console, rtksvr, serial の問題を 1 つの issue にまとめる.** 対象の component と性質が異なり, 直した分だけ閉じられない. 採用しない.
- **すべてを RPC の実装を見せるときにまとめて報告する.** library 側の修正が RPC の PR に混ざるか, 設計の根拠を知らないまま実装を読ませることになる. 採用しない.
- **TOML の不具合も #326 への comment にまとめる.** 今の利用者への修正が #326 の進行に左右される. 採用しない.

## Consequences

- 投稿する文面の草案が必要になる: TOML の不具合の issue, serial の issue, security の private advisory (今). #326 への D 項目の提示 (手順 1). Windows 対応の issue (0004 の時期).
