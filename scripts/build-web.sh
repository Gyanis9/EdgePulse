#!/usr/bin/env bash
# 构建前端并把产物刷新到后端托管的 web/；web/ 不进版本库。
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root_dir}/frontend"

npm ci
npm run build

rm -rf "${root_dir}/web"
cp -r dist "${root_dir}/web"
echo "前端产物已刷新到 ${root_dir}/web"
