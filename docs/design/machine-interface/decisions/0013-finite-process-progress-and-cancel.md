# 0013. 終わる処理の進捗は機械可読な出力 mode で, 中止は stdin で伝える

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)
- **Resolves:** [unresolved.md](../unresolved.md) U-05

## Context

`mrtk post` / `mrtk convert` は入力と設定を受け取り, 出力 file を作って終了する処理であり, リアルタイム性は要らない ([principles.md](../principles.md) P3, [0008](0008-gui-backend-coupling.md)).
RTKPOST / RTKCONV 相当が第一目標に入り ([0007](0007-gui-suite-composition.md)), GUI は進捗の表示と中止を必要とする.
現状は進捗が人間向けの文字列でしか出ず, 構造化された callback (`settspan`, `settime`) は空の関数であり, 中止は強制終了しかない. 詳細は [findings/post-convert-progress.md](../findings/post-convert-progress.md) にある.

## Decision

- 終わる処理は RPC の server にしない.
- `mrtk post` / `mrtk convert` に機械可読な進捗 mode を加える (option 名は未決. 例: `--progress json`). 1 行 1 つの JSON で, 処理の時間範囲, 現在の時刻, 状況の message を出し, 最後に結果 (終了状態, 出力 file) を出す. stdout は解の出力に使われうるので, 進捗は stdout 以外 (stderr または専用の出力先) に出す. 既定の人間向け出力は変えない.
- 中止は stdin で受け取る. GUI が中止を伝える (1 行を書く, または stdin を閉じる) と, `showmsg` が 0 以外を返して処理を中断し, 出力 file を閉じる. GUI が落ちて stdin が閉じた場合も同じく中断する.
- 変更は `apps/mrtk/mrtk_main.c` の callback (`showmsg`, `settspan`, `settime`) の範囲とし, 測位 engine は変えない. stdin を調べる部分は platform layer に置く ([0004](0004-windows-native-platform-layer.md)).

## Alternatives considered

- **GUI が stderr を解析する.** MRTKLIB を変えずに済むが P5 に反する. 採用しない.
- **post を RPC の job server にする.** job の管理が要り, 後処理の code は file 全体で共有する static 変数を持つ. #326 でも範囲外. 採用しない.
- **中止を signal で伝える.** POSIX では自然だが, Windows では GUI から起動した子 process に Ctrl+C 相当を送りにくく, OS ごとに別の実装になる. 採用しない.
- **強制終了のままにする.** 出力 file が中途半端に残る. 採用しない.

## Consequences

- epoch ごとの解の品質は callback に構造化されて渡らない. RTKPLOT 相当や結果の graph は出力の `.pos` file を読む (P3).
- `mrtklib-egui` の既存文書の「stdin を通信路に含めない」とは異なるが, [0006](0006-gui-egui-existing-repository.md) により既存文書には従わない.
- JSON の各行の形式と, 人間向けの message と同じ出力先に出す場合の区別の方法は, 実装時に決める.
