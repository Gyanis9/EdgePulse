#!/usr/bin/env bash
# 生成固件签名密钥对：私钥留在 var/，只把公钥放进信任键目录。
set -euo pipefail
key_id="${1:-prod-2026}"
mkdir -p config/firmware-keys var
if [[ -e var/firmware-signing-key.pem ]]; then
  echo "私钥已存在：var/firmware-signing-key.pem，拒绝覆盖" >&2
  exit 1
fi
openssl genpkey -algorithm ED25519 -out var/firmware-signing-key.pem
openssl pkey -in var/firmware-signing-key.pem -pubout -out "config/firmware-keys/${key_id}.pem"
echo "已生成私钥 var/firmware-signing-key.pem 与公钥 config/firmware-keys/${key_id}.pem"
