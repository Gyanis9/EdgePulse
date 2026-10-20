#!/usr/bin/env bash
# 为外部 Agent 生成一张由开发 CA 签发的客户端证书。
set -euo pipefail


if [[ $# -ne 1 ]]; then
    echo "用法：scripts/gen-agent-certs.sh <agent-name>" >&2
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

name="$1"
extension_file="${cert_dir}/${name}.ext"
openssl ecparam -name prime256v1 -genkey -noout -out "${cert_dir}/${name}-key.pem"
openssl req -new -key "${cert_dir}/${name}-key.pem" -subj "//CN=${name}" -out "${cert_dir}/${name}.csr"
cat > "${extension_file}" <<EOF
extendedKeyUsage=clientAuth
EOF
openssl x509 -req -in "${cert_dir}/${name}.csr" -CA "${ca_cert}" -CAkey "${ca_key}" -CAcreateserial -days 825 -sha256 -extfile "${extension_file}" -out "${cert_dir}/${name}.pem"
rm -f "${cert_dir}/${name}.csr" "${extension_file}"
fingerprint="$(openssl x509 -in "${cert_dir}/${name}.pem" -noout -fingerprint -sha256 | sed 's/.*=//' | tr -d ':' | tr '[:upper:]' '[:lower:]')"
echo "证书：${cert_dir}/${name}.pem"
echo "指纹：${fingerprint}"
