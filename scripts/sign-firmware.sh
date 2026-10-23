#!/usr/bin/env bash
# 对固件文件的 SHA-256 摘要做 Ed25519 签名，输出上传所需的 key 与 signature。
set -euo pipefail
key_file="${1:?用法: sign-firmware.sh <私钥> <固件> <key-id>}"
firmware="${2:?缺少固件路径}"
key_id="${3:?缺少 key id}"
digest_file="$(mktemp)"
signature_file="$(mktemp)"
trap 'rm -f "$digest_file" "$signature_file"' EXIT
# 签的是摘要本身：先取原始 32 字节，再让 Ed25519 对这段字节签名
openssl dgst -sha256 -binary "$firmware" > "$digest_file"
openssl pkeyutl -sign -inkey "$key_file" -rawin -in "$digest_file" -out "$signature_file"
printf 'key=%s\nsignature=%s\n' "$key_id" "$(openssl base64 -A -in "$signature_file")"
