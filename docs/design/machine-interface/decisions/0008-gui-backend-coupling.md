# 0008. GUI と MRTKLIB は別 process で結合する

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答. 「現時点では」)
- **Resolves:** [unresolved.md](../unresolved.md) U-14

## Context

RTKLIB の GUI は RTKLIB の `src/*.c` を直接 compile して組み込む (process 内の結合). [0002](0002-first-milestone-scope.md) と [principles.md](../principles.md) P8 は別 process を前提にしていたが, 選択として明示していなかった.

## Decision

- 現時点では, GUI は同梱した `mrtk` を子 process として起動し, `mrtk run` とは RPC, `mrtk post` / `mrtk convert` とは argv と file でやり取りする.
- 将来, 次の形にできる余地を残す.
  1. **自己起動型の単一実行 file.** GUI の実行 file に MRTKLIB を link し, GUI は自分自身を backend 用の引数で子 process として起動する. 配布物は 1 file になり, process の分離と RPC の境界は残る. GUI から MRTKLIB を呼ぶのは入口の関数 1 つだけである.
  2. **process 内の thread で backend を動かす.** process 全体で共有される状態を取り除いた後に限る.

## 将来の余地を残すための制約

- frontend は protocol (RPC, argv) だけを使い, MRTKLIB の内部構造体や ABI に依存しない (P8).
- GUI 側の「backend の起動方法」を差し替えられるようにする (同梱の `mrtk` を起動する / 自分自身を起動する).
- command layer を新しく作るときは, 状態を構造体にまとめ, 新しい static 変数を増やさない. 現状の `rtkrcv.c` の static 変数と `rtksvr` / stream 層の process 全体の状態は [findings/rtkrcv-console.md](../findings/rtkrcv-console.md), [findings/rtksvr-runtime.md](../findings/rtksvr-runtime.md) にある.
- 起動完了の通知 (rtkrcv-control.md D-5) は 1 の形でも使える.

## 確認した事実 (2026-10-07)

- 各 subcommand は `int mrtk_run(int argc, char** argv)` のような関数である (`apps/rtkrcv/rtkrcv.c:2102`, `apps/rnx2rtkp/rnx2rtkp.c:134`, `apps/str2str/str2str.c:237`).
- `apps/mrtk/mrtk_main.c` の `main()` が表を引いてこれらを呼ぶ. 1 の形には, この振り分けを link できる関数にすることと, app の code を library として build できるようにすることが要る.

## Alternatives considered

- **静的 link (RTKLIB と同じ).** 実行 file 1 つで済むが, GUI が MRTKLIB の内部に依存し (#326 の要件に反する), Rust から C の内部への FFI が要り, 制御と状態の解釈を frontend ごとに書くことになる (P2). 測位側の crash で GUI も落ちる. 採用しない.
- **別 process と静的 link の両方を持つ.** command layer を C API として公開し保守する必要がある (安定した C ABI は #326 で範囲外). 採用しない.
- **`mrtk.exe` を GUI に埋め込み, 実行時に file として取り出す.** 一時 file の扱いとウイルス対策 software の誤検知の問題がある. 将来の候補としても勧めない.

## Consequences

- 配布物は GUI と `mrtk` の 2 種類の実行 file になる. 利用者の体験は配布形態 (U-16) で整える.
