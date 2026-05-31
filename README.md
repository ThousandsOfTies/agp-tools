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
make
make clean
```

Codespace build VM では ARM64 向けにビルドします。EC2 への転送、simulation runtime 操作、Virtual Hardware 操作は WSL hub 側の AgentCockpit から行います。
