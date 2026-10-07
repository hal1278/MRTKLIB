# 0007. 第一目標の GUI suite の構成

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)

## Context

RTKLIB の GUI は機能ごとの application 群 (RTKNAVI, RTKPOST, RTKCONV, STRSVR, RTKPLOT, RTKGET, NTRIP browser) と launcher (RTKLAUNCH) から成る.
[principles.md](../principles.md) P1 により, GUI の分割と MRTKLIB 側の interface の分割は一致させない.
各 application と MRTKLIB 側の対応は [unresolved.md](../unresolved.md) U-13 の表 (この decision で決着) にあった.

## Decision

- 第一目標で作る application は次の 4 つとする.

  | Application | MRTKLIB 側 | 新しい machine interface |
  |---|---|---|
  | RTKNAVI 相当 | `mrtk run` + RPC | 必要 (#326, [rtkrcv-control.md](../rtkrcv-control.md)) |
  | RTKPOST 相当 | `mrtk post` (終了する process) | 進捗の扱いを U-05 で決める |
  | RTKCONV 相当 | `mrtk convert` (終了する process) | 進捗の扱いを U-05 で決める |
  | RTKPLOT 相当 | solution file (定義済み形式, P3) | 不要 |

- STRSVR, RTKGET, NTRIP browser 相当と launcher は第一目標に含めない.
- application は原則として別の実行 file とし, cargo workspace で RPC client, 設定の扱い, process 管理などを共通 crate に置く. launcher は作らない.

## Alternatives considered

- **RTKNAVI 相当のみ.** RPC の最初の consumer としては十分だが, post / convert 系の machine interface の要否を確かめられない. 採用しない.
- **主要 application すべて (STRSVR, launcher を含む).** relay の統計 (U-07) なども前提になり範囲が広がる. 採用しない.
- **1 つの実行 file で複数 window, または tab で切り替え.** RTKLIB の構成から離れる. egui の複数 window 対応も未検証である. 採用しない.

## Consequences

- RTKPOST / RTKCONV 相当は P5 (human-readable output を解析しない) に従う必要があるため, U-05 (進捗) と U-06 (configuration schema) の判断が第一目標の前提になる.
- RTKPLOT 相当は solution file を読む. GUI 側で solution file の parser を持つことになる (P3 で許容).
- U-07 (relay の統計) は第一目標の範囲外になる.
- [0003](0003-work-order.md) の手順 7 のとおり, RTKNAVI 相当を先に作り, post / convert 系は U-05 / U-06 の判断後に着手する.
