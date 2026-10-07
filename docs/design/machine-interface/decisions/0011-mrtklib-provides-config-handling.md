# 0011. 設定処理は MRTKLIB が提供する

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)
- **Resolves:** [unresolved.md](../unresolved.md) U-17, U-06

## Context

frontend は, 設定の TOML の読み書き, 値の検証, legacy `.conf` からの変換, key の一覧・型・選択肢・既定値 (schema) を必要とする.
docker-ui はこれを手書きで実装しており, 未知の key を落とす ([findings/docker-ui-integration.md](../findings/docker-ui-integration.md)).
RTKLIB の GUI は MRTKLIB 相当の code を process 内に組み込むので `loadopts` / `saveopts` を直接呼べたが, MRTKLIB では GUI は別 process である ([0008](0008-gui-backend-coupling.md)).

## Decision

- 設定処理は MRTKLIB が提供し, frontend は TOML の知識を持たない.
- 稼働中の `mrtk run` には RPC (getConfig, setConfig, loadConfig, saveConfig) を使う.
- 終了する処理 (`post`, `convert`) と, server を起動していない間の編集のために, `mrtk` の subcommand を用意する. 想定する操作:
  - schema: key, 型, 選択肢, 既定値, 単位, 説明を機械可読に出力する.
  - read: TOML または legacy `.conf` を正規化した機械可読な形に変換する.
  - write: 機械可読な形から正規の TOML を書き出す.
  - validate: 診断結果を機械可読に返す.
  subcommand の名前と形式は未決である.
- schema は `loadopts` が使うのと同じ option 表から生成し, CI で検査される config reference と食い違わないようにする.

## 補足

schema があっても, どの option をどの配置と名前で見せるかは GUI が決める. RTKLIB の GUI も option 画面の各入力欄と option の対応を手書きしている. schema でなくすのは, 値の対応表, 既定値と検証の規則, file の読み書きの重複である.

## Alternatives considered

- **schema だけを MRTKLIB が提供し, TOML の読み書きと変換は各 frontend が行う.** MRTKLIB の追加は少ないが, 読み書きが frontend ごとに重複する. 採用しない.
- **各 frontend が実装する (現状の docker-ui).** P2 に反する. 採用しない.

## Consequences

- P4 の条件 (複数 frontend から実際に必要) を満たす: docker-ui と GUI suite の双方が使う.
- `mrtk run` の RPC と config 系 subcommand で, 同じ key の形 ([rtkrcv-control.md](../rtkrcv-control.md) D-10: TOML の木) と同じ schema を使う.
- 既存の TOML 保存の欠落 ([findings/rtkrcv-toml-save.md](../findings/rtkrcv-toml-save.md)) の修正が前提になる.
- post / convert 系 GUI (RTKPOST, RTKCONV 相当) の着手条件の 1 つが満たされる ([0007](0007-gui-suite-composition.md)). 残るのは進捗の扱い (U-05) である.
