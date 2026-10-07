# Findings

設計判断の根拠となった調査結果を記録する.

## 規則

- 各 file の冒頭に, 観測対象の repository と commit hash, 調査日を書く.
- 事実には `file:line` を付ける. line 番号は記載した commit におけるものである.
- 推論は `**Inference:**` と明記して事実と分ける.
- 調査後に対象 code が変わっても書き換えない. 必要なら新しい finding を追加し, 古い方から参照する.
- 公開 issue にすべきでない security 上の問題は, upstream の `SECURITY.md` に従い private に報告し, ここには設計上必要な中立的事実だけを書く.

## 一覧

| File | 内容 | 対象 |
|---|---|---|
| [docker-ui-integration.md](docker-ui-integration.md) | `mrtklib-docker-ui` が MRTKLIB を使う方法と, 別 frontend が再実装を強いられる処理 | `mrtklib-docker-ui` `4fcaf45` |
| [rtkrcv-console.md](rtkrcv-console.md) | `rtkrcv` telnet console の構造, command 一覧, command layer 抽出時の論点 | MRTKLIB `8dc1fc6` |
| [rtkrcv-toml-save.md](rtkrcv-toml-save.md) | `save` command で TOML に保存すると測位 option が欠落する | MRTKLIB `8dc1fc6` |
| [rtksvr-runtime.md](rtksvr-runtime.md) | `rtksvr` の state, lock, stream state, event 発生源 | MRTKLIB `8dc1fc6` |
| [windows-build-gap.md](windows-build-gap.md) | `mrtk` の Windows native build を阻む POSIX 依存 | MRTKLIB `8dc1fc6` |
| [windows-route-comparison.md](windows-route-comparison.md) | Windows 対応の経路の比較と Codex のレビュー | 一般知識, Codex, RTKLIB 2.4.3 |
| [windows-cross-build.md](windows-cross-build.md) | MinGW-w64 (UCRT) cross build で実際に失敗する箇所 | MRTKLIB `8dc1fc6` |
| [post-convert-progress.md](post-convert-progress.md) | `mrtk post` / `mrtk convert` の進捗と中止の現状 | MRTKLIB `8dc1fc6` |
