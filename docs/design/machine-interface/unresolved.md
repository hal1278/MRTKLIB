# Unresolved

この文書は, 横断的な未決事項を記録する.
ここにある事項は確定していない. 他の文書で確定事項として扱わない.

`rtkrcv` の operation と state に関する判断事項 (D-1 – D-39) は [rtkrcv-control.md](rtkrcv-control.md) §9 が owner である. ここでは重複して書かない.

決着した項目は `decisions/` に記録し, ここから削除して decision へのリンクだけを残す.

## U-01. GUI repository

決定済み: [decisions/0006](decisions/0006-gui-egui-existing-repository.md) (当面 egui, 既存の `hal1278/mrtklib-egui`), [decisions/0017](decisions/0017-gui-repository-conventions.md) (license, 名前, 既存文書, 文書の分担).

## U-02. Windows 対応の残りの論点

主経路と進め方は [decisions/0004](decisions/0004-windows-native-platform-layer.md), 移植の段階, CI, 設計文書の時期は [decisions/0018](decisions/0018-windows-porting-plan.md) で決定済み. 経路の比較は [findings/windows-route-comparison.md](findings/windows-route-comparison.md), POSIX 依存の分布は [findings/windows-build-gap.md](findings/windows-build-gap.md) と [findings/windows-cross-build.md](findings/windows-cross-build.md) にある.

- **未確定:**
  - platform layer の境界 (内部 API の粒度, file 配置, public header が `pthread.h` / `sys/select.h` を include している点の扱い). Windows 対応の issue を出す前に設計文書の草案を作る (0018). process 起動は引数の配列で行い shell を介さない ([rtkrcv-control.md](rtkrcv-control.md) D-8). stdin の確認も含める ([decisions/0013](decisions/0013-finite-process-progress-and-cancel.md)).
  - upstream が platform layer 方式も受け入れない場合の扱い (issue への反応を見てから決める. 0018).
  - MSVC への対応の要否 (現時点では対象外. 0004).
  - Windows の native build で path の区切りをどう扱うか (2026-10-08 追加). MRTKLIB は区切りを `/` と決め打ちしており (`FILEPATHSEP '/'`, `include/mrtklib/mrtk_foundation.h:333`), path の分解は `\` を区切りと認識しない ([findings/windows-cross-build.md](findings/windows-cross-build.md)). `\` も区切りとして扱うか, `/` を規約とするかを platform layer の設計で決める. 設定 file の文字列の escape はこれと独立に常に行う ([rtkrcv-control.md](rtkrcv-control.md) D-11).
  - Windows 対応の issue を書くときは既存の issue との重複を避ける. 例: [h-shiono/MRTKLIB#301](https://github.com/h-shiono/MRTKLIB/issues/301) の監査は `src/core/mrtk_time.c:295` の `gmtime` (`gmtime_r` を使うべき) を既に挙げており, cross build で見つかった同じ行の型の不一致 ([findings/windows-cross-build.md](findings/windows-cross-build.md)) と関係する.

## U-03. `rtkrcv` の operation と state の意味

[rtkrcv-control.md](rtkrcv-control.md) §9 を参照.

## U-04. WebSocket / JSON library

- **確定した選定条件 (2026-10-07, hal1278):**
  - MRTKLIB の license (BSD 2-clause) と両立すること. GPL のみ, または GPL と商用の dual license の library は採用できない. #326 本文が挙げた Mongoose は GPLv2 と商用の dual license であり外れる.
  - Linux, macOS, Windows (MSYS2 UCRT64, [decisions/0004](decisions/0004-windows-native-platform-layer.md)) で build できること. 将来の MSVC 対応を妨げないこと.
  - token による認証と Origin header の検査ができること ([rtkrcv-control.md](rtkrcv-control.md) D-25). handshake の header を読める必要がある.
  - vendoring (MRTKLIB は `src/core/tomlc99` を取り込んでいる) か vcpkg で取り込めること.
- **評価の時期:** [decisions/0003](decisions/0003-work-order.md) の手順 5 (schema の草案と同時) で行う. license は一次情報で確認し, UCRT64 での cross build を実際に試す.
- **評価候補 (一般知識に基づく. 未確認):** WebSocket は CivetWeb, libwebsockets, 最小限の自前実装. JSON は cJSON, yyjson.
- **未確定:** library の選択.

## U-05. `mrtk post` / `mrtk convert` の進捗と中止

決定済み: [decisions/0013](decisions/0013-finite-process-progress-and-cancel.md) (server にせず機械可読な進捗 mode. 中止は stdin). 背景と比較は [findings/post-convert-progress.md](findings/post-convert-progress.md) にある.

## U-06. Configuration schema の提供

決定済み: [decisions/0011](decisions/0011-mrtklib-provides-config-handling.md) (U-17 と一体で決着). subcommand の名前と形式は未決.

## U-07. `mrtk relay` の統計

- **背景:** relay は stream の byte 数と bps を stderr に人間向けの行で出す. docker-ui はこれを解析せず log として表示している.
- **未確定:** STRSVR 相当の GUI のために machine-readable 化するか.
- **優先度:** STRSVR 相当は第一目標に含めない ([0007](decisions/0007-gui-suite-composition.md)). 第一目標の後で判断する.

## U-08. upstream に出す範囲と言語

決定済み: [decisions/0014](decisions/0014-upstream-scope-and-language.md) (契約部分を英語で PR. 経緯は fork).

## U-09. docker-ui の移行

決定済み (2026-10-07, hal1278): [decisions/0003](decisions/0003-work-order.md) の手順 8 のとおり, GUI の後にまとめて移行する.

- 退けた案: RPC の実装後に docker-ui 側で試験的に移行し, v1 の凍結前に 2 つ目の frontend で契約を検証する (検証は早まるが調整が 2 回になる). GUI より先に移行する (GUI の試作と競合する).
- 影響: 2 つ目の frontend での契約の検証は v1 の凍結後になる. docker-ui で見つかった不足は, 追加的な変更 (minor 版) か互換性のない変更 (major 版) として扱う (P8).
- docker-ui の upstream は `h-shiono/mrtklib-docker-ui` であり, 移行の時期と中身は docker-ui の maintainer と調整する.

## U-10. 作業管理

決定済み: [decisions/0015](decisions/0015-task-tracking.md) (当面は 0003, upstream issue, `tasks/todo.md`).

## U-11. 調査で見つかった既存の問題の扱い

報告済み (2026-10-08): TOML 保存の欠落は [h-shiono/MRTKLIB#342](https://github.com/h-shiono/MRTKLIB/issues/342), serial の書き込みの戻り値は [h-shiono/MRTKLIB#343](https://github.com/h-shiono/MRTKLIB/issues/343).

決定済み: [decisions/0016](decisions/0016-reporting-existing-issues.md). security の行は [decisions/0019](decisions/0019-console-risks-handled-publicly.md) で置き換え (非公開の報告はしない).

## U-12. 作業順序

決定済み: [decisions/0003](decisions/0003-work-order.md).

## U-13. GUI suite の構成

決定済み: [decisions/0007](decisions/0007-gui-suite-composition.md).

## U-14 – U-18 の関係

2026-10-07 に hal1278 が指摘したとおり, 次の 5 つは互いに独立した問題である. 以前の U-14 (configuration の所有モデル) はこれらを 1 つに混ぜていたため分割した.

| ID | 問題 | 問い |
|---|---|---|
| U-14 | GUI と MRTKLIB の結合方式 | 静的 link (process 内) か, 別 process (RPC / argv) か |
| U-15 | backend process の配置 | GUI ごとに専用の process を起動するか, system で 1 つの常駐 process を共有するか |
| U-16 | 配布形態 | `mrtk` を GUI に同梱するか. 実行 file 単体で起動できるか |
| U-17 | 設定処理の担い手 | 設定の parse, 検証, 書き出し, schema を MRTKLIB が提供するか, frontend ごとに実装するか |
| U-18 | 設定の永続化と共有 | GUI の設定をどこに保存し, CLI と設定 file を共有するか |

RTKLIB での対応 (2026-10-07, RTKLIB 2.4.3 の `app/winapp` で確認):

- 結合: RTKPOST の project は RTKLIB の `src/*.c` を GUI に直接 compile して組み込む (`rtkpost/rtkpost.cbproj`). process 内の結合である.
- 永続化: GUI の設定は `.ini` に保存する (`rtkpost/postmain.cpp:115`, `:1170`). CLI とは共有しない.
- 共有: option 画面の Load / Save で, CLI と同じ形式の option file を明示的に読み書きできる (`rtkpost/postopt.cpp:602`, `:866`, `rtknavi/naviopt.cpp:661`, `:1020`). 共有は user が選ぶ import / export である.

## U-14. GUI と MRTKLIB の結合方式

決定済み: [decisions/0008](decisions/0008-gui-backend-coupling.md) (現時点では別 process. 将来の単一 file 化の余地を残す).

## U-15. backend process の配置

決定済み: [decisions/0009](decisions/0009-backend-process-per-gui.md) (GUI ごとに専用. 共有の常駐型は第一目標外).

## U-16. 配布形態

決定済み: [decisions/0010](decisions/0010-bundle-mrtk-with-gui.md) (同梱. 明示指定があれば優先. PATH は既定で使わない).

## U-17. 設定処理の担い手

決定済み: [decisions/0011](decisions/0011-mrtklib-provides-config-handling.md) (MRTKLIB が RPC と config 系 subcommand で提供する).

## U-18. 設定の永続化と共有

決定済み: [decisions/0012](decisions/0012-config-persistence-like-rtklib.md) (RTKLIB と同じ形).

## 範囲外の発案

この取り組みの範囲外だが, 検討の過程で出た発案を記録する. 着手する場合は別の issue として扱う.

## U-19. 測位 engine の warm start

- **発案 (2026-10-07, hal1278):** 停止時の推定状態を次の開始に使い, 収束を早める.
- **整理:** 位置, 受信機時計, 対流圏, bias などを初期値に使えば float 解の収束を短縮できる見込みがある. ambiguity の再利用は, 停止中に搬送波位相の追尾が途切れたかを処理側で確かめられないため危険である. 位置の流用は停止中に antenna が動いていない場合に限られる. 数値の挙動を変える engine の機能であり, MRTKLIB の規則では精度の比較が要る.
- **この取り組みとの関係:** [rtkrcv-control.md](rtkrcv-control.md) D-30 の snapshot は表示用で, 共分散や ambiguity を含まないため使えない.
- **扱い (2026-10-07, hal1278):** 記録のみとし, 第一目標の後に検討する. upstream に engine の機能要望として issue を起こすかはその時に決める.

