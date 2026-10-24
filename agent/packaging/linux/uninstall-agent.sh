#!/usr/bin/env bash
# 卸载 EdgePulse Agent：停止并禁用服务，删除单元与二进制；配置/状态/日志默认保留，--purge 才删除。
set -euo pipefail

usage() {
    cat <<'EOF'
用法：uninstall-agent.sh [选项]

默认卸载路径与 install-agent.sh 一致：
  二进制     /usr/local/bin/edgepulse-agent
  单元文件   /etc/systemd/system/edgepulse-agent.service
  配置目录   /etc/edgepulse-agent
  状态目录   /var/lib/edgepulse-agent
  日志目录   /var/log/edgepulse-agent

选项：
  --prefix DIR       安装前缀，删除 DIR/bin/edgepulse-agent（默认 /usr/local）
  --config-dir DIR   配置目录（默认 /etc/edgepulse-agent）
  --state-dir DIR    SQLite 状态与固件暂存目录（默认 /var/lib/edgepulse-agent）
  --log-dir DIR      日志目录（默认 /var/log/edgepulse-agent）
  --no-start         不调用 systemctl（只删除文件）
  --purge            同时删除配置、状态与日志目录
  -h, --help         显示本帮助

环境变量：
  SYSTEMD_UNIT_DIR   单元文件目录，默认 /etc/systemd/system

默认系统路径需要 root。单元或服务已经不存在时脚本仍可正常结束。
EOF
}

die() {
    echo "错误：$*" >&2
    exit 1
}

require_realpath() {
    command -v realpath >/dev/null 2>&1 ||
        die "未找到 realpath（GNU coreutils）：无法规范化卸载路径，请先安装 coreutils"
}

# 路径先规范化为逻辑绝对路径，再做校验与删除：realpath -m 折叠重复斜杠、. 与 ..，
# 且不要求目标已存在。校验值与实际删除目标来自同一次规范化，避免两者不一致。
canonical_path() {
    local raw="$1"
    local option="$2"
    local canonical
    [[ -n "${raw}" ]] || die "${option} 不能为空"
    [[ "${raw}" != *$'\n'* && "${raw}" != *$'\r'* ]] ||
        die "${option} 不能包含换行或回车：$(printf '%q' "${raw}")"
    [[ "${raw}" == /* ]] || die "${option} 必须是绝对路径：${raw}"
    canonical="$(realpath -m -- "${raw}")" || die "${option} 无法规范化路径：${raw}"
    printf '%s' "${canonical}"
}

# 系统目录不接受为卸载目标；各选项的文档默认值除外，默认卸载仍使用 /usr/local
# 与 /etc/systemd/system。
is_system_dir() {
    case "$1" in
        /|/etc|/usr|/var|/home|/tmp|/opt|/root|/srv|/var/lib|/var/log|/usr/local|/etc/systemd|/etc/systemd/system)
            return 0
            ;;
    esac
    return 1
}

# 校验的是规范化后的路径，因此这里只剩 realpath 失败时的兜底断言：
# 结果必须绝对、无空白字符、无 .. 路径段、不以 . 结尾，且不命中系统目录。
check_path() {
    local path="$1"
    local option="$2"
    local default_value="$3"
    [[ -n "${path}" ]] || die "${option} 不能为空"
    [[ "${path}" == /* ]] || die "${option} 必须是绝对路径：${path}"
    [[ "${path}" != *[[:space:]]* ]] || die "${option} 不能包含空白字符：${path}"
    [[ "${path}" != */../* && "${path}" != */.. ]] || die "${option} 不能包含 .. 路径段：${path}"
    [[ "${path}" != */. ]] || die "${option} 不能以 . 结尾：${path}"
    if [[ "${path}" != "${default_value}" ]] && is_system_dir "${path}"; then
        die "${option} 不能是系统目录：${path}"
    fi
}

prefix="/usr/local"
config_dir="/etc/edgepulse-agent"
state_dir="/var/lib/edgepulse-agent"
log_dir="/var/log/edgepulse-agent"
unit_dir="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"
no_start=0
purge=0

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
        --purge)
            purge=1
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

require_realpath

prefix="$(canonical_path "${prefix}" "--prefix")"
config_dir="$(canonical_path "${config_dir}" "--config-dir")"
state_dir="$(canonical_path "${state_dir}" "--state-dir")"
log_dir="$(canonical_path "${log_dir}" "--log-dir")"
unit_dir="$(canonical_path "${unit_dir}" "SYSTEMD_UNIT_DIR")"

check_path "${prefix}" "--prefix" "/usr/local"
check_path "${config_dir}" "--config-dir" "/etc/edgepulse-agent"
check_path "${state_dir}" "--state-dir" "/var/lib/edgepulse-agent"
check_path "${log_dir}" "--log-dir" "/var/log/edgepulse-agent"
check_path "${unit_dir}" "SYSTEMD_UNIT_DIR" "/etc/systemd/system"

if [[ "${purge}" -eq 1 ]]; then
    for path in "${config_dir}" "${state_dir}" "${log_dir}"; do
        [[ "${path}" == */*/* ]] || die "--purge 目标至少要有两级路径：${path}"
        base_name="${path##*/}"
        [[ -n "${base_name}" && "${base_name}" != "." && "${base_name}" != ".." ]] ||
            die "--purge 目标目录名不能为空、. 或 ..：${path}"
    done
fi

if [[ "${EUID}" -ne 0 ]]; then
    if [[ "${prefix}" == "/usr/local" || "${config_dir}" == "/etc/edgepulse-agent" ||
          "${state_dir}" == "/var/lib/edgepulse-agent" || "${log_dir}" == "/var/log/edgepulse-agent" ||
          "${unit_dir}" == "/etc/systemd/system" ]]; then
        die "默认系统路径需要 root：请用 sudo 运行，或把全部路径选项与 SYSTEMD_UNIT_DIR 指到可写目录"
    fi
    if [[ "${no_start}" -eq 0 ]]; then
        die "停止并禁用 systemd 服务需要 root：非 root 卸载请加 --no-start"
    fi
fi

if [[ "${no_start}" -eq 0 ]] && command -v systemctl >/dev/null 2>&1; then
    if systemctl is-active --quiet edgepulse-agent.service; then
        systemctl stop edgepulse-agent.service || echo "警告：停止 edgepulse-agent.service 失败，继续删除文件" >&2
    fi
    if systemctl is-enabled --quiet edgepulse-agent.service 2>/dev/null; then
        systemctl disable edgepulse-agent.service || echo "警告：禁用 edgepulse-agent.service 失败，继续删除文件" >&2
    fi
fi

if [[ -e "${unit_dir}/edgepulse-agent.service" ]]; then
    rm -f "${unit_dir}/edgepulse-agent.service"
    echo "已删除 systemd 单元：${unit_dir}/edgepulse-agent.service"
else
    echo "systemd 单元不存在，跳过：${unit_dir}/edgepulse-agent.service"
fi

if [[ -e "${prefix}/bin/edgepulse-agent" ]]; then
    rm -f "${prefix}/bin/edgepulse-agent"
    echo "已删除二进制：${prefix}/bin/edgepulse-agent"
else
    echo "二进制不存在，跳过：${prefix}/bin/edgepulse-agent"
fi

if [[ "${no_start}" -eq 0 ]] && command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || echo "警告：systemctl daemon-reload 失败，请手动复核单元状态" >&2
fi

if [[ "${purge}" -eq 1 ]]; then
    for path in "${config_dir}" "${state_dir}" "${log_dir}"; do
        if [[ -e "${path}" ]]; then
            rm -rf "${path}"
            echo "已删除：${path}"
        fi
    done
else
    echo "已保留配置：${config_dir}"
    echo "已保留状态：${state_dir}"
    echo "已保留日志：${log_dir}"
    echo "需要一并删除时加 --purge"
fi
