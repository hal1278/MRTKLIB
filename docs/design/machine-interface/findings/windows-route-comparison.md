# Windows 対応の経路の比較

- **調査日:** 2026-10-07
- **方法:** 一般的な知識に基づく整理, Codex (`gpt-6-astra`, reasoning effort high) へのレビュー依頼, MRTKLIB / RTKLIB source の照合.
- **用途:** [decisions/0004](../decisions/0004-windows-native-platform-layer.md) の根拠. 表の各セルは MRTKLIB で build して確かめたものではない.

## 経路

| 経路 | MRTKLIB の code | "No Win32 API" 方針との関係 | 主な制約 |
|---|---|---|---|
| ① Win32 native (MSYS2 UCRT64, MSVC) | socket, serial, signal, console を Win32 API で実装する. pthread, `dirent`, `strcasecmp` は MinGW-w64 が補う | Win32 実装を再導入するので衝突する. platform layer に閉じ込めれば提案しやすい | 作業量は [windows-build-gap.md](windows-build-gap.md) の Phase A–C |
| ② POSIX 互換層 (Cygwin) | POSIX のまま, 少ない変更で build できる可能性がある | 衝突しにくい | Windows path と POSIX path, COM port 名 (COM3 = `ttyS2`) の変換が要る. `cygwin1.dll` と依存物の同梱 |
| ③ WSL2 | 変更なし | 衝突しない | WSL2 の導入が要る. COM port を直接扱えず, USB serial は `usbipd-win` などで attach する (attach 中は Windows から使えない) |
| ④ WSL1 | 変更なし | 衝突しない | serial access は WSL1 を選ぶ理由とされる (Codex 調べ, 未確認). syscall 互換性と配布負担 |
| ⑤ Windows 側の serial–TCP bridge | `mrtk` は既存の TCP stream を使う | ②③ の補助策 | 別 process が COM を所有し, baud, 制御線, 抜き差しを別に管理する |

## Codex のレビューの要点

推奨は「② Cygwin を先に検証し, 要件を満たさなければ ① UCRT64 native に進む. ③ は開発・比較用」だった. 採用しなかった理由は 0004 にある (変換の負担が各 frontend に出て P2 に反する).

採用の可否と独立に有用だった指摘:

- Cygwin で動いても #326 の task "Windows native" の達成とは扱えない. 「Windows で使えること」と「native build」は分けて合意する.
- upstream には "No Win32 API" の適用範囲を確認する.
- GUI から起動するには console を開かない起動方法が要る (rtkrcv-control.md D-27).
- `mrtk run` の serial 実装に既存の問題がある ([rtksvr-runtime.md](rtksvr-runtime.md) §4 に照合済みの事実として記録).
- RPC library は採用する Windows 経路で評価する (U-04).
- native toolchain は UCRT64 が妥当. MSYS2 は UCRT64 を推奨し MINGW64 を deprecated としている (Codex 調べ, 未確認). GUI と `mrtk` は別 process なので toolchain を揃える必要はない.
- Cygwin の license は LGPLv3-or-later に Cygwin Linking Exception (Codex 調べ, 未確認).
- 検証手順の提案: (1) build と全 ctest, (2) `mrtk relay` で serial の raw data を取得し欠落・破損と送信を確認, (3) 実機で連続測位 (最終的に 24 時間), (4) 小さな GUI launcher で process の起動・RPC・終了, (5) 配布物として未導入の PC で試験. この手順は native 経路の検証にも流用できる.

## RTKLIB との照合

- RTKLIB 2.4.3 の `src/stream.c` には `#ifdef WIN32` / `#ifndef WIN32` / `#else` が 33 箇所ある.
- RTKLIB 2.4.3 の Windows 版は serial を `\\.\<port>` で開く (`src/stream.c:389-394`). serial は `COM3:115200:...` の形で指定する.
