#!/usr/bin/env python3
"""查询 GitHub Actions 最近一次 run；缺 GITHUB_TOKEN 时 SKIP（退出 77）。

令牌只放进请求头，不写进任何输出；含控制字符的令牌在发请求前就被拒绝，错误分支只打印
固定文案，不回显异常文本。退出码：0 表示最近一次 run 成功；1 表示失败、查询出错或没有
run；77 表示缺少令牌，无法查询托管 runner。--self-test 用含换行的假令牌走一遍拒绝路径，
确认令牌值不出现在 stdout/stderr。
"""

from __future__ import annotations

import argparse
import contextlib
import http.client
import io
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8")

DEFAULT_REPO = "Gyanis9/EdgePulse"
DEFAULT_BRANCH = "main"
TIMEOUT_SECONDS = 20
CONTROL_CHARACTERS = re.compile(r"[\x00-\x1f\x7f-\x9f]")
SELF_TEST_TOKEN = "TOKEN_SELFTEST_MARKER\r\nSECOND_MARKER"


def contains_control_characters(text: str) -> bool:
    """判断文本是否含 C0、DEL 或 C1 控制字符。"""
    return CONTROL_CHARACTERS.search(text) is not None


def query_status(repo: str, branch: str, token: str) -> int:
    url = (
        f"https://api.github.com/repos/{urllib.parse.quote(repo, safe='/')}"
        f"/actions/runs?branch={urllib.parse.quote(branch, safe='/')}&per_page=1"
    )
    try:
        request = urllib.request.Request(
            url,
            headers={
                "Authorization": f"Bearer {token}",
                "Accept": "application/vnd.github+json",
                "User-Agent": "edgepulse-ci-status",
            },
        )
        with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as failure:
        print(f"查询失败：HTTP {failure.code}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, http.client.HTTPException, OSError):
        print("查询失败：网络错误或超时", file=sys.stderr)
        return 1
    except ValueError:
        print("查询失败：请求参数或响应格式无效", file=sys.stderr)
        return 1
    if not isinstance(payload, dict):
        print("查询失败：响应不是 JSON 对象", file=sys.stderr)
        return 1
    runs = payload.get("workflow_runs", [])
    if not runs:
        print(f"没有找到 {repo} 分支 {branch} 的 workflow run", file=sys.stderr)
        return 1
    run = runs[0]
    print(
        f"{run.get('name')} #{run.get('run_number')} status={run.get('status')} "
        f"conclusion={run.get('conclusion')} url={run.get('html_url')}"
    )
    return 0 if run.get("conclusion") == "success" else 1


def run_self_test() -> int:
    """用含控制字符的假令牌跑拒绝路径，断言令牌值没有出现在 stdout/stderr。"""
    previous = os.environ.get("GITHUB_TOKEN")
    os.environ["GITHUB_TOKEN"] = SELF_TEST_TOKEN
    stdout = io.StringIO()
    stderr = io.StringIO()
    try:
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = main([])
    finally:
        if previous is None:
            os.environ.pop("GITHUB_TOKEN", None)
        else:
            os.environ["GITHUB_TOKEN"] = previous
    captured = stdout.getvalue() + stderr.getvalue()
    leaked = [part for part in SELF_TEST_TOKEN.splitlines() if part and part in captured]
    if code != 1:
        print("自检失败：含控制字符的令牌没有被拒绝", file=sys.stderr)
        return 1
    if leaked:
        print("自检失败：令牌值出现在 stdout/stderr", file=sys.stderr)
        return 1
    print(f"自检通过：含控制字符的令牌被拒绝（退出码 {code}），stdout/stderr 未包含令牌值")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="查询 GitHub Actions 最近一次 run 的状态。")
    parser.add_argument("--repo", default=DEFAULT_REPO, help=f"仓库全名，默认 {DEFAULT_REPO}")
    parser.add_argument("--branch", default=DEFAULT_BRANCH, help=f"分支名，默认 {DEFAULT_BRANCH}")
    parser.add_argument("--self-test", action="store_true", help="用含控制字符的假令牌自检输出不泄漏令牌")
    options = parser.parse_args(argv)
    if options.self_test:
        return run_self_test()
    token = os.environ.get("GITHUB_TOKEN", "").strip()
    if not token:
        print("SKIP：缺少 GITHUB_TOKEN，无法查询托管 runner；请用仓库 Actions 页面人工确认")
        return 77
    if contains_control_characters(token):
        print("查询失败：GITHUB_TOKEN 含控制字符，已拒绝发送请求", file=sys.stderr)
        return 1
    return query_status(options.repo, options.branch, token)


if __name__ == "__main__":
    sys.exit(main())
