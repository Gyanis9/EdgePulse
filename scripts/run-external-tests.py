#!/usr/bin/env python3
"""启动一次性 MySQL/Redis，运行环境门控用例，最后清理容器。

容器监听随机宿主端口，root 口令每次随机；脚本把它们写进 EDGEPULSE_TEST_MYSQL_*
与 EDGEPULSE_TEST_REDIS_* 再交给 ctest。加 --keep 在失败时保留容器，便于接着排查。
"""

from __future__ import annotations

import argparse
import os
import secrets
import subprocess
import sys
import time
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]


def run(args: list[str], *, env: dict[str, str] | None = None, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, env=env, check=True, text=True, capture_output=capture)


def docker(*args: str, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return run(["docker", *args], capture=capture)


def free_port(container: str, container_port: str) -> int:
    # 同一端口可能同时以 IPv4 与 IPv6 两行输出，两行的宿主端口相同，取最后一行即可
    text = docker("port", container, container_port, capture=True).stdout.strip()
    return int(text.splitlines()[-1].rsplit(":", 1)[1])


def wait_mysql(container: str, password: str) -> None:
    deadline = time.time() + 120
    while time.time() < deadline:
        result = subprocess.run(
            ["docker", "exec", "-e", f"MYSQL_PWD={password}", container, "mysqladmin", "ping", "-h", "127.0.0.1", "-uroot"],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if result.returncode == 0 and "mysqld is alive" in result.stdout:
            return
        time.sleep(1)
    raise RuntimeError("MySQL 未在 120 秒内就绪")


def wait_redis(container: str, password: str) -> None:
    deadline = time.time() + 60
    while time.time() < deadline:
        result = subprocess.run(
            ["docker", "exec", container, "redis-cli", "--no-auth-warning", "-a", password, "ping"],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if result.returncode == 0 and "PONG" in result.stdout:
            return
        time.sleep(1)
    raise RuntimeError("Redis 未在 60 秒内就绪")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--keep", action="store_true", help="失败时保留容器")
    parser.add_argument("--mysql-image", default="mysql:8.4")
    parser.add_argument("--redis-image", default="redis:7")
    options = parser.parse_args()

    suffix = f"{os.getpid()}-{secrets.token_hex(4)}"
    mysql_name = f"edgepulse-ext-mysql-{suffix}"
    redis_name = f"edgepulse-ext-redis-{suffix}"
    mysql_password = secrets.token_hex(16)
    redis_password = secrets.token_hex(16)
    created: list[str] = []
    try:
        docker(
            "run",
            "-d",
            "--rm",
            "--name",
            mysql_name,
            "-e",
            f"MYSQL_ROOT_PASSWORD={mysql_password}",
            "-e",
            "MYSQL_DATABASE=edgepulse_test",
            "-p",
            "127.0.0.1::3306",
            options.mysql_image,
        )
        created.append(mysql_name)
        docker(
            "run",
            "-d",
            "--rm",
            "--name",
            redis_name,
            "-p",
            "127.0.0.1::6379",
            options.redis_image,
            "redis-server",
            "--requirepass",
            redis_password,
        )
        created.append(redis_name)
        wait_mysql(mysql_name, mysql_password)
        wait_redis(redis_name, redis_password)
        env = os.environ.copy()
        env.update(
            {
                "EDGEPULSE_TEST_MYSQL_HOST": "127.0.0.1",
                "EDGEPULSE_TEST_MYSQL_PORT": str(free_port(mysql_name, "3306/tcp")),
                "EDGEPULSE_TEST_MYSQL_USER": "root",
                "EDGEPULSE_TEST_MYSQL_PASSWORD": mysql_password,
                "EDGEPULSE_TEST_MYSQL_DATABASE": "edgepulse_test",
                "EDGEPULSE_TEST_REDIS_HOST": "127.0.0.1",
                "EDGEPULSE_TEST_REDIS_PORT": str(free_port(redis_name, "6379/tcp")),
                "EDGEPULSE_TEST_REDIS_PASSWORD": redis_password,
            }
        )
        run(["ctest", "--preset", "debug", "--output-on-failure", "-R", "ExternalStores|RealtimeBus"], env=env)
        return 0
    except Exception as failure:
        print(f"外部依赖测试失败：{failure}", file=sys.stderr)
        if options.keep:
            print(f"保留容器：{mysql_name} {redis_name}", file=sys.stderr)
            created.clear()
        return 1
    finally:
        for name in created:
            subprocess.run(["docker", "rm", "-f", name], cwd=ROOT, capture_output=True, check=False)


if __name__ == "__main__":
    sys.exit(main())
