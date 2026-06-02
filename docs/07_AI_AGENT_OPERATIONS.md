# AgentCockpit 操作ガイド

> このドキュメントは `agp-tools` リポジトリ単体での作業用抹です。最新・正本は上流の **AgentCockpit リポジトリ `docs/07_AI_AGENT_OPERATIONS.md`** を参照してください。

AgentCockpit の狙いは、開発者が手順を覚えて操作する環境ではなく、**VSCode 上で AI エージェントがビルド、デプロイ、実行、仮想 H/W 操作、ログ確認まで担える環境**にすることです。

そのために、EC2 simulation runtime は AgentCockpit 側の `agp sim`、ブラウザの Virtual Hardware Panel と同じ操作は AgentCockpit 側の HTTP API / Make ターゲットから実行します。

人間は「何をしたいか」を指示し、AI は WSL hub 側の AgentCockpit から `agp` コマンド / Make ターゲット / HTTP API / ログを使って最後まで進めます。

---

## 基本方針

| 人間向け | AI エージェント向け |
|---|---|
| Web Panel で LED / ボタン / RFID / OLED を見る | `make panel-*` で仮想 H/W を操作する |
| SSH でログを見る | AgentCockpit の `agp sim log` / `agp sim diag` で観察する |
| 手順書を読みながら実行する | AgentCockpit の `agp` コマンドと Make ターゲットを組み合わせて実行する |

AIに任せたい操作は、なるべく「短く、明示的で、再実行しやすいコマンド」にします。

---

## EC2 シミュレータ操作

```bash
# Codespace build VM でビルド
# target software ごとの README / build script に従ってビルド

# 成果物は WSL hub 経由で EC2 へ転送

# EC2 上で bridge / CUSE / sensor_demo を起動
agp sim start

# ログ確認
agp sim log

# プロセス、デバイス、API状態をまとめて確認
agp sim diag

# 代表シナリオを一括実行
make sim-test EC2=vibecode-graviton

# JSON シナリオを実行
make sim-scenario EC2=vibecode-graviton SCENARIO=scenarios/sensor_demo_rfid.json

# 停止
agp sim stop
```

---

## 仮想 H/W 操作

```bash
# GPIO17 ボタンを短押し
make panel-button EC2=vibecode-graviton LINE=17 DURATION_MS=150

# RFID カードをタップ
make panel-rfid EC2=vibecode-graviton UID=04:AB:CD:EF:01:23

# RFID カードを外す
make panel-rfid-remove EC2=vibecode-graviton

# VL53L0X 距離センサー値を変更
make panel-range EC2=vibecode-graviton RANGE_MM=450

# bridge の共有状態を確認
agp sim status
```

---

## bridge HTTP API

`bridge.py` は Web Panel 用の WebSocket と、AI/CI 用の HTTP API を持ちます。入口は分かれていますが、状態変更は同じ仮想 H/W 操作関数を通るため、Panel と自動試験の挙動がずれにくい構成です。

| Endpoint | Method | 用途 |
|---|---|---|
| `/api/state` | GET | 現在の仮想 H/W 状態を取得 |
| `/api/button` | POST | GPIO ボタン状態を直接セット |
| `/api/button/press` | POST | GPIO ボタンを押して離す |
| `/api/rfid/tap` | POST | RFID カードを置く |
| `/api/rfid/remove` | POST | RFID カードを外す |
| `/api/range` | POST | VL53L0X の距離値をセット |

例:

```bash
curl -s -X POST "http://127.0.0.1:8080/api/button/press?line=17&duration_ms=150"
curl -s -X POST "http://127.0.0.1:8080/api/rfid/tap?uid=04:AB:CD:EF:01:23"
curl -s http://127.0.0.1:8080/api/state
```

---

## シナリオ自動試験

仮想 H/W 操作は JSON シナリオとして定義できます。AI Agent や CI は、同じシナリオを繰り返し実行することで、手順の再現性を確保できます。

```bash
make sim-scenario EC2=vibecode-graviton SCENARIO=scenarios/sensor_demo_rfid.json
```

シナリオ例:

```json
{
  "name": "sensor_demo system-on rfid flow",
  "steps": [
    { "action": "button_press", "line": 17, "duration_ms": 150 },
    { "action": "wait", "seconds": 0.5 },
    { "action": "rfid_tap", "uid": "04:AB:CD:EF:01:23" },
    { "action": "expect", "path": "spi.mfrc522.present", "equals": true }
  ]
}
```

対応アクション:

| action | 用途 |
|---|---|
| `button_press` | GPIOボタンを押して離す |
| `button_set` | GPIOボタン状態を直接セット |
| `rfid_tap` | RFIDカードを置く |
| `rfid_remove` | RFIDカードを外す |
| `range_set` | VL53L0X の距離値をセット |
| `wait` | 指定秒数待つ |
| `expect` | `/api/state` の値を検証する |

---

## AI に依頼するタスク例

```text
sensor_demo を EC2 にデプロイして、シミュレータを起動し、GPIO17 を押して System ON にしてから RFID をタップしてください。OLED とログを確認して、UID が表示されるか見てください。
```

この依頼は、AIが次のように分解できます。

```bash
# Codespace build VM でビルドし、成果物を WSL hub 経由で EC2 へ転送済み
agp sim start
make panel-button EC2=vibecode-graviton LINE=17
make panel-rfid EC2=vibecode-graviton UID=04:AB:CD:EF:01:23
agp sim status
agp sim log
make sim-test EC2=vibecode-graviton
```

---

## 今後の改善候補

- OLED framebuffer の期待値チェックを追加する
- `/api/events` で操作履歴を取得する
- 実機 RasPi5 にも同じ `run / logs / diagnose` 抽象を用意する
- systemd 化して EC2 上の起動/停止を安定させる
