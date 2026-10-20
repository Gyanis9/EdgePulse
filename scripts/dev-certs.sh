#!/usr/bin/env bash
# 生成本地开发证书：CA、服务器、Agent 与一张同 CA 但不进白名单的对照证书。
set -euo pipefail


root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cert_dir="${root_dir}/var/certs"
mkdir -p "${cert_dir}"

if ! command -v openssl >/dev/null 2>&1; then
    echo "缺少 openssl：请先安装 OpenSSL 并加入 PATH" >&2
    exit 1
fi

ca_key="${cert_dir}/ca-key.pem"
ca_cert="${cert_dir}/ca.pem"

if [[ ! -f "${ca_cert}" || ! -f "${ca_key}" ]]; then
    openssl ecparam -name prime256v1 -genkey -noout -out "${ca_key}"
    openssl req -x509 -new -key "${ca_key}" -sha256 -days 3650 -subj "//CN=EdgePulse Dev CA" -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -out "${ca_cert}"
    chmod 600 "${ca_key}" 2>/dev/null || true
    echo "已生成开发 CA：${ca_cert}"
fi

make_server_certificate() {
    local name="$1"
    local common_name="$2"
    local extension_file="${cert_dir}/${name}.ext"
    if [[ -f "${cert_dir}/${name}.pem" && -f "${cert_dir}/${name}-key.pem" ]]; then
        return
    fi
    openssl ecparam -name prime256v1 -genkey -noout -out "${cert_dir}/${name}-key.pem"
    openssl req -new -key "${cert_dir}/${name}-key.pem" -subj "//CN=${common_name}" -out "${cert_dir}/${name}.csr"
    cat > "${extension_file}" <<EOF
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
subjectAltName=DNS:localhost,IP:127.0.0.1
extendedKeyUsage=serverAuth
EOF
    openssl x509 -req -in "${cert_dir}/${name}.csr" -CA "${ca_cert}" -CAkey "${ca_key}" -CAcreateserial -days 825 -sha256 -extfile "${extension_file}" -out "${cert_dir}/${name}.pem"
    rm -f "${cert_dir}/${name}.csr" "${extension_file}"
    chmod 600 "${cert_dir}/${name}-key.pem" 2>/dev/null || true
    echo "已生成服务器证书：${cert_dir}/${name}.pem"
}

make_client_certificate() {
    local name="$1"
    local common_name="$2"
    local extension_file="${cert_dir}/${name}.ext"
    if [[ -f "${cert_dir}/${name}.pem" && -f "${cert_dir}/${name}-key.pem" ]]; then
        return
    fi
    openssl ecparam -name prime256v1 -genkey -noout -out "${cert_dir}/${name}-key.pem"
    openssl req -new -key "${cert_dir}/${name}-key.pem" -subj "//CN=${common_name}" -out "${cert_dir}/${name}.csr"
    cat > "${extension_file}" <<EOF
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth
EOF
    openssl x509 -req -in "${cert_dir}/${name}.csr" -CA "${ca_cert}" -CAkey "${ca_key}" -CAcreateserial -days 825 -sha256 -extfile "${extension_file}" -out "${cert_dir}/${name}.pem"
    rm -f "${cert_dir}/${name}.csr" "${extension_file}"
    chmod 600 "${cert_dir}/${name}-key.pem" 2>/dev/null || true
    echo "已生成客户端证书：${cert_dir}/${name}.pem"
}

make_server_certificate "server" "localhost"
make_client_certificate "agent" "edgepulse-agent"
make_client_certificate "agent-foreign" "edgepulse-agent-foreign"

fingerprint="$(openssl x509 -in "${cert_dir}/agent.pem" -noout -fingerprint -sha256 | sed 's/.*=//' | tr -d ':' | tr '[:upper:]' '[:lower:]')"
printf '%s\n' "${fingerprint}" > "${cert_dir}/agent-fingerprint.txt"
printf '%s\n' "${fingerprint}" > "${cert_dir}/agent-ca-fingerprint.txt"

echo "Agent 指纹：${fingerprint}"
echo "证书目录：${cert_dir}"
