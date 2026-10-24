#!/usr/bin/env bash
# 打包 Linux Agent：收集二进制、systemd 单元、安装/卸载脚本与配置模板，生成带 SHA-256 清单的 tar.gz。
# 提供签名私钥时同时生成 manifest.sig；checksums.sha256 只列 manifest.files 里的数据文件。
set -euo pipefail

usage() {
    cat <<'EOF'
用法：package-agent.sh [选项]

选项：
  --version VER         包版本，默认取 CMakeLists.txt 中的项目版本
  --output DIR          输出目录，默认 build/agent-package
  --binary PATH         已有的 Linux Agent 二进制；省略时在 Linux 上执行 Release 构建
  --signing-key PATH    Ed25519 私钥（PEM）；给出时对 manifest 签名
  --signing-key-id ID   manifest 里记录的 key id，必须与 --signing-key 同时给出
  -h, --help            显示本帮助

生成的 tar.gz 顶层包含 edgepulse-agent、edgepulse-agent.service、
install-agent.sh、uninstall-agent.sh、agent.yaml.example、manifest.json 与
checksums.sha256；提供签名私钥时另有 manifest.sig。

签名前 manifest 会写入 signingKeyId 与 signatureAlgorithm（ed25519-sha256）。
manifest.files 与 checksums.sha256 只覆盖上面的数据文件：manifest.json、
manifest.sig 与 checksums.sha256 自身不在清单内，避免清单与校验文件互相引用。
输出目录同时保留 <包名>.manifest.json、<包名>.checksums.sha256 与
<包名>.manifest.sig 三份旁路文件，供服务端上传与安装器核验。
EOF
}

die() {
    echo "错误：$*" >&2
    exit 1
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9.]*\).*$/\1/p' "${repo_root}/CMakeLists.txt" | head -n 1)"
[[ -n "${version}" ]] || die "无法从 CMakeLists.txt 读取项目版本，请用 --version 指定"
output_dir="${repo_root}/build/agent-package"
binary=""
signing_key=""
signing_key_id=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            [[ $# -ge 2 ]] || die "--version 缺少版本号"
            version="$2"
            shift 2
            ;;
        --output)
            [[ $# -ge 2 ]] || die "--output 缺少目录"
            output_dir="$2"
            shift 2
            ;;
        --binary)
            [[ $# -ge 2 ]] || die "--binary 缺少二进制路径"
            binary="$2"
            shift 2
            ;;
        --signing-key)
            [[ $# -ge 2 ]] || die "--signing-key 缺少私钥路径"
            signing_key="$2"
            shift 2
            ;;
        --signing-key-id)
            [[ $# -ge 2 ]] || die "--signing-key-id 缺少 key id"
            signing_key_id="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "未知参数：$1（--help 查看用法）"
            ;;
    esac
done

[[ "${version}" =~ ^[0-9]+(\.[0-9]+)*([-+][0-9A-Za-z.-]+)?$ ]] || die "版本号不合法：${version}"

if [[ -n "${signing_key}" || -n "${signing_key_id}" ]]; then
    [[ -n "${signing_key}" && -n "${signing_key_id}" ]] || die "--signing-key 与 --signing-key-id 必须同时给出"
    [[ -f "${signing_key}" ]] || die "找不到签名私钥：${signing_key}"
    [[ "${signing_key_id}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ && "${signing_key_id}" != *..* ]] ||
        die "key id 只能由字母、数字、点、下划线与连字符组成，且不能包含 ..：${signing_key_id}"
    command -v openssl >/dev/null 2>&1 || die "缺少 openssl，请先安装 OpenSSL 3"
fi
signed=0
[[ -n "${signing_key}" ]] && signed=1

machine="$(uname -m)"
case "${machine}" in
    x86_64|amd64)
        architecture="x86_64"
        ;;
    aarch64|arm64)
        architecture="aarch64"
        ;;
    *)
        architecture="${machine}"
        ;;
esac
platform="linux-${architecture}"

if [[ -z "${binary}" ]]; then
    if [[ "$(uname -s)" != "Linux" ]]; then
        die "当前主机不是 Linux，无法执行 Release 构建：请用 --binary 指定已构建的 Linux Agent 二进制"
    fi
    echo "构建 Release Agent..."
    cd "${repo_root}"
    cmake --preset release
    cmake --build --preset release --target edgepulse-agent
    binary="${repo_root}/build/release/bin/edgepulse-agent"
fi

[[ -f "${binary}" ]] || die "找不到 Agent 二进制：${binary}"
binary="$(cd "$(dirname "${binary}")" && pwd)/$(basename "${binary}")"

for tool in tar sha256sum install; do
    command -v "${tool}" >/dev/null 2>&1 || die "缺少 ${tool}，请先安装 coreutils 与 tar"
done

mkdir -p "${output_dir}"
output_dir="$(cd "${output_dir}" && pwd)"

source_files=(
    "${repo_root}/agent/packaging/linux/edgepulse-agent.service"
    "${repo_root}/agent/packaging/linux/install-agent.sh"
    "${repo_root}/agent/packaging/linux/uninstall-agent.sh"
    "${repo_root}/agent/packaging/config/agent.yaml.example"
)
for file in "${source_files[@]}"; do
    [[ -f "${file}" ]] || die "缺少打包输入：${file}"
done

stage_dir="$(mktemp -d)"
trap 'rm -rf "${stage_dir}"' EXIT

install -m 0755 "${binary}" "${stage_dir}/edgepulse-agent"
install -m 0644 "${repo_root}/agent/packaging/linux/edgepulse-agent.service" "${stage_dir}/edgepulse-agent.service"
install -m 0755 "${repo_root}/agent/packaging/linux/install-agent.sh" "${stage_dir}/install-agent.sh"
install -m 0755 "${repo_root}/agent/packaging/linux/uninstall-agent.sh" "${stage_dir}/uninstall-agent.sh"
install -m 0644 "${repo_root}/agent/packaging/config/agent.yaml.example" "${stage_dir}/agent.yaml.example"

mapfile -t package_files < <(cd "${stage_dir}" && find . -maxdepth 1 -type f ! -name manifest.json -exec basename {} \; | LC_ALL=C sort)
[[ "${#package_files[@]}" -gt 0 ]] || die "没有可打包的文件"

{
    printf '{\n'
    printf '  "version": "%s",\n' "${version}"
    printf '  "platform": "%s",\n' "${platform}"
    if [[ "${signed}" -eq 1 ]]; then
        printf '  "signingKeyId": "%s",\n' "${signing_key_id}"
        printf '  "signatureAlgorithm": "ed25519-sha256",\n'
    fi
    printf '  "files": [\n'
    first=1
    for name in "${package_files[@]}"; do
        size="$(wc -c < "${stage_dir}/${name}" | tr -d '[:space:]')"
        digest="$(sha256sum "${stage_dir}/${name}" | cut -d' ' -f1)"
        if [[ "${first}" -eq 1 ]]; then
            first=0
        else
            printf ',\n'
        fi
        printf '    {"path": "%s", "size": %s, "sha256": "%s"}' "${name}" "${size}" "${digest}"
    done
    printf '\n  ]\n'
    printf '}\n'
} > "${stage_dir}/manifest.json"

# 校验文件与 manifest 覆盖同一批数据文件：清单自身、签名与校验文件不列进去，避免互相引用。
# sha256sum 在部分平台用「空格 + 星号」标记二进制模式，这里统一成「空格 + 空格」。
(cd "${stage_dir}" && sha256sum "${package_files[@]}" | sed -E 's/^([0-9a-f]{64}) [ *]/\1  /') > "${stage_dir}/checksums.sha256"

if [[ "${signed}" -eq 1 ]]; then
    bash "${repo_root}/scripts/sign-agent-package.sh" "${signing_key}" "${stage_dir}/manifest.json" "${signing_key_id}"
fi

archive_name="edgepulse-agent-${version}-${platform}.tar.gz"
archive_path="${output_dir}/${archive_name}"
manifest_path="${output_dir}/edgepulse-agent-${version}-${platform}.manifest.json"
checksums_path="${output_dir}/edgepulse-agent-${version}-${platform}.checksums.sha256"

archive_entries=("${package_files[@]}" manifest.json checksums.sha256)
if [[ "${signed}" -eq 1 ]]; then
    archive_entries+=(manifest.sig)
fi
tar -czf "${archive_path}" -C "${stage_dir}" "${archive_entries[@]}"
cp "${stage_dir}/manifest.json" "${manifest_path}"
cp "${stage_dir}/checksums.sha256" "${checksums_path}"

if [[ "${signed}" -eq 1 ]]; then
    cp "${stage_dir}/manifest.sig" "${output_dir}/edgepulse-agent-${version}-${platform}.manifest.sig"
fi

echo "安装包：${archive_path}"
echo "清单：${manifest_path}"
echo "校验：${checksums_path}"
if [[ "${signed}" -eq 1 ]]; then
    echo "签名：${output_dir}/edgepulse-agent-${version}-${platform}.manifest.sig（key id ${signing_key_id}）"
else
    echo "未提供签名私钥：本次生成的是未签名安装包，不能用于需要验签的升级"
fi
