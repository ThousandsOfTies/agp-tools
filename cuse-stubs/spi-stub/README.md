# cuse_spi — CUSE SPI stub (MFRC-522 sim)

LD_PRELOAD の `spi_shim.so` を置き換える、CUSE ベースの SPI スタブです。
本物の `/dev/spidev0.0` を userspace で生やし、未改変の `~/sensor_demo`
バイナリがそのまま MFRC-522（RFID リーダ）を読めるようにします。

`i2c-stub/cuse_i2c`（I2C）と同じ構造で、デバイス固有のレジスタ模擬は
`mfrc522_sim.c` に分離しています。カード提示状態は web bridge
（`GAR_HW_SIM_SOCK` または `GAR_RUNTIME_DIR/hw_sim.sock`）から取得するので、`gar sim ui rfid tap <UID>` でタップを
注入できます。

## なぜ CUSE か（spi_shim との違い）

`spi_shim.so` は `LD_PRELOAD` でアプリ内の `open()/ioctl()` を横取りする方式で、
アプリの起動コマンドに `LD_PRELOAD=...` を足す必要がありました。CUSE 版は
カーネルが spidev の ioctl を `/dev/fuse` 経由でこのプロセスに配送するため、
アプリ側は本番（RasPi5）と同じ `~/sensor_demo` 一発で済みます。差し替えの
責務をアプリから Gapless Agent Runtime runtime 側へ閉じ込めるのが狙いです。

## 対応 ioctl

| ioctl | 動作 |
|---|---|
| `SPI_IOC_RD/WR_MODE` | 受理（値を保持して読み返す） |
| `SPI_IOC_RD/WR_LSB_FIRST` | 受理 |
| `SPI_IOC_RD/WR_BITS_PER_WORD` | 受理 |
| `SPI_IOC_RD/WR_MAX_SPEED_HZ` | 受理 |
| `SPI_IOC_RD/WR_MODE32` | 受理 |
| `SPI_IOC_MESSAGE(N)` | MFRC-522 レジスタ模擬で応答 |

MFRC-522 の SPI は 2 バイト転送です。

```
tx[0]: アドレスバイト = (reg << 1) & 0x7E、MSB=1 READ / 0 WRITE
tx[1]: データ（書き込み時）/ 0x00（読み出し時、応答は rx[1]）
```

`VersionReg(0x37)` は `0x92`（MFRC-522 v2.0）を返します。REQA / ANTICOLL で
ATQA・UID を返し、UID は bridge から取得したカードのものを使います。

## ビルド

**ビルドは Codespaces で行う**（鉄則）。EC2/RasPi5 上では `make` しない —
それらは実行専用ターゲットで、将来はツールチェーンもシェルも無い前提。

```bash
# Codespaces: aarch64 cross-build（EC2 Graviton / RasPi5 と同じ ABI）
make CC=aarch64-linux-gnu-gcc

# x86_64（CI / Codespaces 内での構文確認用）
make
```

`libfuse3-dev`（`/usr/include/fuse3`）が必要。成果物 `cuse_spi` は
WSL 経由でターゲットへ deploy し、ターゲットでは**起動のみ**行う。

## 起動

```bash
# /dev/fuse へのアクセスが必要
sudo ./cuse_spi -f --devname=spidev0.0
```

`-f` は foreground 実行。バックグラウンド常駐は `gar sim start` に組み込みます
（`cuse_i2c` と同様）。起動後は `chmod 666 /dev/spidev0.0` でアプリから読める
ようにします。

## 動作確認の受け入れ基準

「`/dev/spidev0.0` が生えた」「ioctl が通った」だけでは done にしません。

1. `gar sim start` で `cuse_spi` が常駐し `/dev/spidev0.0` が見える
2. `~/sensor_demo`（LD_PRELOAD なし）が VersionReg=0x92 を読めて初期化成功
3. `gar sim rfid tap 04:AB:CD:EF:01:23` 後に `sensor_demo` が UID を表示する
4. `gar sim rfid remove` でカード無し挙動に戻る

ここまで確認できて初めて S4 完了です。
