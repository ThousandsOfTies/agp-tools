# agp-tools

AgentCockpit のシミュレーション環境で使うツール群です。

主な内容:

- `cuse-stubs/`: EC2 Graviton などで I2C/GPIO/SPI を仮想化するスタブ
- `cuse-stubs/web-bridge/`: 仮想ハードウェア操作用の HTTP bridge
- `scripts/run_scenario.py`: bridge に対してシナリオを実行する補助ツール
- `setup_ssh.sh`, `ssh_config.template`: Remote SSH 用の補助ファイル
- `docs/`: シミュレーション設定と AI エージェント操作メモ

## Build

```bash
make cross
make native
```

## Deploy To EC2

```bash
make deploy-ec2 EC2=vibecode-graviton
```

アプリ本体も同時に転送する場合:

```bash
make deploy-ec2 EC2=vibecode-graviton APP_BINARY=../embedded-poc-app/app/sensor_demo
```

## Simulation

```bash
make sim-start EC2=vibecode-graviton APP_BINARY=../embedded-poc-app/app/sensor_demo
make panel-button EC2=vibecode-graviton LINE=17
make panel-rfid EC2=vibecode-graviton
make sim-logs EC2=vibecode-graviton
```
