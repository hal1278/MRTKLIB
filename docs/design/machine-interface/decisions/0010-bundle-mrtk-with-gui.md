# 0010. `mrtk` を GUI に同梱する

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)
- **Resolves:** [unresolved.md](../unresolved.md) U-16

## Context

従来の RTKLIB user のために, RTKLIB と同様に実行 file を起動するだけで使えることが望ましい (hal1278). GUI と MRTKLIB は別 process で結合する ([0008](0008-gui-backend-coupling.md)). `mrtklib-egui` の文書には「同梱の `mrtk` と user 指定の `mrtk` の優先順位」が未決として残っていた.

## Decision

- `mrtk` を GUI と同じ folder (または installer) に同梱し, 既定では GUI は自分の隣にある `mrtk` を使う.
- user が設定で明示的に `mrtk` を指定した場合はそれを使う.
- PATH 上の `mrtk` は既定では使わない.
- 「実行 file 単体で起動できる」を, 追加の install, 常駐 process, terminal なしで GUI を起動できること, と解釈する. 1 file への統合は [0008](0008-gui-backend-coupling.md) の将来の余地 (自己起動型) とする.

## Alternatives considered

- **同梱の `mrtk` だけを使う.** 版の不一致は起きないが, 開発中の MRTKLIB で試すたびに GUI を作り直す必要がある. 採用しない.
- **別途 install した `mrtk` を PATH などから探す.** 従来の user に手間が増える. 採用しない.

## Consequences

- RTKLIB が `bin` folder に複数の実行 file を置くのと同じ体験になる.
- GUI は使用中の `mrtk` の path と版を表示できるようにする (`mrtklib-egui` の既存文書の方針と同じ).
- user が指定した `mrtk` の protocol の版が GUI と合わない場合の扱い (起動時に `getVersion` で確認して拒否するなど) を `docs/rpc/` の版の規則と合わせて決める必要がある.
