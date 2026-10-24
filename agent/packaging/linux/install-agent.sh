#!/usr/bin/env bash
# 安装 EdgePulse Agent：复制二进制与 systemd 单元、生成配置与环境文件模板，并按需启动服务；
# 加 --upgrade 时按健康检查窗口替换单个二进制，失败自动恢复上一版。
set -euo pipefail

usage() {
    cat <<'EOF'
用法：install-agent.sh [选项]
      install-agent.sh --upgrade 包 --health-timeout 秒 --state-dir DIR --prefix DIR

在解包后的安装包目录中运行。默认安装路径：
  二进制     /usr/local/bin/edgepulse-agent
  安装器副本 /usr/local/lib/edgepulse/install-agent.sh
  单元文件   /etc/systemd/system/edgepulse-agent.service
  配置目录   /etc/edgepulse-agent
  状态目录   /var/lib/edgepulse-agent
  日志目录   /var/log/edgepulse-agent

普通安装选项：
  --prefix DIR       安装前缀，二进制写入 DIR/bin（默认 /usr/local）
  --config-dir DIR   配置目录（默认 /etc/edgepulse-agent）
  --state-dir DIR    SQLite 状态、升级暂存与安装器日志目录（默认 /var/lib/edgepulse-agent）
  --log-dir DIR      日志目录（默认 /var/log/edgepulse-agent）
  --no-start         只安装文件，不执行 systemctl daemon-reload 与 enable --now
  -h, --help         显示本帮助

升级选项：
  --upgrade 包             升级包（tar.gz），包内 checksums.sha256 必须覆盖 edgepulse-agent
  --health-timeout 秒      等待新版本健康标记的秒数，必须为正整数
  --state-dir DIR          状态目录；健康标记与回滚标记位于该目录
  --prefix DIR             安装前缀；只替换 DIR/bin/edgepulse-agent

升级模式不接受 --no-start；--config-dir 与 --log-dir 在升级模式中不使用。
自动升级由 Agent 先对归档 SHA-256 验签再调用本脚本；手工 --upgrade 没有签名门槛，
只校验包内 checksums.sha256，归档来源需要调用方自行确认。

环境变量：
  SYSTEMD_UNIT_DIR   单元文件目录，默认 /etc/systemd/system；测试时指向临时目录

升级只替换二进制，配置、状态、安装器副本与 systemd 单元保持不变；日志写入
<state-dir>/update-installer.log。默认系统路径需要 root。已有 agent.yaml 与
agent.env 不会被覆盖。
EOF
}

die() {
    echo "错误：$*" >&2
    exit 1
}

require_realpath() {
    command -v realpath >/dev/null 2>&1 ||
        die "未找到 realpath（GNU coreutils）：无法规范化安装路径，请先安装 coreutils"
}

# 路径先规范化为逻辑绝对路径，再做校验与写入：realpath -m 折叠重复斜杠、. 与 ..，
# 且不要求目标已存在。校验值与实际使用值来自同一次规范化，避免两者不一致。
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

# 系统目录不接受为安装目标；各选项的文档默认值除外，默认安装仍使用 /usr/local
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

escape_sed_replacement() {
    printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'
}

prefix="/usr/local"
config_dir="/etc/edgepulse-agent"
state_dir="/var/lib/edgepulse-agent"
log_dir="/var/log/edgepulse-agent"
unit_dir="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"
no_start=0
upgrade_package=""
health_timeout=60

while [[ $# -gt 0 ]]; do
    case "$1" in
        --upgrade)
            [[ $# -ge 2 ]] || die "--upgrade 缺少安装包"
            upgrade_package="$2"
            shift 2
            ;;
        --health-timeout)
            [[ $# -ge 2 ]] || die "--health-timeout 缺少秒数"
            health_timeout="$2"
            shift 2
            ;;
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

if [[ -n "${upgrade_package}" && "${no_start}" -eq 1 ]]; then
    die "--upgrade 不接受 --no-start：升级必须启动服务并等待健康标记"
fi

# 升级日志与状态目录在解析参数后确定；日志固定写在状态目录，便于新旧两个版本共用
upgrade_log=""
upgrade_binary=""
upgrade_backup=""
upgrade_version=""

log_upgrade() {
    local line
    line="$(date -u +'%Y-%m-%dT%H:%M:%SZ') $*"
    printf '%s\n' "${line}" >> "${upgrade_log}"
    printf '%s\n' "${line}" >&2
}

# 替换前的失败不改二进制；启动器进程退出后旧服务可能已是停的，必须把旧版本重新拉起来
abort_before_replace() {
    local reason="$1"
    log_upgrade "升级失败：${reason}"
    if systemctl is-active --quiet edgepulse-agent.service; then
        log_upgrade "旧版本服务仍在运行，二进制未改动"
    elif systemctl start edgepulse-agent.service >>"${upgrade_log}" 2>&1; then
        log_upgrade "旧版本服务已重新启动，二进制未改动"
    else
        log_upgrade "错误：重新启动旧版本服务失败，请检查 systemctl status edgepulse-agent.service"
    fi
    exit 1
}

# 升级期间的所有失败都走这里：先停服务，再恢复备份、写回滚标记、启动旧版本
rollback_upgrade() {
    local reason="$1"
    # 备份尚未建立说明失败发生在替换之前，二进制没改动，但旧服务可能已被启动器带停
    if [[ -z "${upgrade_binary}" || ! -f "${upgrade_backup}" ]]; then
        abort_before_replace "${reason}"
    fi
    log_upgrade "升级失败：${reason}"
    systemctl stop edgepulse-agent.service >>"${upgrade_log}" 2>&1 || true

    if [[ -f "${upgrade_backup}" ]]; then
        if install -m 0755 -- "${upgrade_backup}" "${upgrade_binary}"; then
            rm -f -- "${upgrade_backup}" || log_upgrade "警告：删除旧二进制备份 ${upgrade_backup} 失败"
            log_upgrade "已恢复旧二进制：${upgrade_binary}"
        else
            log_upgrade "错误：恢复旧二进制失败，备份保留在 ${upgrade_backup}"
        fi
    else
        log_upgrade "错误：找不到旧二进制备份 ${upgrade_backup}"
    fi

    if [[ -n "${upgrade_version}" ]] && ! printf '%s\n' "${upgrade_version}" > "${state_dir}/update-rollback"; then
        log_upgrade "错误：写入回滚标记 ${state_dir}/update-rollback 失败"
    fi
    if ! systemctl start edgepulse-agent.service >>"${upgrade_log}" 2>&1; then
        log_upgrade "错误：启动旧版本服务失败，请检查 systemctl status edgepulse-agent.service"
    fi
    log_upgrade "回滚完成：目标版本 ${upgrade_version} 未通过健康检查"
    exit 1
}

# 升级模式只替换二进制；配置、状态、安装器副本与 unit 均不改动
run_upgrade() {
    local package="${upgrade_package}"
    local work_dir=""
    local extraction_dir=""
    local observed=""
    local elapsed=0

    mkdir -p -- "${state_dir}" || die "无法创建状态目录 ${state_dir}"
    upgrade_log="${state_dir}/update-installer.log"
    log_upgrade "开始升级：包 ${package}，前缀 ${prefix}，健康时限 ${health_timeout} 秒"

    for tool in tar sha256sum install systemctl; do
        command -v "${tool}" >/dev/null 2>&1 || rollback_upgrade "缺少 ${tool}"
    done
    [[ -f "${package}" ]] || rollback_upgrade "升级包不存在或不是普通文件：${package}"

    work_dir="$(mktemp -d "${TMPDIR:-/tmp}/edgepulse-agent-upgrade.XXXXXX")" || rollback_upgrade "无法创建临时目录"
    trap 'rm -rf "${work_dir}"' EXIT
    extraction_dir="${work_dir}/package"
    mkdir -p -- "${extraction_dir}"

    # 先拒绝绝对路径与 .. 路径段，再解包；tar 自身的解包顺序不应成为写入任意位置的机会
    local entry
    while IFS= read -r entry; do
        case "${entry}" in
            /*|..|../*|*/../*|*/..)
                rollback_upgrade "升级包包含不安全路径：${entry}"
                ;;
        esac
    done < <(tar -tzf "${package}")
    tar -xzf "${package}" -C "${extraction_dir}" >>"${upgrade_log}" 2>&1 || rollback_upgrade "解包升级包失败"

    for file in manifest.json checksums.sha256 edgepulse-agent; do
        [[ -f "${extraction_dir}/${file}" ]] || rollback_upgrade "升级包不完整，缺少 ${file}"
    done

    upgrade_version="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${extraction_dir}/manifest.json" | head -n 1)"
    [[ -n "${upgrade_version}" ]] || rollback_upgrade "manifest.json 缺少 version"
    [[ "${upgrade_version}" =~ ^[0-9]+(\.[0-9]+)*([-+][0-9A-Za-z.-]+)?$ ]] || rollback_upgrade "manifest.json 的 version 不合法：${upgrade_version}"

    # 平台名沿用打包脚本的映射：升级包由目标机自身架构打出，比较前先把 uname -m 归一
    local machine architecture expected_platform
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
    expected_platform="linux-${architecture}"

    local platform
    platform="$(sed -n 's/.*"platform"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${extraction_dir}/manifest.json" | head -n 1)"
    [[ "${platform}" == "${expected_platform}" ]] || rollback_upgrade "manifest.json 的 platform 不是 ${expected_platform}：${platform:-缺失}"

    # 自动升级的归档真实性由 Agent 启动本脚本前的签名检查建立：签名覆盖归档 SHA-256，
    # 因此下面的 checksums.sha256 校验的是已认证归档内的文件。手工 --upgrade 没有签名门槛，
    # 只按这里的哈希核对，调用方需要自行确认归档来源。
    grep -Eq '^[0-9a-fA-F]{64}[[:space:]]+\*?edgepulse-agent$' "${extraction_dir}/checksums.sha256" ||
        rollback_upgrade "checksums.sha256 没有覆盖 edgepulse-agent"
    if ! (cd "${extraction_dir}" && sha256sum -c --strict checksums.sha256) >>"${upgrade_log}" 2>&1; then
        rollback_upgrade "checksums.sha256 校验失败"
    fi

    upgrade_binary="${prefix}/bin/edgepulse-agent"
    upgrade_backup="${upgrade_binary}.previous"
    [[ -f "${upgrade_binary}" ]] || rollback_upgrade "找不到现有二进制 ${upgrade_binary}"
    # 旧标记必须在启动新版本之前清掉；清理失败时不替换二进制，由 abort_before_replace 保证旧服务仍在运行
    if ! rm -f -- "${state_dir}/update-health" "${state_dir}/update-rollback"; then
        abort_before_replace "无法删除旧的升级健康标记或回滚标记"
    fi

    if ! cp -p -- "${upgrade_binary}" "${upgrade_backup}"; then
        rm -f -- "${upgrade_backup}" 2>/dev/null || true
        abort_before_replace "备份现有二进制失败"
    fi
    log_upgrade "已备份现有二进制到 ${upgrade_backup}"

    log_upgrade "停止服务 edgepulse-agent.service"
    if ! systemctl stop edgepulse-agent.service >>"${upgrade_log}" 2>&1; then
        log_upgrade "警告：systemctl stop 返回非零（服务可能已停止），继续升级"
    fi

    install -m 0755 -- "${extraction_dir}/edgepulse-agent" "${upgrade_binary}" || rollback_upgrade "替换二进制失败"
    log_upgrade "已替换二进制，等待健康标记 ${state_dir}/update-health 出现版本 ${upgrade_version}"

    if ! systemctl start edgepulse-agent.service >>"${upgrade_log}" 2>&1; then
        rollback_upgrade "启动新版本服务失败"
    fi

    while (( elapsed < health_timeout )); do
        if [[ -f "${state_dir}/update-health" ]]; then
            observed="$(cat -- "${state_dir}/update-health" 2>/dev/null || true)"
            if [[ "${observed}" == "${upgrade_version}" ]]; then
                rm -f -- "${upgrade_backup}" "${state_dir}/update-health" || log_upgrade "警告：清理旧二进制备份或健康标记失败"
                log_upgrade "升级成功：版本 ${upgrade_version} 已通过健康检查"
                exit 0
            fi
            log_upgrade "健康标记内容为 ${observed}，等待版本 ${upgrade_version}"
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    rollback_upgrade "健康检查超时（${health_timeout} 秒内未看到版本 ${upgrade_version} 的健康标记）"
}

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
[[ "${health_timeout}" =~ ^[1-9][0-9]*$ ]] || die "--health-timeout 必须是正整数：${health_timeout}"

if [[ -n "${upgrade_package}" && "${EUID}" -ne 0 ]]; then
    die "升级需要 root：请用 sudo 运行，或把安装器交给服务管理进程执行"
fi

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

if [[ -n "${upgrade_package}" ]]; then
    run_upgrade
    exit 0
fi

package_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
binary_source="${package_dir}/edgepulse-agent"
installer_source="${package_dir}/install-agent.sh"
unit_source="${package_dir}/edgepulse-agent.service"
config_source="${package_dir}/agent.yaml.example"

for file in "${binary_source}" "${installer_source}" "${unit_source}" "${config_source}"; do
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

install -d -m 0755 "${prefix}/bin" "${prefix}/lib/edgepulse" "${unit_dir}"
install -d -m 0750 "${config_dir}" "${state_dir}" "${log_dir}"

if [[ -e "${config_dir}/agent.yaml" ]]; then
    echo "保留已有配置：${config_dir}/agent.yaml"
else
    sed \
        -e "s|/etc/edgepulse-agent|$(escape_sed_replacement "${config_dir}")|g" \
        -e "s|/var/lib/edgepulse-agent|$(escape_sed_replacement "${state_dir}")|g" \
        -e "s|/var/log/edgepulse-agent|$(escape_sed_replacement "${log_dir}")|g" \
        -e "s|/usr/local/lib/edgepulse|$(escape_sed_replacement "${prefix}/lib/edgepulse")|g" \
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
# Agent 的升级命令按配置里的 installer_script 启动安装器；普通安装把解包目录里的安装器复制到
# 前缀下，后续升级不必再依赖解包目录。直接跑已安装副本时源与目标相同，跳过复制
if [[ "${installer_source}" != "${prefix}/lib/edgepulse/install-agent.sh" ]]; then
    install -m 0755 -- "${installer_source}" "${prefix}/lib/edgepulse/install-agent.sh"
fi

echo "已安装二进制：${prefix}/bin/edgepulse-agent"
echo "已安装升级安装器：${prefix}/lib/edgepulse/install-agent.sh"
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
