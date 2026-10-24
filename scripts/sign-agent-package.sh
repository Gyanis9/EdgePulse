#!/usr/bin/env bash
# 对包 manifest 的 SHA-256 摘要做 Ed25519 签名，写出 Base64 的 manifest.sig。
#
# 约定：key id 必须先写进 manifest。打包脚本生成 manifest 时写入 signingKeyId 与
# signatureAlgorithm；本脚本只校验 manifest 里的这两项与参数一致，然后对 manifest
# 的原始字节取 SHA-256，用私钥对 32 字节摘要签名。脚本不修改 manifest，避免签名
# 对象与落盘内容出现第二份来源。
set -euo pipefail

usage() {
    cat <<'EOF'
用法：sign-agent-package.sh <私钥> <manifest.json> <key-id>

manifest.json 必须已包含：
  "signingKeyId": "<key-id>"
  "signatureAlgorithm": "ed25519-sha256"
脚本校验 key id 与算法后，把 manifest 字节的 SHA-256 摘要交给 Ed25519 签名，
并在 manifest 同目录写出 manifest.sig（标准 Base64，无换行）。
EOF
}

die() {
    echo "错误：$*" >&2
    exit 1
}

[[ $# -eq 3 ]] || {
    usage >&2
    exit 1
}

key_file="$1"
manifest="$2"
key_id="$3"

[[ -f "${key_file}" ]] || die "找不到签名私钥：${key_file}"
[[ -f "${manifest}" ]] || die "找不到 manifest：${manifest}"
[[ "${key_id}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "key id 只能由字母、数字、点、下划线与连字符组成：${key_id}"
command -v openssl >/dev/null 2>&1 || die "缺少 openssl，请先安装 OpenSSL 3"

manifest_key_id="$(sed -n 's/.*"signingKeyId"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${manifest}" | head -n 1)"
[[ -n "${manifest_key_id}" ]] || die "manifest 缺少 signingKeyId，请先用打包脚本写入 key id 再签名"
[[ "${manifest_key_id}" == "${key_id}" ]] || die "manifest 的 signingKeyId 是 ${manifest_key_id}，与参数 ${key_id} 不一致"

manifest_algorithm="$(sed -n 's/.*"signatureAlgorithm"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${manifest}" | head -n 1)"
[[ "${manifest_algorithm}" == "ed25519-sha256" ]] ||
    die "manifest 的 signatureAlgorithm 必须是 ed25519-sha256，当前为 ${manifest_algorithm:-空}"

digest_file="$(mktemp)"
signature_file="$(mktemp)"
trap 'rm -f "${digest_file}" "${signature_file}"' EXIT

# 签的是摘要本身：先取原始 32 字节，再让 Ed25519 对这段字节签名
openssl dgst -sha256 -binary "${manifest}" > "${digest_file}"
openssl pkeyutl -sign -inkey "${key_file}" -rawin -in "${digest_file}" -out "${signature_file}"
openssl base64 -A -in "${signature_file}" -out "$(dirname "${manifest}")/manifest.sig"

echo "已签名：$(dirname "${manifest}")/manifest.sig（key id ${key_id}）"
