#!/usr/bin/env bash
# 生成固件签名密钥对：私钥留在 var/，只把公钥放进信任键目录。
set -euo pipefail
key_id="${1:-prod-2026}"
# key id 会直接拼进文件名；含路径分隔符或 .. 会把公钥写出信任键目录，先拒绝
if [[ ! "$key_id" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ || "$key_id" == *..* ]]; then
  echo "key id 只能由字母、数字、点、下划线与连字符组成，且不能包含 ..：${key_id}" >&2
  exit 1
fi
public_key="config/firmware-keys/${key_id}.pem"
if [[ -e var/firmware-signing-key.pem ]]; then
  echo "私钥已存在：var/firmware-signing-key.pem，拒绝覆盖" >&2
  exit 1
fi
if [[ -e "$public_key" ]]; then
  echo "公钥已存在：${public_key}，拒绝覆盖" >&2
  exit 1
fi
mkdir -p config/firmware-keys var
openssl genpkey -algorithm ED25519 -out var/firmware-signing-key.pem
openssl pkey -in var/firmware-signing-key.pem -pubout -out "$public_key"
echo "已生成私钥 var/firmware-signing-key.pem 与公钥 ${public_key}"
