#!/usr/bin/env bash
# 开发运行：生成证书与运维令牌，然后前台启动 edgepulse-server。
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root_dir}"

bash scripts/dev-certs.sh

if [[ ! -s var/ops-token ]]; then
    mkdir -p var
    openssl rand -hex 32 > var/ops-token
    chmod 600 var/ops-token 2>/dev/null || true
fi

export EDGEPULSE_OPS_BEARER_TOKEN="$(cat var/ops-token)"
if [[ -s var/certs/agent-fingerprint.txt ]]; then
    export EDGEPULSE_AGENT_FINGERPRINT="$(cat var/certs/agent-fingerprint.txt)"
fi

server_binary="build/debug/edgepulse-server"
if [[ -x "${server_binary}.exe" ]]; then
    server_binary="${server_binary}.exe"
fi

exec "${server_binary}" --config config/dev.yaml
