#!/usr/bin/env bash
# 安装 EdgePulse Agent：复制二进制与 systemd 单元、生成配置与环境文件模板，并按需启动服务。
set -euo pipefail

usage() {
    cat <<'EOF'
用法：install-agent.sh [选项]

在解包后的安装包目录中运行。默认安装路径：
  二进制     /usr/local/bin/edgepulse-agent
  单元文件   /etc/systemd/system/edgepulse-agent.service
  配置目录   /etc/edgepulse-agent
  状态目录   /var/lib/edgepulse-agent
  日志目录   /var/log/edgepulse-agent

选项：
  --prefix DIR       安装前缀，二进制写入 DIR/bin（默认 /usr/local）
  --config-dir DIR   配置目录（默认 /etc/edgepulse-agent）
  --state-dir DIR    SQLite 状态与固件暂存目录（默认 /var/lib/edgepulse-agent）
  --log-dir DIR      日志目录（默认 /var/log/edgepulse-agent）
  --no-start         只安装文件，不执行 systemctl daemon-reload 与 enable --now
  -h, --help         显示本帮助

环境变量：
  SYSTEMD_UNIT_DIR   单元文件目录，默认 /etc/systemd/system；测试时指向临时目录

默认系统路径需要 root。已有 agent.yaml 与 agent.env 不会被覆盖。
EOF
}

die() {
    echo "错误：$*" >&2
    exit 1
}

normalize_path() {
    local path="$1"
    while [[ "${path}" == */ && "${path}" != "/" ]]; do
        path="${path%/}"
    done
    printf '%s' "${path}"
}

escape_sed_replacement() {
    printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'
}

prefix="/usr/local"
config_dir="/etc/edgepulse-agent"
state_dir="/var/lib/edgepulse-agent"
log_dir="/var/log/edgepulse-agent"
unit_dir="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"
no_start=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            [[ $# -ge 2 ]] || die "--prefix 缺少目录"
            prefix="$2"
            shift 2
            ;;
        --config-dir)
            [[ $# -ge 2 ]] || die "--config-dir 缺少目录"
            config_dir="$2"
            shift 2
            ;;
        --state-dir)
            [[ $# -ge 2 ]] || die "--state-dir 缺少目录"
            state_dir="$2"
            shift 2
            ;;
        --log-dir)
            [[ $# -ge 2 ]] || die "--log-dir 缺少目录"
            log_dir="$2"
            shift 2
            ;;
        --no-start)
            no_start=1
            shift
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

prefix="$(normalize_path "${prefix}")"
config_dir="$(normalize_path "${config_dir}")"
state_dir="$(normalize_path "${state_dir}")"
log_dir="$(normalize_path "${log_dir}")"
unit_dir="$(normalize_path "${unit_dir}")"

for path in "${prefix}" "${config_dir}" "${state_dir}" "${log_dir}" "${unit_dir}"; do
    [[ -n "${path}" ]] || die "安装路径不能为空"
    [[ "${path}" != "/" ]] || die "安装路径不能是 /"
    [[ "${path}" != *[[:space:]]* ]] || die "安装路径不能包含空白字符：${path}"
done

if [[ "${EUID}" -ne 0 ]]; then
    if [[ "${prefix}" == "/usr/local" || "${config_dir}" == "/etc/edgepulse-agent" ||
          "${state_dir}" == "/var/lib/edgepulse-agent" || "${log_dir}" == "/var/log/edgepulse-agent" ||
          "${unit_dir}" == "/etc/systemd/system" ]]; then
        die "默认系统路径需要 root：请用 sudo 运行，或把全部路径选项与 SYSTEMD_UNIT_DIR 指到可写目录"
    fi
    if [[ "${no_start}" -eq 0 ]]; then
        die "注册并启动 systemd 服务需要 root：非 root 安装请加 --no-start"
    fi
fi

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
binary_source="${package_dir}/edgepulse-agent"
unit_source="${package_dir}/edgepulse-agent.service"
config_source="${package_dir}/agent.yaml.example"

for file in "${binary_source}" "${unit_source}" "${config_source}"; do
    [[ -f "${file}" ]] || die "安装包不完整，缺少 ${file}"
done

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

# 单元里的路径随安装选项替换；默认值下渲染结果与仓库中的单元模板一致
awk \
    -v exec_start="ExecStart=${prefix}/bin/edgepulse-agent --config ${config_dir}/agent.yaml --console" \
    -v environment_file="EnvironmentFile=-${config_dir}/agent.env" \
    -v read_write_paths="ReadWritePaths=${state_dir} ${log_dir} ${config_dir}" '
    /^ExecStart=/ { print exec_start; next }
    /^EnvironmentFile=/ { print environment_file; next }
    /^ReadWritePaths=/ { print read_write_paths; next }
    { print }
' "${unit_source}" > "${work_dir}/edgepulse-agent.service"

install -d -m 0755 "${prefix}/bin" "${unit_dir}"
install -d -m 0750 "${config_dir}" "${state_dir}" "${log_dir}"

if [[ -e "${config_dir}/agent.yaml" ]]; then
    echo "保留已有配置：${config_dir}/agent.yaml"
else
    sed \
        -e "s|/etc/edgepulse-agent|$(escape_sed_replacement "${config_dir}")|g" \
        -e "s|/var/lib/edgepulse-agent|$(escape_sed_replacement "${state_dir}")|g" \
        -e "s|/var/log/edgepulse-agent|$(escape_sed_replacement "${log_dir}")|g" \
        "${config_source}" > "${work_dir}/agent.yaml"
    install -m 0600 "${work_dir}/agent.yaml" "${config_dir}/agent.yaml"
    echo "已写入配置模板：${config_dir}/agent.yaml"
fi

if [[ -e "${config_dir}/agent.env" ]]; then
    echo "保留已有环境文件：${config_dir}/agent.env"
else
    cat > "${work_dir}/agent.env" <<'EOF'
# EdgePulse Agent 令牌环境变量，权限保持 0600，不要提交到版本库。
# 首次注册时填入控制台签发的一次性 enroll 令牌，注册成功后清空。
#EDGEPULSE_AGENT_ENROLLMENT_TOKEN=
# 旧节点更换证书时填入 rebind 令牌，成功后清空。
#EDGEPULSE_AGENT_REBIND_TOKEN=
EOF
    install -m 0600 "${work_dir}/agent.env" "${config_dir}/agent.env"
    echo "已创建环境文件：${config_dir}/agent.env"
fi

install -m 0755 "${binary_source}" "${prefix}/bin/edgepulse-agent"
install -m 0644 "${work_dir}/edgepulse-agent.service" "${unit_dir}/edgepulse-agent.service"

echo "已安装二进制：${prefix}/bin/edgepulse-agent"
echo "已安装 systemd 单元：${unit_dir}/edgepulse-agent.service"

if [[ "${no_start}" -eq 1 ]]; then
    echo "已跳过 systemctl（--no-start）：核对 ${config_dir}/agent.yaml 与 ${config_dir}/agent.env 后，执行 systemctl daemon-reload && systemctl enable --now edgepulse-agent.service"
    exit 0
fi

if ! command -v systemctl >/dev/null 2>&1; then
    die "未找到 systemctl：请加 --no-start 只安装文件，或在有 systemd 的主机上重新安装"
fi

systemctl daemon-reload
systemctl enable --now edgepulse-agent.service
echo "服务已启动：systemctl status edgepulse-agent.service"
