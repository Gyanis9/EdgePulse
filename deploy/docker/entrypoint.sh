#!/usr/bin/env sh
# 容器入口：没有挂载生产配置时从样例复制一份，然后前台启动服务。
set -eu

if [ ! -f /app/config/prod.yaml ]; then
    cp /app/config/prod.yaml.example /app/config/prod.yaml
fi

exec /app/edgepulse-server --config /app/config/prod.yaml
