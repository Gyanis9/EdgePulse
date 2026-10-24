#!/usr/bin/env bash
# 对任意文件的 SHA-256 摘要做 Ed25519 签名，写出 Base64 签名。
#
# 升级包的签名对象是归档文件本身：Agent 收到 agent.update 后用命令里的 sha256 摘要验签，
# 下载完再核对同一份摘要。manifest.json 也按同样方式签，签名文件固定叫 manifest.sig，
# 作为归档内的纵深防御。输入文件不改动，签名对象与落盘内容只有一份来源。
set -euo pipefail

usage() {
    cat <<'EOF'
用法：sign-agent-package.sh [--force] <私钥> <文件> <key-id>

脚本对 <文件> 的原始字节取 SHA-256，用 Ed25519 私钥对这 32 字节摘要签名，写出标准
Base64（无换行）的签名文件：
  普通文件      <文件>.sig，升级包因此得到 <归档>.sig
  manifest.json manifest.sig（同目录），保持归档内既有布局

私钥必须是 Ed25519。key id 只能由字母、数字、点、下划线与连字符组成。输入名为
manifest.json 时，脚本还要求文件内已写入相同的 signingKeyId，且 signatureAlgorithm
为 ed25519-sha256。

已存在的签名文件默认不覆盖；确需重签时加 --force。
EOF
}

die() {
    echo "错误：$*" >&2
    exit 1
}

force=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force)
            force=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

[[ $# -eq 3 ]] || {
    usage >&2
    exit 1
}

key_file="$1"
input_file="$2"
key_id="$3"

[[ -f "${key_file}" ]] || die "找不到签名私钥：${key_file}"
[[ -f "${input_file}" ]] || die "找不到待签名文件：${input_file}"
[[ "${key_id}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ && "${key_id}" != *..* ]] ||
    die "key id 只能由字母、数字、点、下划线与连字符组成，且不能包含 ..：${key_id}"
command -v openssl >/dev/null 2>&1 || die "缺少 openssl，请先安装 OpenSSL 3"

key_text="$(openssl pkey -in "${key_file}" -noout -text 2>/dev/null | head -n 1 || true)"
[[ "${key_text}" == ED25519* ]] || die "签名私钥不是 Ed25519：${key_file}"

is_manifest=0
if [[ "$(basename "${input_file}")" == "manifest.json" ]]; then
    is_manifest=1
    manifest_key_id="$(sed -n 's/.*"signingKeyId"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${input_file}" | head -n 1)"
    [[ -n "${manifest_key_id}" ]] || die "manifest 缺少 signingKeyId，请先用打包脚本写入 key id 再签名"
    [[ "${manifest_key_id}" == "${key_id}" ]] || die "manifest 的 signingKeyId 是 ${manifest_key_id}，与参数 ${key_id} 不一致"

    manifest_algorithm="$(sed -n 's/.*"signatureAlgorithm"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${input_file}" | head -n 1)"
    [[ "${manifest_algorithm}" == "ed25519-sha256" ]] ||
        die "manifest 的 signatureAlgorithm 必须是 ed25519-sha256，当前为 ${manifest_algorithm:-空}"
fi

if [[ "${is_manifest}" -eq 1 ]]; then
    signature_path="$(dirname "${input_file}")/manifest.sig"
else
    signature_path="${input_file}.sig"
fi
if [[ -e "${signature_path}" && "${force}" -ne 1 ]]; then
    die "签名文件已存在：${signature_path}；确需重签请加 --force"
fi

digest_file="$(mktemp)"
raw_signature_file="$(mktemp)"
encoded_file="$(mktemp)"
trap 'rm -f "${digest_file}" "${raw_signature_file}" "${encoded_file}"' EXIT

# 签的是摘要本身：先取原始 32 字节，再让 Ed25519 对这段字节签名
openssl dgst -sha256 -binary "${input_file}" > "${digest_file}"
openssl pkeyutl -sign -inkey "${key_file}" -rawin -in "${digest_file}" -out "${raw_signature_file}"
[[ "$(wc -c < "${raw_signature_file}" | tr -d '[:space:]')" == "64" ]] || die "Ed25519 签名长度不是 64 字节"
openssl base64 -A -in "${raw_signature_file}" -out "${encoded_file}"

# 先写临时文件再改名，避免中途失败留下半份签名
mv -f "${encoded_file}" "${signature_path}"

echo "已签名：${signature_path}（key id ${key_id}）"
