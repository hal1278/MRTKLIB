# Principles

この文書は, machine interface の設計と実装で用いる判断基準を定義する.
個々の operation や schema は定義しない. それらは owner 文書 (例: [`rtkrcv-control.md`](rtkrcv-control.md)) で扱う.

各原則には出典と状態を付ける.
状態が `Proposed` の原則は判断材料として使ってよいが, 確定事項として扱わない.

| ID | 出典 | 状態 |
|---|---|---|
| P1–P4 | [#327](https://github.com/h-shiono/MRTKLIB/issues/327) Decision needed | Proposed upstream (`status:confirmed` label) |
| P5–P6 | [decisions/0002](decisions/0002-first-milestone-scope.md) | Accepted (fork) |
| P7–P8 | [#326](https://github.com/h-shiono/MRTKLIB/issues/326) 本文 | Proposed upstream (`status:needs-triage`) |

## P1. Frontend の application 構成と backend 境界を一対一に対応させない

GUI application の分割をそのまま MRTKLIB 側の process や interface の分割にしない.
例えば real-time positioning と post-processing を別 application から使う frontend と, 同じ application から使う frontend の双方を許容する.

## P2. MRTKLIB 固有の control / runtime state handling を frontend ごとに実装しない

start / stop, configuration, runtime state の取得など, MRTKLIB 側で意味が決まる処理は MRTKLIB 側で共通化する.
text formatting, serialization, transport は各 external interface の責務とする.

## P3. 既存の execution model と machine-readable interface が十分なら維持する

終了する処理 (`mrtk post`, `mrtk convert` など) は独立 process として実行し, exit status と生成物を使ってよい.
定義済みの file format (TOML configuration, solution file, RINEX など) の読み書きは frontend が行ってよい.

## P4. 共通 framework を先に定義せず, 実際に複数 frontend から必要になった範囲から具体化する

`rtkrcv` の command layer を MRTKLIB 全体向けの汎用 service framework として先に設計しない.

## P5. Frontend は MRTKLIB の human-readable output を解析しない

telnet console の表示, stderr の進捗行など, 人間向けの出力を frontend が解析して状態を得る構成にしない.
frontend が必要とする情報が machine-readable に得られない場合, それは MRTKLIB 側の machine interface の候補であり, P3 と P4 に従って個別に判断する.

P3 で許容する定義済み file format と exit status の利用は, この原則の対象外である.

## P6. Machine interface とその実装は cross platform を前提に選ぶ

RTKLIB 型 GUI の主な対象は従来の RTKLIB user であり, Windows 対応を可能な限り達成する.
protocol, library, thread / socket の扱いは Linux, macOS, Windows の native build で使えるものを選ぶ.
POSIX 専用の実装の上に新しい interface を重ねない.

## P7. telnet console は compatibility interface として凍結する

既存の script や手動確認のために telnet console を維持するが, 新しい command と表示形式の変更を加えない.
新しい機能は machine interface だけに追加する.

## P8. Machine interface は versioned contract とする

protocol は version を持ち, 追加的な変更は minor version, 互換性のない変更は major version を上げる.
schema は MRTKLIB repository に置き, frontend repository は対応する major version を固定する.
frontend は MRTKLIB の内部構造体や ABI に依存しない.
