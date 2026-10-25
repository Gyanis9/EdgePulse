#!/usr/bin/env python3
"""启动一次性 MySQL/Redis，运行环境门控用例，最后清理容器。

容器只绑定 127.0.0.1 的随机宿主端口，容器口令每次随机生成；脚本把它们写进
EDGEPULSE_TEST_MYSQL_* 与 EDGEPULSE_TEST_REDIS_* 再交给 ctest。口令通过 docker run -e
写入容器，因此会出现在宿主进程参数与 docker inspect 的输出里；容器与口令都是一次性的，
这是仅用于测试的取舍，不要照搬到部署。

ctest 以 --no-tests=error 运行，并核对输出确实报告全部环境门控用例；正则零命中、目标缺失或
出现跳过标记都会让脚本以非零退出。加 --keep 在失败时保留容器，脚本会打印容器名与映射端口，便于用
docker logs 或客户端接着排查；正常日志不打印口令。
"""

from __future__ import annotations

import argparse
import os
import re
import secrets
import subprocess
import sys
import time
from pathlib import Path

for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]

CTEST_FILTER = r"^ExternalStores\.|^RealtimeBus\.Redis|^AgentCommandBus\.RedisFanout"
EXPECTED_TESTS = (
    "ExternalStores.MySqlAppliesMigrationsAndScopesLogs",
    "ExternalStores.RedisKeyValueStoreRoundTrip",
    "ExternalStores.RedisCacheProbeRecoversAfterConnectionLoss",
    "RealtimeBus.RedisFanoutReachesSecondInstance",
    "RealtimeBus.RedisPublisherRecoversAfterServerSideKill",
    "RealtimeBus.RedisSubscriberSurvivesMalformedEnvelopes",
    "AgentCommandBus.RedisFanoutReachesAgentHandlerOnSecondInstance",
)
SKIP_MARKERS = ("***Skipped", "The following tests did not run")


def run(args: list[str], *, env: dict[str, str] | None = None, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, env=env, check=True, text=True, capture_output=capture)


def docker(*args: str, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return run(["docker", *args], capture=capture)


def free_port(container: str, container_port: str) -> int:
    # 同一端口可能同时以 IPv4 与 IPv6 两行输出，两行的宿主端口相同，取最后一行即可
    text = docker("port", container, container_port, capture=True).stdout.strip()
    return int(text.splitlines()[-1].rsplit(":", 1)[1])


def kept_container(name: str, container_port: str) -> str:
    try:
        return f"{name}（127.0.0.1:{free_port(name, container_port)}）"
    except Exception:  # noqa: BLE001 - 保留现场提示不应掩盖原始失败
        return f"{name}（容器未运行，无法查询映射端口）"


def assert_expected_tests(output: str) -> None:
    """确认 ctest 真的执行了全部环境门控用例，而不是正则零命中、目标未构建或环境缺失被跳过。"""
    match = re.search(r"tests passed, \d+ tests failed out of (\d+)", output)
    if match is None:
        raise RuntimeError("ctest 输出没有用例计数，无法确认外部依赖用例已执行")
    count = int(match.group(1))
    if count != len(EXPECTED_TESTS):
        raise RuntimeError(
            f"ctest 只执行了 {count} 个用例，应为 {len(EXPECTED_TESTS)} 个；请检查测试目标是否改名、删除或未构建"
        )
    missing = [name for name in EXPECTED_TESTS if name not in output]
    if missing:
        raise RuntimeError(f"ctest 输出缺少用例：{'、'.join(missing)}")
    skipped = [marker for marker in SKIP_MARKERS if marker in output]
    if skipped:
        raise RuntimeError(
            f"ctest 输出包含跳过标记（{'、'.join(skipped)}）：环境门控用例必须真实执行，"
            "请检查 EDGEPULSE_TEST_MYSQL_* 与 EDGEPULSE_TEST_REDIS_* 是否已注入"
        )


def run_ctest(env: dict[str, str]) -> None:
    result = subprocess.run(
        ["ctest", "--preset", "debug", "--output-on-failure", "--no-tests=error", "-R", CTEST_FILTER],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
    )
    if result.stdout:
        sys.stdout.write(result.stdout)
        sys.stdout.flush()
    if result.stderr:
        sys.stderr.write(result.stderr)
        sys.stderr.flush()
    if result.returncode != 0:
        raise RuntimeError(f"ctest 退出码 {result.returncode}，外部依赖用例未全部通过")
    assert_expected_tests(result.stdout)


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
        run_ctest(env)
        return 0
    except Exception as failure:
        print(f"外部依赖测试失败：{failure}", file=sys.stderr)
        if options.keep:
            print(
                "保留容器："
                + kept_container(mysql_name, "3306/tcp")
                + " "
                + kept_container(redis_name, "6379/tcp"),
                file=sys.stderr,
            )
            print("用 docker logs 容器名 查看日志；口令不打印，需要时用 docker inspect 查看环境变量。", file=sys.stderr)
            created.clear()
        return 1
    finally:
        for name in created:
            subprocess.run(["docker", "rm", "-f", name], cwd=ROOT, capture_output=True, check=False)


if __name__ == "__main__":
    sys.exit(main())
