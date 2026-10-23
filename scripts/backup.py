#!/usr/bin/env python3
"""备份 EdgePulse 数据库、固件与非敏感配置，生成带 SHA-256 清单的归档。"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

FORMAT_VERSION = 1
DATABASE_ARCHIVE = {"sqlite": "db/edgepulse.db", "mysql": "db/edgepulse.sql"}
SKIPPED_DIRECTORIES = {".git", "logs", "log", "__pycache__"}
SENSITIVE_SUFFIXES = {".key", ".p12", ".pfx", ".jks", ".keystore"}
PRIVATE_KEY_NAMES = {"id_rsa", "id_dsa", "id_ecdsa", "id_ed25519"}
PRIVATE_KEY_MARKER = b"PRIVATE KEY-----"
CONFIG_SCAN_CHUNK_SIZE = 64 * 1024


@dataclass
class SkippedCounts:
    """记录备份时主动跳过的条目数量。"""

    links: int = 0
    logs: int = 0
    sensitive: int = 0


class BackupError(Exception):
    """备份失败时抛出的可报告错误。"""


class HashingReader:
    """按固定上限读取文件，同时累计 SHA-256 与实际读取长度。"""

    def __init__(self, raw: io.BufferedReader, limit: int) -> None:
        self._raw = raw
        self._remaining = limit
        self._digest = hashlib.sha256()
        self._size = 0

    def read(self, size: int = -1) -> bytes:
        if self._remaining <= 0:
            return b""
        if size < 0 or size > self._remaining:
            size = self._remaining
        data = self._raw.read(size)
        if data:
            self._digest.update(data)
            self._size += len(data)
            self._remaining -= len(data)
        return data

    def digest(self) -> str:
        return self._digest.hexdigest()

    def size(self) -> int:
        return self._size


def mysql_environment() -> dict[str, str]:
    """把数据库口令放进子进程环境，避免出现在命令行与日志里。"""
    environment = os.environ.copy()
    password = os.environ.get("EDGEPULSE_DB_PASSWORD")
    if password is None:
        environment.pop("MYSQL_PWD", None)
    else:
        environment["MYSQL_PWD"] = password
    return environment


def warn_if_mysql_password_missing() -> None:
    """口令环境变量缺失时说明将按免密方式连接，避免静默套用空口令。"""
    if "EDGEPULSE_DB_PASSWORD" not in os.environ:
        print("提示：未设置 EDGEPULSE_DB_PASSWORD，MySQL 将按免密方式连接。", file=sys.stderr)


def mysql_connection_arguments(args: argparse.Namespace) -> list[str]:
    return [
        "mysql",
        "-h",
        args.db_host,
        "-P",
        str(args.db_port),
        "-u",
        args.db_user,
    ]


def query_mysql_version(args: argparse.Namespace) -> int:
    command = mysql_connection_arguments(args) + [
        "--batch",
        "--skip-column-names",
        "--database",
        args.db_name,
        "-e",
        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations",
    ]
    try:
        result = subprocess.run(command, capture_output=True, text=True, env=mysql_environment())
    except FileNotFoundError as exc:
        raise BackupError("找不到 mysql 客户端，无法读取 MySQL 迁移版本") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or "mysql 返回非零退出码"
        raise BackupError(f"读取 MySQL 迁移版本失败：{detail}")
    value = result.stdout.strip()
    if not value.isdigit():
        raise BackupError(f"MySQL 返回的迁移版本不是非负整数：{value!r}")
    return int(value)


def dump_mysql(args: argparse.Namespace, target: Path) -> None:
    command = [
        "mysqldump",
        "--single-transaction",
        "--routines",
        "--triggers",
        "-h",
        args.db_host,
        "-P",
        str(args.db_port),
        "-u",
        args.db_user,
        args.db_name,
    ]
    try:
        with target.open("wb") as output:
            result = subprocess.run(command, stdout=output, stderr=subprocess.PIPE, env=mysql_environment())
    except FileNotFoundError as exc:
        raise BackupError("找不到 mysqldump，无法导出 MySQL") from exc
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise BackupError(f"mysqldump 执行失败：{detail or '退出码 ' + str(result.returncode)}")


def snapshot_sqlite(source_path: Path, target: Path) -> int:
    """用 SQLite 在线备份接口取一致快照，返回快照里的最高迁移版本。"""
    if not source_path.is_file():
        raise BackupError(f"SQLite 数据库文件不存在：{source_path}")
    if source_path.stat().st_size == 0:
        raise BackupError(f"SQLite 数据库文件为空：{source_path}")
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        source = sqlite3.connect(source_path.resolve().as_uri() + "?mode=ro", uri=True)
    except sqlite3.Error as exc:
        raise BackupError(f"无法打开 SQLite 数据库 {source_path}：{exc}") from exc
    try:
        destination = sqlite3.connect(str(target))
        try:
            try:
                source.backup(destination)
            except sqlite3.Error as exc:
                raise BackupError(f"SQLite 快照失败：{exc}") from exc
            try:
                row = destination.execute(
                    "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"
                ).fetchone()
            except sqlite3.Error as exc:
                raise BackupError(f"无法读取 SQLite 的 schema_migrations：{exc}") from exc
            version = int(row[0])
        finally:
            destination.close()
    finally:
        source.close()
    return version


def raise_walk_error(error: OSError) -> None:
    raise error


def contains_private_key(path: Path) -> bool:
    """分块扫描整个文件，证书在前时也不能把私钥漏出检测窗口。"""
    try:
        with path.open("rb") as handle:
            tail = b""
            while True:
                chunk = handle.read(CONFIG_SCAN_CHUNK_SIZE)
                if not chunk:
                    return False
                window = tail + chunk
                if PRIVATE_KEY_MARKER in window:
                    return True
                tail = window[-(len(PRIVATE_KEY_MARKER) - 1):]
    except OSError as exc:
        raise BackupError(f"无法读取配置候选文件 {path}：{exc}") from exc


def is_sensitive_config(path: Path) -> bool:
    name = path.name.lower()
    if name == "prod.yaml" or name == ".env" or name.endswith(".env"):
        return True
    if path.suffix.lower() in SENSITIVE_SUFFIXES:
        return True
    if name in PRIVATE_KEY_NAMES:
        return True
    return contains_private_key(path)


def collect_tree(
    root: Path,
    prefix: str,
    *,
    filter_config: bool,
    skipped: SkippedCounts,
) -> list[tuple[Path, str]]:
    if not root.is_dir():
        raise BackupError(f"目录不存在或不是目录：{root}")
    entries: list[tuple[Path, str]] = []
    try:
        for current, directories, files in os.walk(
            root, followlinks=False, onerror=raise_walk_error
        ):
            current_path = Path(current)
            kept_directories: list[str] = []
            for name in sorted(directories):
                if name in SKIPPED_DIRECTORIES:
                    continue
                if (current_path / name).is_symlink():
                    skipped.links += 1
                    continue
                kept_directories.append(name)
            directories[:] = kept_directories
            for name in sorted(files):
                path = current_path / name
                if name == ".git":
                    continue
                if path.is_symlink():
                    skipped.links += 1
                    continue
                if not path.is_file():
                    continue
                if name.endswith(".log") or name.endswith(".log.gz"):
                    skipped.logs += 1
                    continue
                if filter_config and is_sensitive_config(path):
                    skipped.sensitive += 1
                    continue
                relative = path.relative_to(root).as_posix()
                entries.append((path, f"{prefix}/{relative}"))
    except OSError as exc:
        raise BackupError(f"遍历目录失败 {root}：{exc}") from exc
    return entries


def create_archive(args: argparse.Namespace) -> tuple[Path, int, int, SkippedCounts]:
    output = Path(args.output)
    if output.exists():
        raise BackupError(f"归档文件已存在，拒绝覆盖：{output}")
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="edgepulse-backup-") as directory:
        staging = Path(directory)
        database_arcname = DATABASE_ARCHIVE[args.driver]
        database_file = staging / Path(database_arcname).name
        if args.driver == "sqlite":
            version = snapshot_sqlite(Path(args.db_path), database_file)
        else:
            version = query_mysql_version(args)
            dump_mysql(args, database_file)
        entries: list[tuple[Path, str]] = [(database_file, database_arcname)]
        skipped = SkippedCounts()
        entries.extend(
            collect_tree(
                Path(args.firmware_dir), "firmware", filter_config=False, skipped=skipped
            )
        )
        if args.config_dir:
            entries.extend(
                collect_tree(
                    Path(args.config_dir), "config", filter_config=True, skipped=skipped
                )
            )

        manifest_files: list[dict[str, str]] = []
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=output.name + ".", suffix=".tmp", dir=output.parent
        )
        os.close(descriptor)
        temporary_output = Path(temporary_name)
        try:
            with tarfile.open(temporary_output, "w:gz") as archive:
                for source, arcname in entries:
                    info = archive.gettarinfo(str(source), arcname=arcname)
                    if info is None:
                        raise BackupError(f"无法读取文件属性：{source}")
                    if source == database_file:
                        info.mode = 0o600
                    try:
                        with source.open("rb") as raw:
                            reader = HashingReader(raw, info.size)
                            archive.addfile(info, reader)
                            if reader.size() != info.size:
                                raise BackupError(f"读取文件时长度发生变化：{source}")
                    except OSError as exc:
                        raise BackupError(f"读取文件失败 {source}：{exc}") from exc
                    manifest_files.append({"path": arcname, "sha256": reader.digest()})

                manifest = {
                    "format": FORMAT_VERSION,
                    "createdAt": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                    "driver": args.driver,
                    "migrationVersion": version,
                    "files": manifest_files,
                }
                payload = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
                info = tarfile.TarInfo("manifest.json")
                info.size = len(payload)
                info.mode = 0o600
                info.mtime = int(time.time())
                archive.addfile(info, io.BytesIO(payload))
            os.chmod(temporary_output, 0o600)
            os.replace(temporary_output, output)
        except BackupError:
            temporary_output.unlink(missing_ok=True)
            raise
        except (OSError, tarfile.TarError, ValueError) as exc:
            temporary_output.unlink(missing_ok=True)
            raise BackupError(f"写入归档失败：{exc}") from exc
    return output, len(entries), version, skipped


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="备份 EdgePulse 数据库、固件与非敏感配置。")
    parser.add_argument("--driver", choices=("sqlite", "mysql"), required=True, help="数据库驱动")
    parser.add_argument("--db-path", help="SQLite 数据库文件（sqlite 必填）")
    parser.add_argument("--db-host", help="MySQL 主机（mysql 必填）")
    parser.add_argument("--db-port", type=int, default=3306, help="MySQL 端口，默认 3306")
    parser.add_argument("--db-name", help="MySQL 数据库名（mysql 必填）")
    parser.add_argument("--db-user", help="MySQL 用户名（mysql 必填）")
    parser.add_argument("--firmware-dir", required=True, help="固件目录")
    parser.add_argument("--config-dir", help="配置目录；会跳过敏感文件")
    parser.add_argument("--output", required=True, help="输出 tar.gz 归档")
    return parser


def validate_arguments(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if args.driver == "sqlite":
        if not args.db_path:
            parser.error("--driver sqlite 需要 --db-path")
    else:
        missing = [
            flag
            for flag, value in (
                ("--db-host", args.db_host),
                ("--db-name", args.db_name),
                ("--db-user", args.db_user),
            )
            if not value
        ]
        if missing:
            parser.error("--driver mysql 需要 " + "、".join(missing))
    if args.db_port <= 0 or args.db_port > 65535:
        parser.error("--db-port 必须在 1 到 65535 之间")


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    validate_arguments(parser, args)
    if args.driver == "mysql":
        warn_if_mysql_password_missing()
    try:
        output, file_count, version, skipped = create_archive(args)
    except BackupError as exc:
        print(f"备份失败：{exc}", file=sys.stderr)
        return 1
    except (OSError, sqlite3.Error) as exc:
        print(f"备份失败：{exc}", file=sys.stderr)
        return 1
    print(f"备份完成：{output}")
    print(f"文件数：{file_count}，迁移版本：{version}，驱动：{args.driver}")
    print(
        f"已跳过：符号链接 {skipped.links} 个，日志 {skipped.logs} 个，"
        f"敏感配置 {skipped.sensitive} 个"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
