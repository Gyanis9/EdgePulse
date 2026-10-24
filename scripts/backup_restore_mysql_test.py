#!/usr/bin/env python3
"""Docker MySQL 上的 backup.py/restore.py 往返测试。

启动一次性 MySQL 8.4 容器，写入迁移版本与探针数据，备份后篡改数据再恢复，
确认恢复值回到备份时的状态。测试只用 docker exec 执行 SQL，不依赖宿主机上的
mysql 或 mysqldump 客户端；容器在结束时始终删除。
"""

from __future__ import annotations

import argparse
import os
import secrets
import subprocess
import sys
import tempfile
import time
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[1]


def docker(*args: str, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["docker", *args], cwd=ROOT, text=True, check=True, capture_output=capture
    )


def wait_mysql(name: str, password: str) -> None:
    deadline = time.time() + 120
    while time.time() < deadline:
        result = subprocess.run(
            [
                "docker",
                "exec",
                "-e",
                f"MYSQL_PWD={password}",
                name,
                "mysqladmin",
                "ping",
                "-h",
                "127.0.0.1",
                "-uroot",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if result.returncode == 0 and "mysqld is alive" in result.stdout:
            return
        time.sleep(1)
    raise RuntimeError("MySQL 未就绪")


def exec_sql(name: str, password: str, database: str, sql: str) -> str:
    result = subprocess.run(
        [
            "docker",
            "exec",
            "-e",
            f"MYSQL_PWD={password}",
            name,
            "mysql",
            "-uroot",
            "-N",
            "-B",
            "--database",
            database,
            "-e",
            sql,
        ],
        cwd=ROOT,
        text=True,
        check=True,
        capture_output=True,
    )
    return result.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description="在一次性 MySQL 容器里验证备份与恢复。")
    parser.add_argument("--mysql-image", default="mysql:8.4", help="MySQL 镜像，默认 mysql:8.4")
    options = parser.parse_args()
    name = f"edgepulse-backup-mysql-{os.getpid()}-{secrets.token_hex(4)}"
    password = secrets.token_hex(16)
    database = "edgepulse_backup_test"
    try:
        docker(
            "run",
            "-d",
            "--rm",
            "--name",
            name,
            "-e",
            f"MYSQL_ROOT_PASSWORD={password}",
            "-e",
            f"MYSQL_DATABASE={database}",
            options.mysql_image,
            capture=True,
        )
        wait_mysql(name, password)
        exec_sql(
            name,
            password,
            database,
            "CREATE TABLE schema_migrations (version BIGINT PRIMARY KEY);"
            "INSERT INTO schema_migrations VALUES (19);"
            "CREATE TABLE probe (id BIGINT PRIMARY KEY, value VARCHAR(64));"
            "INSERT INTO probe VALUES (1, 'before');",
        )
        with tempfile.TemporaryDirectory(prefix="edgepulse-mysql-backup-") as directory:
            staging = Path(directory)
            # backup.py 要求固件目录；本用例只验证数据库，给一个空目录
            firmware = staging / "firmware"
            firmware.mkdir()
            archive = staging / "backup.tar.gz"
            environment = {**os.environ, "EDGEPULSE_DB_PASSWORD": password}
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "backup.py"),
                    "--driver",
                    "mysql",
                    "--mysql-container",
                    name,
                    "--db-name",
                    database,
                    "--db-user",
                    "root",
                    "--firmware-dir",
                    str(firmware),
                    "--output",
                    str(archive),
                ],
                cwd=ROOT,
                check=True,
                env=environment,
            )
            exec_sql(name, password, database, "UPDATE probe SET value = 'after' WHERE id = 1")
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "restore.py"),
                    "--archive",
                    str(archive),
                    "--driver",
                    "mysql",
                    "--mysql-container",
                    name,
                    "--db-name",
                    database,
                    "--db-user",
                    "root",
                    "--force",
                ],
                cwd=ROOT,
                check=True,
                env=environment,
            )
            value = exec_sql(name, password, database, "SELECT value FROM probe WHERE id = 1")
            if value != "before":
                print(f"恢复值错误：{value!r}", file=sys.stderr)
                return 1
        print("MySQL 备份/恢复往返通过")
        return 0
    finally:
        subprocess.run(
            ["docker", "rm", "-f", name], cwd=ROOT, capture_output=True, check=False
        )


if __name__ == "__main__":
    sys.exit(main())
