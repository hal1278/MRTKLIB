# 0003. 作業順序

- **Status:** Accepted
- **Date:** 2026-10-07
- **Decided by:** hal1278 (作業 session での回答)

## Context

第一目標 ([0002](0002-first-milestone-scope.md)) に至る作業の順序が決まっていなかった.
GUI suite の役割は machine interface が実用上十分であることを確かめることなので, schema を凍結する前に GUI 側から不足を見つけられる方が手戻りが少ない.

## Decision

次の順で進める.

1. [rtkrcv-control.md](../rtkrcv-control.md) の判断事項を fork の案として固め, #326 に提示する.
2. 並行して, Windows 対応について upstream に相談し (U-02), GUI の実装先と構成を決める (U-01, U-13).
3. `rtkrcv` telnet 出力の characterization test を追加する.
4. command layer を抽出する (telnet の挙動は変えない). TOML 保存の欠落を直す.
5. `docs/rpc/` の schema 草案を書き, WebSocket / JSON library を選ぶ (U-04).
6. RPC を実装し, contract test を追加する.
7. GUI suite の RTKNAVI 相当を実装する. **5 の schema 草案ができた時点で, schema に従う mock server を相手に GUI の試作を始め, 6 と並行させる.** GUI 側で見つかった不足は schema の v1 凍結前に反映する. post / convert 系は U-05 / U-06 の判断後に着手する.
8. docker-ui を machine interface に移行する (U-09).

Windows の移植は U-02 の合意後に, 上と並行して進める.

## Alternatives considered

- **直列に進める (GUI は RPC 実装と contract test の後).** 単純だが, GUI で見つかった不足を schema の改版で吸収することになる. 採用しない.
- **Windows 移植を最優先にする.** RTKLIB user 向けの価値は早く出るが, upstream の方針変更 (U-02) を待つ間 #326 の作業が止まる. 採用しない.

## Consequences

- mock server を schema とともに保守する必要がある. mock は schema から生成するか, contract test と同じ定義を共有して, 実装との乖離を防ぐ.
- GUI の試作には実装先 (U-01) の決定が先に必要である.
