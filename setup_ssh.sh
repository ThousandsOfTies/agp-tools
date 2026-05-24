#!/bin/bash
# Setup SSH connection to EC2 Graviton from Codespaces.
#
# Usage:
#   1. Set EC2_PUBLIC_IP environment variable (or pass as argument)
#   2. Paste the .pem key content when prompted
#
#   $ bash setup_ssh.sh 1.2.3.4

set -e

EC2_IP="${1:-${EC2_PUBLIC_IP:-}}"

if [ -z "$EC2_IP" ]; then
    read -rp "EC2 Public IP: " EC2_IP
fi

mkdir -p ~/.ssh
chmod 700 ~/.ssh

PEM_FILE="$HOME/.ssh/vibecode-graviton.pem"

# .pem の配置 (Codespaces Secret 経由 or 手動貼り付け)
if [ -n "${VIBECODE_GRAVITON_PEM:-}" ]; then
    # Codespaces Secret が環境変数として注入されている場合
    echo "$VIBECODE_GRAVITON_PEM" > "$PEM_FILE"
    echo "[setup_ssh] .pem written from Codespaces Secret."
elif [ ! -f "$PEM_FILE" ]; then
    echo "Paste the contents of vibecode-graviton.pem, then press Ctrl+D:"
    cat > "$PEM_FILE"
    echo "[setup_ssh] .pem written from stdin."
else
    echo "[setup_ssh] $PEM_FILE already exists, skipping."
fi

chmod 600 "$PEM_FILE"

# ~/.ssh/config への追記 (重複チェックあり)
if grep -q "vibecode-graviton" ~/.ssh/config 2>/dev/null; then
    echo "[setup_ssh] ~/.ssh/config already has vibecode-graviton entry, skipping."
else
    cat >> ~/.ssh/config << EOF

Host vibecode-graviton
    HostName ${EC2_IP}
    User ubuntu
    IdentityFile ~/.ssh/vibecode-graviton.pem
    ServerAliveInterval 60
EOF
    echo "[setup_ssh] Added vibecode-graviton to ~/.ssh/config."
fi

echo ""
echo "Done. Test with:"
echo "  ssh vibecode-graviton"
