# 0004. Windows 対応は UCRT64 native build と platform layer 方式で行う

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答). Codex (gpt-6-astra) の意見を参考にした.

## Context

GUI suite の Windows 対応には `mrtk` が Windows 上で動く必要がある ([0002](0002-first-milestone-scope.md)).
upstream は "POSIX & C11 Pure — No Win32 API" を掲げ, v0.5.7 で WIN32 code を削除している.
経路の比較は [findings/windows-route-comparison.md](../findings/windows-route-comparison.md), POSIX 依存の分布は [findings/windows-build-gap.md](../findings/windows-build-gap.md) にある.

## Decision

- 主経路は MSYS2 UCRT64 (MinGW-w64, UCRT) による Win32 native build とする.
- OS 依存の処理 (socket, serial, thread 生成, 時刻, directory 操作, process 実行, signal / console 制御) を小さな内部 API の後ろに置き, `posix` と `win32` の実装を別 file に分けて CMake で選ぶ (platform layer 方式). core の code に `#ifdef _WIN32` を散在させない.
- 進め方:
  1. 当面はこの Linux 環境で MinGW-w64 の cross build により検証し, platform layer の境界を設計する.
  2. 最終的には Windows 上での native build (MSYS2 UCRT64) を可能にする.
  3. Linux 上の検証がある程度完了したら, hal1278 の Windows PC と受信機で build と観測の検証を行う.
- upstream には, local で build method を詰めた後に Windows 対応の issue を新設して提案する. 提案では platform layer 方式であることと, "No Win32 API" 方針の適用範囲の確認を含める.
- MSVC は現時点の対象外とする. 必要になれば同じ platform layer の上に追加する.

## Alternatives considered

- **Cygwin (POSIX 互換層).** MRTKLIB の code 変更は少ない可能性があるが, Windows path と POSIX path, COM port 名 (COM3 = `ttyS2`) の変換を Windows 向けの各 frontend が実装することになり, [principles.md](../principles.md) P2 に反する. `cygwin1.dll` と依存物の同梱, 既存 Cygwin 環境との共存も負担になる. 採用しない.
- **WSL2 / WSL1.** user に WSL の導入と USB 接続の管理を求め, 通常の installer だけで COM port を選ぶ従来の RTKLIB の体験から遠い. 製品経路としては採用しない. 開発・比較用途での利用は妨げない.
- **RTKLIB と同じ散在した `#ifdef WIN32`.** upstream が v0.5.7 で除去した形そのものであり, 保守性も低い. 採用しない.

## Consequences

- platform layer の境界 (内部 API, file 配置, CMake での選択) を設計する文書が必要になる.
- native build では serial を RTKLIB と同じ `COM3:115200:...` の形で指定できる.
- upstream が platform layer 方式も受け入れない場合の扱い (fork で維持するかなど) は未決である ([unresolved.md](../unresolved.md) U-02).
