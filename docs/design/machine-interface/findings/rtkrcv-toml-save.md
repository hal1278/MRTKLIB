# `rtkrcv` の `save` で TOML に保存すると測位 option が欠落する

- **対象:** `h-shiono/MRTKLIB` `develop` `8dc1fc6`
- **調査日:** 2026-10-07
- **再現:** 実機で確認済み
- **報告:** upstream に [h-shiono/MRTKLIB#342](https://github.com/h-shiono/MRTKLIB/issues/342) として報告した (2026-10-08)

## 事実

console command `save <file>` は 2 回に分けて書き込む (`apps/rtkrcv/rtkrcv.c:1689-1710`, 書き込みは `:1705`).

1. `saveopts(file, "w", comment, rcvopts)`: `rtkrcv` 固有の option (console, stream, misc, file-cmdfile)
2. `saveopts(file, "a", NULL, sysopts)`: 測位 option (positioning, ambiguity resolution, Kalman filter など)

`saveopts()` は拡張子が `.toml` のとき append mode を処理せず, 成功として返す (`src/pos/mrtk_options.c:518-525`).

```c
if (mode && strcmp(mode, "a") == 0) {
    trace(NULL, 2, "saveopts: TOML append not supported, skipping (%s)\n", file);
    return 1; /* not an error — caller likely already wrote with "w" */
}
```

このため 2 回目の書き込みは行われず, console には `options saved to <file>` と表示される.

## 再現手順と結果

`conf/claslib/rtkrcv.toml` を読み込ませて起動し, remote console から `save` を実行した.

```text
mrtk run -o rtkrcv.toml -p <port> -w admin
rtkrcv> save saved.toml
rtkrcv> save saved.conf
```

| file | section 数 | key 数 | 測位 option |
|---|---|---|---|
| 入力 `rtkrcv.toml` | 34 | 160 | あり |
| `saved.toml` | 11 | 42 | なし (`[server]`, `[streams.*]`, `[console]` と `[files]` の `cmd_file_1`–`cmd_file_3` のみ) |
| `saved.conf` (legacy) | — | 242 | あり (`pos1-posmode`, `pos2-armode` など) |

最初の再現 (2026-10-07) の build は, `8dc1fc6` に本件と無関係な未 commit の変更 (`src/data/rcv/mrtk_rcv_ublox.c` 1 行) を含む tree から行った.

key 数は Python の `tomllib` で末端の key を数えた (2026-10-08 に 156 から訂正). 失われるのは測位 option だけでなく `sysopts` 全体であり, 解の出力 (`[output]`), antenna (`[antenna.*]`), 補助 file (`[files]` の `satellite_atx` など) も含む. 2026-10-08 に変更のない 8dc1fc6 の tree で再現し直し, 同じ結果を得た. 保存した file から起動し直すと `pos1-posmode = single`, `pos2-armode = continuous` になる (入力は `ppp-rtk`, `fix-and-hold`).

## 付随して観測した事実

- `-w <pwd>` で指定した password は, configuration file に `console-passwd` があると上書きされる. command line の解析 (`rtkrcv.c:2114-2145`, `-w` は `:2128`) の後に `loadopts(file, rcvopts)` (`rtkrcv.c:2185`) が実行されるためである. 上の再現では `-w admin` を与えたが, 使用した `conf/claslib/rtkrcv.toml` が `[console] passwd = ""` を含むため, 保存結果の `console-passwd` は空だった.
- `saveopts_toml()` は渡された option table を走査して書き出す. 読み込んだ file にあった未知の key や comment は保存されない.

## 設計への影響

- #326 の `saveConfig` は既存の `save` の実装をそのまま使えない. 1 つの TOML document として rcvopts と sysopts を同時に書き出す経路が必要である.
- `getConfig` / `saveConfig` が返す・保存する configuration の範囲 (未知 key, comment, credential を含むか) を [rtkrcv-control.md](../rtkrcv-control.md) で決める必要がある.
- **Inference:** この欠落は machine interface とは独立した既存の bug であり, upstream に別 issue として報告する価値がある.
