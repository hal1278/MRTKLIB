# docker-ui の MRTKLIB 利用方法

- **対象:** `hal1278/mrtklib-docker-ui` `develop` `4fcaf45`
- **調査日:** 2026-10-07
- **Path:** 特記なき限り `src/mrtklib_web_ui/` からの相対 path

## 要約

MRTKLIB から machine-readable な出力を受け取っている箇所はない.
docker-ui が使っているのは argv, 手書きの TOML, human-readable な stdout / stderr / telnet 表示, exit code, `.pos` file, receiver の raw SBF stream である.

MRTKLIB は `docker/Dockerfile:25-26` で `ARG MRTKLIB_BRANCH=main` を `git clone` して build している.
`mrtk` の path `/usr/local/bin/mrtk` は 6 箇所に hard-code されている.

## Real-time (`mrtk run`) — `services/mrtk_run_service.py`

| 項目 | 方法 | 箇所 |
|---|---|---|
| 起動 | `mrtk run -s -o <toml> -p <port> -w ""` | `:343-351` |
| console port | port 0 に bind して得た番号を close 後に渡す (取得から使用までに競合しうる) | `:330-335` |
| 起動待ち | 固定 5 秒 sleep の後に telnet 接続 | `:369` |
| login | welcome に `password` が含まれれば `admin` を送る (`-w ""` を渡しているにもかかわらず) | `:392-397` |
| command 送受信 | prompt `rtkrcv>` / `mrtk>` が現れるまで読む | `_send_command` `:475`, `:511` |
| 状態取得 | 1 秒ごとに `status`, `satellite`, `observ` を送り解析 | `_poll_status` `:573` |
| 停止 | telnet `shutdown`, 次に SIGTERM, 最後に kill | `:431-453` |

`status` の解析 (`parse_status_output` `:148`) は次の label 文字列に依存する (`:23-43`).

- `rtk server state`
- `solution status`
- `pos llh single (deg,m) rover` (正規表現 `pos llh.*rover`)
- `# of valid satellites`
- `ratio for ar validation`
- `age of differential (s)`
- `time of receiver clock rover`

`satellite` の解析 (`parse_satellite_output` `:57`) は列位置 (PRN, `OK`, Az, El, FIX/FLOAT/HOLD) に依存する.
SNR は `satellite` 表示に含まれないため, `observ` の表示から推定している (`parse_observ_snr` `:103`).

## Post-processing (`mrtk post`) — `services/mrtk_post_service.py`

- configuration は Pydantic model から f-string で TOML を生成する (`generate_conf_file`). GUI が知らない key は保持されない.
- 進捗は stderr の行を `_PROGRESS_PATTERN` (`:33`) で解析し, epoch / Q / ns / ratio を得る.
- 結果の `.pos` file は browser 側で列 offset により解析する (`frontend/src/components/viewer/posParser.ts`).

## Conversion (`mrtk convert`) — `api/convert.py`

- argv だけで実行する.
- `scanning:` で始まる行, または `": O="`, `": E="`, `": S="` を含む行を進捗とみなす (`:191`).

## Relay (`mrtk relay`) と CLAS pipeline

- relay の `-in` / `-out` URI は browser 側で組み立て, backend は argv をそのまま渡す.
- relay の統計行 (bytes / bps) は解析せず log として表示するだけである.
- CLAS pipeline の状態は MRTKLIB の出力ではなく, backend が relay の TCP server に接続して SBF PVTGeodetic block を自前で decode して得ている (`services/sbf_pvt_sniffer.py`).

## 別 frontend が同じ方法を取る場合に再実装が必要な処理

1. post / run 用 TOML の生成 (値の対応表と `[streams.*]` section を含む) と legacy `.conf` からの変換
2. `mrtk` の起動, stdout / stderr の行分割, throttle, credential masking
3. post の進捗正規表現と convert の進捗判定
4. `rtkrcv` console: port 確保, 起動待ち, password 処理, prompt 区切りの読み取り, `status` / `satellite` / `observ` の 1 秒 polling と上記の正規表現・列位置解析
5. 停止手順 (`shutdown`, SIGTERM, kill) と終了検出
6. relay URI の組み立て, CLAS relay → `cssr2rtcm3` の起動順序と監視, SBF PVT の decode
7. `.pos` file の列 offset 解析

**Inference:** 1, 3, 4 は MRTKLIB 固有の意味の解釈を含み, [principles.md](../principles.md) P2 / P5 の対象である. 7 は定義済み file format の読み取りであり P3 で許容される範囲にある.
