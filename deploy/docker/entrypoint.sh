#!/usr/bin/env sh
# 容器入口：优先使用挂载的生产配置；没有时把样例复制到可写目录再启动。
# 设置 EDGEPULSE_INSTANCE_ID 时，把同一份只读配置复制到可写目录并写入唯一实例标识，
# 多个 HA 实例因此可以共享一份配置目录。
set -eu

source_config=/app/config/prod.yaml
if [ ! -f "${source_config}" ]; then
    source_config=/app/config/prod.yaml.example
fi
if [ ! -f "${source_config}" ]; then
    echo "找不到配置文件：/app/config/prod.yaml 或 /app/config/prod.yaml.example" >&2
    exit 1
fi

if [ -n "${EDGEPULSE_INSTANCE_ID:-}" ]; then
    instance_id=${EDGEPULSE_INSTANCE_ID}
    case "${instance_id}" in
        *[!A-Za-z0-9._-]*)
            echo "EDGEPULSE_INSTANCE_ID 只能包含字母、数字、点、下划线与连字符：${instance_id}" >&2
            exit 1
            ;;
    esac
    if [ "${#instance_id}" -gt 128 ]; then
        echo "EDGEPULSE_INSTANCE_ID 长度不能超过 128：${#instance_id}" >&2
        exit 1
    fi
    config_dir=/var/lib/edgepulse/runtime
    mkdir -p "${config_dir}"
    config="${config_dir}/prod-${instance_id}.yaml"
    if grep -Eq '^[[:space:]]*instance_id:' "${source_config}"; then
        awk -v id="${instance_id}" '{ if ($0 ~ /^[[:space:]]*instance_id:/) sub(/instance_id:.*/, "instance_id: " id); print }' "${source_config}" > "${config}"
    else
        # 没有 instance_id 时插到顶层 edgepulse: 之后；插入失败让启动直接失败，避免两个
        # 实例悄悄拿到同一个缺省标识
        awk -v id="${instance_id}" '{ print; if (!inserted && $0 ~ /^edgepulse:[[:space:]]*$/) { print "  instance_id: " id; inserted=1 } } END { if (!inserted) exit 3 }' \
            "${source_config}" > "${config}" || {
            echo "配置缺少顶层 edgepulse: 段，无法写入 EDGEPULSE_INSTANCE_ID" >&2
            exit 1
        }
    fi
    echo "已生成实例配置：instance=${instance_id} path=${config}"
else
    if [ -f /app/config/prod.yaml ]; then
        config=/app/config/prod.yaml
    else
        config=/var/lib/edgepulse/prod.yaml
        if [ ! -f "${config}" ]; then
            cp "${source_config}" "${config}"
        fi
    fi
fi

exec /app/edgepulse-server --config "${config}"
