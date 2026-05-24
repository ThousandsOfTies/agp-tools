# シミュレーション・セットアップ

このドキュメントでは、クラウド上（AWS EC2 Graviton）で物理ハードウェアをエミュレートする仕組みについて解説します。

## 全体構成

```
[EC2 arm64 (Graviton)]

  sensor_demo (アプリケーション)
    │
    │  GPIO: /dev/gpiochip0 (ioctl)
    │  ──→ gpio_shim.so (LD_PRELOAD で intercept)
    │       └─ Unix socket ──→ bridge.py
    │
    │  I2C:  /dev/i2c-1 (read/write/ioctl)
    │  ──→ cuse_i2c (CUSE で /dev/i2c-1 を生成)
    │       └─ SSD1306 0x3C: ssd1306_sim → bridge.py
    │       └─ VL53L0X 0x29: vl53l0x_sim (内部状態)
    │
    │  SPI:  /dev/spidev0.0 (SPI_IOC_MESSAGE)
    │  ──→ spi_shim.so (LD_PRELOAD で intercept)
    │       └─ MFRC-522 register sim → bridge.py
    │
    └─ bridge.py
         ├─ Unix socket /tmp/hw_sim.sock (シム/スタブ ⇔ bridge)
         ├─ WebSocket  ws://0.0.0.0:8765 (bridge ⇔ panel)
         └─ HTTP       http://0.0.0.0:8080 (panel HTML/CSS/JS 配信)
                ↓
          [Antigravity Simple Browser]
            Virtual Hardware Panel
              - LED GPIO18 / GPIO24 (canvas)
              - Button GPIO17 / GPIO27
              - VL53L0X range slider
              - MFRC-522 Tap Card / Remove
              - SSD1306 OLED 128×64 canvas
```

---

## 起動手順

Codespaces で `make cross && make deploy-ec2 EC2=vibecode-graviton` 済みの前提です。

### ターミナル 1: ウェブブリッジ起動

```bash
ssh vibecode-graviton
~/venv/bin/python3 ~/web-bridge/bridge.py
# → [bridge] Unix socket listening: /tmp/hw_sim.sock
# → [bridge] WebSocket  ws://0.0.0.0:8765
# → [bridge] HTTP panel http://0.0.0.0:8080
```

### ターミナル 2: I2C CUSE スタブ起動

```bash
ssh vibecode-graviton
sudo ~/cuse_i2c -f --devname=i2c-1
# → [vl53l0x_sim] initialized, range=300mm
# → [cuse_i2c] starting /dev/i2c-1 stub

# 別ターミナルから一度だけ
sudo chmod 666 /dev/i2c-1
```

### ターミナル 3: sensor_demo 起動（シム経由）

```bash
ssh vibecode-graviton
LD_PRELOAD="$HOME/gpio_shim.so $HOME/spi_shim.so" ~/sensor_demo
# → [gpio_shim] loaded, bridge=/tmp/hw_sim.sock
# → [spi_shim] loaded (MFRC-522 sim)
# → Sensor Demo started. Press Ctrl+C to quit.
```

---

## バックグラウンド実行（一括起動）

3 プロセスを背景で起動する場合：

```bash
ssh vibecode-graviton 'setsid bash -c "nohup ~/venv/bin/python3 ~/web-bridge/bridge.py > /tmp/bridge.log 2>&1 &" < /dev/null'
sleep 2
ssh vibecode-graviton 'setsid bash -c "sudo nohup ~/cuse_i2c -f --devname=i2c-1 > /tmp/cuse.log 2>&1 &" < /dev/null'
sleep 3
ssh vibecode-graviton 'sudo chmod 666 /dev/i2c-1; setsid bash -c "LD_PRELOAD=\"$HOME/gpio_shim.so $HOME/spi_shim.so\" nohup ~/sensor_demo > /tmp/sensor.log 2>&1 &" < /dev/null'
```

ログ確認:
```bash
ssh vibecode-graviton 'tail -f /tmp/sensor.log'
```

---

## ブラウザパネルへのアクセス

Antigravity から EC2 に Remote SSH 接続している場合、ポートは自動的にフォワードされます。

1. **Open Folder → `/home/ubuntu/AgentCockpit`** を開く（`.vscode/settings.json` の自動転送設定が有効化される）
2. **PORTS タブ**で `8080` の行を右クリック → "Open in Simple Browser"
3. HTML パネルが開き、各デバイスの状態がリアルタイム表示される

> 自動検出されない場合は手動で `8080` と `8765` を Add Port してください。

---

## 操作と確認

| 操作 | パネル表示 / 期待される挙動 |
|---|---|
| `BTN GPIO17` PUSH | LED GPIO18 がトグル / OLED の `System: ON/OFF` 切替 |
| `Range` スライダ | VL53L0X 距離値が変動（vl53l0x_read で確認可） |
| `Tap Card` | OLED に `Last UID: 04:AB:CD:EF` 表示 / LED GPIO24 がフラッシュ / Scans カウンタ増加 |
| `Remove` | カード未検出に戻る |

---

## トラブルシューティング

| 症状 | 原因 | 対処 |
|------|------|------|
| `bridge not available` | bridge.py が未起動 | ターミナル1 を確認 |
| `/dev/fuse: Permission denied` | sudo なしで CUSE 起動 | `sudo` で起動 |
| sensor_demo が `/dev/gpiochip0: No such file` | gpio_shim 未ロード | `LD_PRELOAD` を確認 |
| `Tap Card しても OLED に UID 出ない` | spi_shim が bridge レスポンスをパース失敗 | バージョン要確認（whitespace 対応済み） |
| パネルが Disconnected のまま | ポート 8765 未転送 | PORTS タブで 8765 を Add Port |
| OLED に表示が出ない | I2C アドレス 0x3C 未認識 | `i2cdetect -y 1` で 0x3C があるか確認 |
| `Last UID` が更新されない | system_on が OFF | パネルの GPIO17 PUSH で ON に切替 |
