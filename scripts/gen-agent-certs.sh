#!/usr/bin/env bash
# 为外部 Agent 生成一张由开发 CA 签发的客户端证书。
# 用法：scripts/gen-agent-certs.sh <agent-name> [--days <有效期天数>] [--out-dir <输出目录>]
# 省略有效期时按 825 天签发；省略输出目录时写入 var/certs。
set -euo pipefail


usage() {
    echo "用法：scripts/gen-agent-certs.sh <agent-name> [--days <有效期天数>] [--out-dir <输出目录>]" >&2
}

validity_days=825
out_dir=""
name=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --days)
            if [[ $# -lt 2 ]]; then
                usage
                exit 2
            fi
            validity_days="$2"
            shift 2
            ;;
        --out-dir)
            if [[ $# -lt 2 ]]; then
                usage
                exit 2
            fi
            out_dir="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "未知选项：$1" >&2
            usage
            exit 2
            ;;
        *)
            if [[ -n "${name}" ]]; then
                usage
                exit 2
            fi
            name="$1"
            shift
            ;;
    esac
done

if [[ -z "${name}" ]]; then
    usage
    exit 2
fi
if [[ ! "${validity_days}" =~ ^[0-9]+$ ]] || (( validity_days < 1 || validity_days > 3650 )); then
    echo "有效期必须是 1 到 3650 之间的整数天：${validity_days}" >&2
    exit 2
fi

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cert_dir="${root_dir}/var/certs"
ca_key="${cert_dir}/ca-key.pem"
ca_cert="${cert_dir}/ca.pem"

if [[ ! -f "${ca_key}" || ! -f "${ca_cert}" ]]; then
    echo "缺少开发 CA：请先运行 scripts/dev-certs.sh" >&2
    exit 1
fi

if [[ -z "${out_dir}" ]]; then
    out_dir="${cert_dir}"
fi
mkdir -p "${out_dir}"
# 临时工作文件与成品同目录，避免在 CA 目录留下本次调用的 CSR 或扩展文件
extension_file="${out_dir}/${name}.ext"
request_file="${out_dir}/${name}.csr"

openssl ecparam -name prime256v1 -genkey -noout -out "${out_dir}/${name}-key.pem"
openssl req -new -key "${out_dir}/${name}-key.pem" -subj "//CN=${name}" -out "${request_file}"
cat > "${extension_file}" <<EOF
extendedKeyUsage=clientAuth
EOF
# 串行号固定落在 CA 所在目录，输出目录改到别处也不会新建第二份 ca.srl
openssl x509 -req -in "${request_file}" -CA "${ca_cert}" -CAkey "${ca_key}" -CAserial "${cert_dir}/ca.srl" -CAcreateserial \
    -days "${validity_days}" -sha256 -extfile "${extension_file}" -out "${out_dir}/${name}.pem"
rm -f "${request_file}" "${extension_file}"
chmod 600 "${out_dir}/${name}-key.pem" 2>/dev/null || true
fingerprint="$(openssl x509 -in "${out_dir}/${name}.pem" -noout -fingerprint -sha256 | sed 's/.*=//' | tr -d ':' | tr '[:upper:]' '[:lower:]')"
echo "证书：${out_dir}/${name}.pem"
echo "有效期：${validity_days} 天"
echo "指纹：${fingerprint}"
