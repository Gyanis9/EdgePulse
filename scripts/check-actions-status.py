#!/usr/bin/env python3
"""查询 GitHub Actions 最近一次 run；缺 GITHUB_TOKEN 时 SKIP（退出 77）。

令牌只放进请求头，不写进任何输出。退出码：0 表示最近一次 run 成功；1 表示失败、
查询出错或没有 run；77 表示缺少令牌，无法查询托管 runner。
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8")

DEFAULT_REPO = "Gyanis9/EdgePulse"
DEFAULT_BRANCH = "main"
TIMEOUT_SECONDS = 20


def main() -> int:
    parser = argparse.ArgumentParser(description="查询 GitHub Actions 最近一次 run 的状态。")
    parser.add_argument("--repo", default=DEFAULT_REPO, help=f"仓库全名，默认 {DEFAULT_REPO}")
    parser.add_argument("--branch", default=DEFAULT_BRANCH, help=f"分支名，默认 {DEFAULT_BRANCH}")
    options = parser.parse_args()
    token = os.environ.get("GITHUB_TOKEN", "").strip()
    if not token:
        print("SKIP：缺少 GITHUB_TOKEN，无法查询托管 runner；请用仓库 Actions 页面人工确认")
        return 77
    url = f"https://api.github.com/repos/{options.repo}/actions/runs?branch={options.branch}&per_page=1"
    request = urllib.request.Request(
        url,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "User-Agent": "edgepulse-ci-status",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as failure:
        print(f"查询失败：HTTP {failure.code}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, TimeoutError, ValueError) as failure:
        print(f"查询失败：{failure}", file=sys.stderr)
        return 1
    if not isinstance(payload, dict):
        print("查询失败：响应不是 JSON 对象", file=sys.stderr)
        return 1
    runs = payload.get("workflow_runs", [])
    if not runs:
        print(f"没有找到 {options.repo} 分支 {options.branch} 的 workflow run", file=sys.stderr)
        return 1
    run = runs[0]
    print(
        f"{run.get('name')} #{run.get('run_number')} status={run.get('status')} "
        f"conclusion={run.get('conclusion')} url={run.get('html_url')}"
    )
    return 0 if run.get("conclusion") == "success" else 1


if __name__ == "__main__":
    sys.exit(main())
