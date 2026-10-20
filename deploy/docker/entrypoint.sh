#!/usr/bin/env sh
# 容器入口：优先使用挂载的生产配置；没有时把样例复制到可写目录再启动。
set -eu

config=/app/config/prod.yaml
if [ ! -f "${config}" ]; then
    config=/var/lib/edgepulse/prod.yaml
    if [ ! -f "${config}" ]; then
        cp /app/config/prod.yaml.example "${config}"
    fi
fi

exec /app/edgepulse-server --config "${config}"
