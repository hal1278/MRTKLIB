# 外部 command の起動

- **対象:** `h-shiono/MRTKLIB` `develop` `8dc1fc6`. 比較として RTKLIB 2.4.3 b34 (`v2.4.3-b34`)
- **調査日:** 2026-10-08
- **方法:** code reading. 実行はしていない.

## 事実

- `execcmd` は受け取った文字列を `system()` に渡すだけである (`src/core/mrtk_sys.c:83-86`).
- ftp / http の入力 stream は, 1 本の shell の文字列を組み立てて `execcmd` で実行する (`src/stream/mrtk_stream.c:2775-2792`).
  - 使う command は `wget` で, timeout は 30 秒である (`:74-75`).
  - proxy が設定されていれば, 先頭に `set ftp_proxy=http://<addr> & ` (http なら `http_proxy`) を付ける (`:2777`). 値は `misc-proxyaddr` である (`apps/rtkrcv/rtkrcv.c:282`).
  - ftp では `--ftp-user=<user> --ftp-password=<password>` を wget の引数に入れる (`:2783`).
  - 出力 file は `-O "<local>"`, URL は `"ftp://..."` / `"http://..."`, stderr は `2> "<errfile>"` で指定する (`:2784-2789`).
- 圧縮された入力 file の展開も `execcmd` を通る (`src/core/mrtk_sys.c`).
  - `gzip -f -d -c "<in>" > "<out>"` (`:383`)
  - `tar -C "<dir>" -xf "<in>"` (`:403`)
  - `crx2rnx < "<in>" > "<out>"` (`:420`)
- `misc-startcmd` と `misc-stopcmd` は start の前と stop の後に `system()` で実行される (`apps/rtkrcv/rtkrcv.c:625`, `:701`).
- RTKLIB 2.4.3 b34 の Windows 版の `execcmd` は, `cmd /c <文字列>` を `CreateProcess` に渡す (`src/rtkcmn.c:3238-3262`, `cmd /c` は `:3249`). Windows でも command は cmd.exe を通る.

## Inference

- **Inference:** `set <name>=<value> & ` は cmd.exe の書き方である. Linux / macOS の `/bin/sh` では `set` が background の subshell の位置 parameter を変えるだけで, wget の環境変数にならない. このため `misc-proxyaddr` は Linux / macOS の ftp / http の入力 stream に効いていない可能性が高い. 実行での確認はしていない.
- **Inference:** ftp の user と password は download の間 wget の引数にあり, Linux / macOS では同じ PC の他の user から `ps` などで見える (一般知識による. 未確認). NTRIP の password は wget に渡されず, stream 層が process の中で扱う.
- **Inference:** shell を介さずに同じことをするには, 引数の配列だけでなく, 環境変数の追加, stdin / stdout / stderr の file への付け替え, 終了 code の受け取りを起動の部品が引き受ける必要がある.
