# 0012. 設定の永続化と共有は RTKLIB と同じ形にする

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)
- **Resolves:** [unresolved.md](../unresolved.md) U-18

## Context

RTKLIB の GUI は自分の設定を `.ini` に保存し, CLI とは共有しない. option 画面の Load / Save で CLI と同じ形式の option file を明示的に読み書きできる (RTKLIB 2.4.3 の `app/winapp` で確認. [unresolved.md](../unresolved.md) の U-14 – U-18 の関係を参照).
設定処理は MRTKLIB が提供する ([0011](0011-mrtklib-provides-config-handling.md)).

## Decision

- GUI 自身の状態 (window, 最近使った file, 各 application が最後に使った設定) は GUI 専用の保存先に置く.
- 測位の設定は MRTKLIB 形式 (TOML) の file として Load / Save できる. CLI と共有するかは user が選ぶ.
- GUI が最後に使った測位の設定も, GUI 専用の保存先に TOML として置く. 読み書きは MRTKLIB の設定処理で行い, GUI は設定の意味を知らない.

## Alternatives considered

- **CLI と同じ TOML file を直接編集する.** 手書きの comment や未知の key を残す部分的な書き換えが要る. 現在の TOML library は読み取り専用である. 採用しない.
- **GUI 独自の形式だけで保存する.** CLI と設定をやり取りできない. 採用しない.

## Consequences

- saveConfig ([rtkrcv-control.md](../rtkrcv-control.md) D-11) は export の意味になり, 全 key を揃えた canonical な書き出しで足りる.
- GUI 専用の保存先の具体的な path は platform ごとに GUI repository で決める (`mrtklib-egui` の既存文書の logical root の考え方を参照してよい).
