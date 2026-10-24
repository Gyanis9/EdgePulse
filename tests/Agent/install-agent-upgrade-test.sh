#!/usr/bin/env bash
# 在 Linux 上验证 install-agent.sh 的升级成功、健康超时回滚与替换前失败重启旧服务：假 systemctl
# 记录服务状态并启停假 Agent 二进制，假二进制在收到健康开关时写健康标记。默认用于 ubuntu:24.04 容器。
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
installer="${repo_root}/agent/packaging/linux/install-agent.sh"

say() {
    printf '%s\n' "$*"
}

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [[ "$(uname -s)" != "Linux" ]]; then
    say "SKIP: 需要 Linux；请在 ubuntu:24.04 容器内运行"
    exit 0
fi
if [[ "${EUID}" -ne 0 ]]; then
    say "SKIP: 升级安装器需要 root；请在容器内以 root 运行"
    exit 0
fi

for tool in tar sha256sum realpath install; do
    command -v "${tool}" >/dev/null 2>&1 || fail "缺少工具 ${tool}"
done
[[ -f "${installer}" ]] || fail "找不到安装器 ${installer}"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
fakebin="${work}/fakebin"
mkdir -p "${fakebin}"

cat > "${fakebin}/systemctl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
action="${1:-}"
if [[ $# -ge 2 ]]; then
    shift
fi
state_file="${FAKE_AGENT_STATE_DIR:-}/service-state"
case "${action}" in
    is-active)
        [[ -n "${FAKE_AGENT_STATE_DIR:-}" && -f "${state_file}" && "$(cat "${state_file}")" == "active" ]]
        exit $?
        ;;
    stop)
        if [[ -n "${FAKE_AGENT_STATE_DIR:-}" ]]; then
            printf 'inactive\n' > "${state_file}"
        fi
        exit 0
        ;;
    start)
        if [[ -n "${FAKE_AGENT_PREFIX:-}" && -x "${FAKE_AGENT_PREFIX}/bin/edgepulse-agent" ]]; then
            printf 'active\n' > "${state_file}"
            sha256sum "${FAKE_AGENT_PREFIX}/bin/edgepulse-agent" >>"${FAKE_AGENT_STATE_DIR}/systemctl.log"
            "${FAKE_AGENT_PREFIX}/bin/edgepulse-agent" >>"${FAKE_AGENT_STATE_DIR}/fake-agent.log" 2>&1 &
        fi
        exit 0
        ;;
    *)
        exit 0
        ;;
esac
EOF
chmod 0755 "${fakebin}/systemctl"

# 构造一个只含二进制、manifest 与校验文件的最小升级包
make_package() {
    local version="$1"
    local output="$2"
    local stage="${work}/stage-${version}"
    mkdir -p "${stage}"
    cat > "${stage}/edgepulse-agent" <<EOF
#!/usr/bin/env bash
# fake edgepulse-agent ${version}
if [[ -f "\${FAKE_AGENT_STATE_DIR}/update-expected" ]]; then
    cp "\${FAKE_AGENT_STATE_DIR}/update-expected" "\${FAKE_AGENT_STATE_DIR}/expected-seen"
fi
if [[ -f "\${FAKE_AGENT_STATE_DIR}/healthy" ]]; then
    printf '%s\\n' "${version}" > "\${FAKE_AGENT_STATE_DIR}/update-health"
fi
exit 0
EOF
    chmod 0755 "${stage}/edgepulse-agent"
    local digest size
    digest="$(sha256sum "${stage}/edgepulse-agent" | cut -d' ' -f1)"
    size="$(wc -c < "${stage}/edgepulse-agent" | tr -d '[:space:]')"
    cat > "${stage}/manifest.json" <<EOF
{
  "version": "${version}",
  "platform": "linux-x86_64",
  "signingKeyId": "test-key",
  "signatureAlgorithm": "ed25519-sha256",
  "files": [
    {"path": "edgepulse-agent", "size": ${size}, "sha256": "${digest}"}
  ]
}
EOF
    (cd "${stage}" && sha256sum edgepulse-agent > checksums.sha256)
    tar -czf "${output}" -C "${stage}" edgepulse-agent manifest.json checksums.sha256
}

old_package="${work}/old-1.0.0.tar.gz"
new_package="${work}/new-2.0.0.tar.gz"
make_package "1.0.0" "${old_package}"
make_package "2.0.0" "${new_package}"
old_source="${work}/old-source"
mkdir -p "${old_source}"
tar -xzf "${old_package}" -C "${old_source}"

install_old_binary() {
    local prefix="$1"
    install -d -m 0755 "${prefix}/bin"
    install -m 0755 "${old_source}/edgepulse-agent" "${prefix}/bin/edgepulse-agent"
}

file_digest() {
    sha256sum "$1" | cut -d' ' -f1
}

# 成功路径：新二进制写健康标记，安装器保留新二进制并清理备份与标记
success_root="${work}/success"
success_prefix="${success_root}/prefix"
success_state="${success_root}/state"
mkdir -p "${success_state}"
install_old_binary "${success_prefix}"
touch "${success_state}/healthy"
success_old_digest="$(file_digest "${success_prefix}/bin/edgepulse-agent")"

set +e
PATH="${fakebin}:${PATH}" FAKE_AGENT_PREFIX="${success_prefix}" FAKE_AGENT_STATE_DIR="${success_state}" \
    "${installer}" --upgrade "${new_package}" --health-timeout 5 --state-dir "${success_state}" --prefix "${success_prefix}" \
    >"${success_root}/stdout.log" 2>&1
success_rc=$?
set -e

if [[ "${success_rc}" -ne 0 ]]; then
    cat "${success_state}/update-installer.log" >&2 || true
    fail "成功路径应返回 0，实际 ${success_rc}"
fi
[[ "$(file_digest "${success_prefix}/bin/edgepulse-agent")" != "${success_old_digest}" ]] || fail "二进制未被新版本替换"
grep -q 'fake edgepulse-agent 2.0.0' "${success_prefix}/bin/edgepulse-agent" || fail "替换后的二进制不是新版本"
[[ ! -e "${success_prefix}/bin/edgepulse-agent.previous" ]] || fail "成功后应删除二进制备份"
[[ ! -e "${success_state}/update-health" ]] || fail "成功后应删除健康标记"
[[ ! -e "${success_state}/update-expected" ]] || fail "成功后应删除期望版本标记"
[[ "$(cat "${success_state}/expected-seen" 2>/dev/null || true)" == "2.0.0" ]] || fail "新版本未在启动前看到 update-expected"
grep -q "升级成功" "${success_state}/update-installer.log" || fail "升级日志缺少成功记录"
say "PASS: 成功路径替换二进制、等待健康标记并清理备份"
say "成功路径日志：${success_state}/update-installer.log"
grep -E "已备份|已替换|升级成功" "${success_state}/update-installer.log" | sed 's/^/  /'

# 超时路径：新二进制不写健康标记，安装器恢复旧二进制并写回滚标记
timeout_root="${work}/timeout"
timeout_prefix="${timeout_root}/prefix"
timeout_state="${timeout_root}/state"
mkdir -p "${timeout_state}"
install_old_binary "${timeout_prefix}"
timeout_old_digest="$(file_digest "${timeout_prefix}/bin/edgepulse-agent")"

set +e
PATH="${fakebin}:${PATH}" FAKE_AGENT_PREFIX="${timeout_prefix}" FAKE_AGENT_STATE_DIR="${timeout_state}" \
    "${installer}" --upgrade "${new_package}" --health-timeout 2 --state-dir "${timeout_state}" --prefix "${timeout_prefix}" \
    >"${timeout_root}/stdout.log" 2>&1
timeout_rc=$?
set -e

if [[ "${timeout_rc}" -eq 0 ]]; then
    cat "${timeout_state}/update-installer.log" >&2 || true
    fail "健康检查超时后安装器应返回非零"
fi
[[ "$(file_digest "${timeout_prefix}/bin/edgepulse-agent")" == "${timeout_old_digest}" ]] || fail "超时后未恢复旧二进制"
[[ -f "${timeout_state}/update-rollback" ]] || fail "超时后缺少 update-rollback"
[[ "$(cat "${timeout_state}/update-rollback")" == "2.0.0" ]] || fail "回滚标记内容不是目标版本"
[[ ! -e "${timeout_prefix}/bin/edgepulse-agent.previous" ]] || fail "回滚后应删除二进制备份"
[[ ! -e "${timeout_state}/update-expected" ]] || fail "回滚后应删除期望版本标记"
[[ "$(cat "${timeout_state}/expected-seen" 2>/dev/null || true)" == "2.0.0" ]] || fail "新版本未在启动前看到 update-expected"
grep -q "回滚完成" "${timeout_state}/update-installer.log" || fail "升级日志缺少回滚记录"
say "PASS: 超时路径恢复旧二进制、写入回滚标记并以非零退出"
say "超时路径日志：${timeout_state}/update-installer.log，回滚标记内容 $(cat "${timeout_state}/update-rollback")"
grep -E "升级失败|已恢复旧二进制|回滚完成" "${timeout_state}/update-installer.log" | sed 's/^/  /'

# 替换前失败：坏包（校验不符）时旧服务已随启动器退出而停着，安装器必须把它重新拉起来
corrupt_root="${work}/corrupt"
corrupt_prefix="${corrupt_root}/prefix"
corrupt_state="${corrupt_root}/state"
corrupt_package="${work}/corrupt-3.0.0.tar.gz"
corrupt_stage="${work}/corrupt-stage"
mkdir -p "${corrupt_state}" "${corrupt_stage}"
install_old_binary "${corrupt_prefix}"
corrupt_old_digest="$(file_digest "${corrupt_prefix}/bin/edgepulse-agent")"
make_package "3.0.0" "${corrupt_package}"
tar -xzf "${corrupt_package}" -C "${corrupt_stage}"
printf '\n# tampered after checksums were written\n' >> "${corrupt_stage}/edgepulse-agent"
(cd "${corrupt_stage}" && tar -czf "${corrupt_package}" edgepulse-agent manifest.json checksums.sha256)
# 启动器进程退出后旧服务处于停止状态
printf 'inactive\n' > "${corrupt_state}/service-state"

set +e
PATH="${fakebin}:${PATH}" FAKE_AGENT_PREFIX="${corrupt_prefix}" FAKE_AGENT_STATE_DIR="${corrupt_state}" \
    "${installer}" --upgrade "${corrupt_package}" --health-timeout 2 --state-dir "${corrupt_state}" --prefix "${corrupt_prefix}" \
    >"${corrupt_root}/stdout.log" 2>&1
corrupt_rc=$?
set -e

if [[ "${corrupt_rc}" -eq 0 ]]; then
    cat "${corrupt_state}/update-installer.log" >&2 || true
    fail "校验失败时安装器应返回非零"
fi
[[ "$(file_digest "${corrupt_prefix}/bin/edgepulse-agent")" == "${corrupt_old_digest}" ]] || fail "校验失败时二进制不应被替换"
[[ "$(cat "${corrupt_state}/service-state")" == "active" ]] || fail "替换前失败后旧服务未重新启动"
[[ -f "${corrupt_state}/systemctl.log" ]] || fail "替换前失败后未记录服务启动"
[[ "$(cut -d' ' -f1 "${corrupt_state}/systemctl.log" | head -n 1)" == "${corrupt_old_digest}" ]] || fail "重新启动的服务用的不是旧二进制"
grep -q "旧版本服务已重新启动" "${corrupt_state}/update-installer.log" || fail "升级日志缺少旧服务重新启动记录"
if grep -q "服务与二进制未改动" "${corrupt_state}/update-installer.log"; then
    fail "替换前失败时不应声称服务与二进制未改动"
fi
[[ ! -e "${corrupt_state}/update-rollback" ]] || fail "替换前失败不应写回滚标记"
[[ ! -e "${corrupt_state}/update-expected" ]] || fail "替换前失败不应留下期望版本标记"
say "PASS: 校验失败时安装器以非零退出并用旧二进制重新启动旧服务"
say "校验失败路径日志：${corrupt_state}/update-installer.log"
grep -E "升级失败|旧版本服务已重新启动" "${corrupt_state}/update-installer.log" | sed 's/^/  /'

# 普通安装：复制安装器到前缀并替换配置里的安装器路径
normal_stage="${work}/normal-package"
normal_root="${work}/normal"
mkdir -p "${normal_stage}" "${normal_root}"
cp "${installer}" "${normal_stage}/install-agent.sh"
cp "${repo_root}/agent/packaging/linux/uninstall-agent.sh" "${normal_stage}/uninstall-agent.sh"
cp "${repo_root}/agent/packaging/linux/edgepulse-agent.service" "${normal_stage}/edgepulse-agent.service"
cp "${repo_root}/agent/packaging/config/agent.yaml.example" "${normal_stage}/agent.yaml.example"
install -m 0755 "${old_source}/edgepulse-agent" "${normal_stage}/edgepulse-agent"
chmod 0755 "${normal_stage}/install-agent.sh"

SYSTEMD_UNIT_DIR="${normal_root}/unit" "${normal_stage}/install-agent.sh" \
    --prefix "${normal_root}/prefix" \
    --config-dir "${normal_root}/config" \
    --state-dir "${normal_root}/state" \
    --log-dir "${normal_root}/log" \
    --no-start \
    >"${normal_root}/stdout.log" 2>&1 || {
    cat "${normal_root}/stdout.log" >&2 || true
    fail "普通安装失败"
}

[[ -x "${normal_root}/prefix/lib/edgepulse/install-agent.sh" ]] || fail "普通安装未复制安装器到前缀"
grep -q "installer_script: ${normal_root}/prefix/lib/edgepulse/install-agent.sh" "${normal_root}/config/agent.yaml" ||
    fail "配置里的 installer_script 未替换为自定义前缀"
if grep -q '/usr/local/lib/edgepulse' "${normal_root}/config/agent.yaml"; then
    fail "配置里仍残留默认安装器路径"
fi
say "PASS: 普通安装复制安装器并替换配置里的前缀"

SYSTEMD_UNIT_DIR="${normal_root}/unit" bash "${normal_stage}/uninstall-agent.sh" \
    --prefix "${normal_root}/prefix" \
    --config-dir "${normal_root}/config" \
    --state-dir "${normal_root}/state" \
    --log-dir "${normal_root}/log" \
    --no-start \
    >"${normal_root}/uninstall.log" 2>&1 || {
    cat "${normal_root}/uninstall.log" >&2 || true
    fail "卸载失败"
}
[[ ! -e "${normal_root}/prefix/bin/edgepulse-agent" ]] || fail "卸载后二进制仍存在"
[[ ! -e "${normal_root}/prefix/lib/edgepulse/install-agent.sh" ]] || fail "卸载后安装器副本仍存在"
[[ -f "${normal_root}/config/agent.yaml" ]] || fail "卸载默认应保留配置"
say "PASS: 卸载删除二进制与安装器副本并保留配置"

if [[ -n "${EDGEPULSE_INSTALLER_TEST_ARTIFACTS:-}" ]]; then
    mkdir -p "${EDGEPULSE_INSTALLER_TEST_ARTIFACTS}"
    cp "${success_state}/update-installer.log" "${EDGEPULSE_INSTALLER_TEST_ARTIFACTS}/success-update-installer.log"
    cp "${timeout_state}/update-installer.log" "${EDGEPULSE_INSTALLER_TEST_ARTIFACTS}/timeout-update-installer.log"
    cp "${timeout_state}/update-rollback" "${EDGEPULSE_INSTALLER_TEST_ARTIFACTS}/timeout-update-rollback"
    cp "${corrupt_state}/update-installer.log" "${EDGEPULSE_INSTALLER_TEST_ARTIFACTS}/corrupt-update-installer.log"
    cp "${corrupt_state}/systemctl.log" "${EDGEPULSE_INSTALLER_TEST_ARTIFACTS}/corrupt-systemctl.log"
    cp "${normal_root}/config/agent.yaml" "${EDGEPULSE_INSTALLER_TEST_ARTIFACTS}/normal-agent.yaml"
    say "安装器日志已复制到 ${EDGEPULSE_INSTALLER_TEST_ARTIFACTS}"
fi

say "ALL PASS"
