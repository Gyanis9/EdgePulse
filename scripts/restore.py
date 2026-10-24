#!/usr/bin/env python3
"""校验 EdgePulse 备份归档并恢复数据库、固件与非敏感配置。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

FORMAT_VERSION = 1
CHUNK_SIZE = 1024 * 1024
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
DATABASE_ARCHIVE = {"sqlite": "db/edgepulse.db", "mysql": "db/edgepulse.sql"}
SHA256_PATTERN = re.compile(r"[0-9a-fA-F]{64}")
SQLITE_SIDECAR_SUFFIXES = ("-wal", "-shm", "-journal")
WINDOWS_FORBIDDEN_CHARACTERS = '<>:"|?*'
WINDOWS_RESERVED_NAMES = {"CON", "PRN", "AUX", "NUL"} | {
    f"{prefix}{index}" for prefix in ("COM", "LPT") for index in range(1, 10)
}


class RestoreError(Exception):
    """恢复失败时抛出的可报告错误。"""


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


def query_mysql(args: argparse.Namespace, query: str) -> str:
    command = mysql_connection_arguments(args) + [
        "--batch",
        "--skip-column-names",
        "--database",
        args.db_name,
        "-e",
        query,
    ]
    try:
        result = subprocess.run(command, capture_output=True, text=True, env=mysql_environment())
    except FileNotFoundError as exc:
        raise RestoreError("找不到 mysql 客户端，无法检查或恢复 MySQL") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or f"mysql 退出码 {result.returncode}"
        raise RestoreError(f"查询 MySQL 失败：{detail}")
    return result.stdout.strip()


def mysql_target_state(args: argparse.Namespace) -> tuple[int, bool]:
    """返回目标库的最高迁移版本与是否已有表。"""
    tables = int(
        query_mysql(
            args, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE()"
        )
    )
    if tables == 0:
        return 0, False
    migrations = int(
        query_mysql(
            args,
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = DATABASE() AND table_name = 'schema_migrations'",
        )
    )
    if migrations == 0:
        return 0, True
    value = query_mysql(args, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations")
    if not value.isdigit():
        raise RestoreError(f"MySQL 返回的迁移版本不是非负整数：{value!r}")
    return int(value), True


def sqlite_target_state(path: Path) -> tuple[int, bool]:
    """返回目标库的最高迁移版本与是否已有内容。"""
    if not path.exists():
        return 0, False
    if not path.is_file():
        raise RestoreError(f"目标数据库路径不是文件：{path}")
    if path.stat().st_size == 0:
        return 0, False
    try:
        connection = sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True)
    except sqlite3.Error as exc:
        raise RestoreError(f"无法打开目标 SQLite 数据库 {path}：{exc}") from exc
    try:
        try:
            row = connection.execute(
                "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"
            ).fetchone()
        except sqlite3.Error as exc:
            if "no such table" in str(exc).lower():
                return 0, True
            raise RestoreError(f"无法读取目标库的 schema_migrations：{exc}") from exc
        return int(row[0]), True
    finally:
        connection.close()


def collect_regular_members(tar: tarfile.TarFile) -> dict[str, tarfile.TarInfo]:
    regular: dict[str, tarfile.TarInfo] = {}
    for member in tar.getmembers():
        if member.isdir():
            continue
        if not member.isfile():
            raise RestoreError(f"归档包含不支持的文件类型：{member.name}")
        if member.name in regular:
            raise RestoreError(f"归档成员重复：{member.name}")
        regular[member.name] = member
    return regular


def validate_archive_path(value: object) -> PurePosixPath:
    if not isinstance(value, str) or not value:
        raise RestoreError("manifest.json 中的文件路径不是非空字符串")
    if "\\" in value:
        raise RestoreError(f"manifest.json 中的路径含反斜杠：{value}")
    path = PurePosixPath(value)
    if str(path) != value or path.is_absolute():
        raise RestoreError(f"manifest.json 中的路径未规范化：{value}")
    for part in path.parts:
        if part in ("", ".", ".."):
            raise RestoreError(f"manifest.json 中的路径越出归档目录：{value}")
        if any(
            character in WINDOWS_FORBIDDEN_CHARACTERS or ord(character) < 32
            for character in part
        ):
            raise RestoreError(f"manifest.json 中的路径含 Windows 非法字符：{value}")
        if part.endswith((".", " ")):
            raise RestoreError(f"manifest.json 中的路径分段以点或空格结尾：{value}")
        if part.split(".")[0].upper() in WINDOWS_RESERVED_NAMES:
            raise RestoreError(f"manifest.json 中的路径使用 Windows 保留设备名：{value}")
    if path.parts[0] not in ("db", "firmware", "config"):
        raise RestoreError(f"manifest.json 中的路径不在允许的目录下：{value}")
    return path


def load_manifest(
    tar: tarfile.TarFile, driver: str
) -> tuple[dict[str, object], dict[str, tarfile.TarInfo], list[tuple[str, str]]]:
    regular = collect_regular_members(tar)
    manifest_member = regular.pop("manifest.json", None)
    if manifest_member is None:
        raise RestoreError("归档缺少 manifest.json")
    if manifest_member.size > MAX_MANIFEST_BYTES:
        raise RestoreError("manifest.json 过大，拒绝解析")
    handle = tar.extractfile(manifest_member)
    if handle is None:
        raise RestoreError("无法读取 manifest.json")
    with handle:
        raw = handle.read()
    try:
        manifest = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RestoreError(f"manifest.json 不是有效的 UTF-8 JSON：{exc}") from exc
    if not isinstance(manifest, dict):
        raise RestoreError("manifest.json 顶层必须是对象")
    archive_format = manifest.get("format")
    if isinstance(archive_format, bool) or archive_format != FORMAT_VERSION:
        raise RestoreError(f"不支持的归档格式：{archive_format!r}")
    archived_driver = manifest.get("driver")
    if archived_driver != driver:
        raise RestoreError(f"归档驱动 {archived_driver!r} 与 --driver {driver} 不一致")
    if not isinstance(manifest.get("createdAt"), str):
        raise RestoreError("manifest.json 缺少 createdAt")
    version = manifest.get("migrationVersion")
    if isinstance(version, bool) or not isinstance(version, int) or version < 0:
        raise RestoreError("manifest.json 的 migrationVersion 必须是非负整数")
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise RestoreError("manifest.json 的 files 必须是非空数组")

    entries: list[tuple[str, str]] = []
    seen: set[str] = set()
    for item in files:
        if not isinstance(item, dict):
            raise RestoreError("manifest.json 的 files 项必须是对象")
        path = validate_archive_path(item.get("path"))
        name = str(path)
        digest = item.get("sha256")
        if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest):
            raise RestoreError(f"manifest.json 中 {name} 的 sha256 不是 64 位十六进制")
        if name in seen:
            raise RestoreError(f"manifest.json 重复登记文件：{name}")
        if name not in regular:
            raise RestoreError(f"清单列出的文件不在归档中：{name}")
        if name.startswith("db/") and name != DATABASE_ARCHIVE[driver]:
            raise RestoreError(f"归档数据库文件与驱动不符：{name}")
        entries.append((name, digest.lower()))
        seen.add(name)

    database_arcname = DATABASE_ARCHIVE[driver]
    if database_arcname not in seen:
        raise RestoreError(f"归档缺少数据库文件：{database_arcname}")
    unknown = sorted(set(regular) - seen)
    if unknown:
        raise RestoreError("归档包含清单未登记的文件：" + "、".join(unknown[:5]))
    return manifest, regular, entries


def hash_member(tar: tarfile.TarFile, member: tarfile.TarInfo) -> str:
    digest = hashlib.sha256()
    size = 0
    handle = tar.extractfile(member)
    if handle is None:
        raise RestoreError(f"无法读取归档文件：{member.name}")
    with handle:
        while True:
            chunk = handle.read(CHUNK_SIZE)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
    if size != member.size:
        raise RestoreError(f"归档文件长度与头部记录不符：{member.name}")
    return digest.hexdigest()


def verify_entries(
    tar: tarfile.TarFile,
    regular: dict[str, tarfile.TarInfo],
    entries: list[tuple[str, str]],
) -> None:
    for name, expected in entries:
        if hash_member(tar, regular[name]) != expected:
            raise RestoreError(f"文件校验失败：{name} 的 SHA-256 与清单不符")


def clamp_mode(mode: int) -> int:
    """把归档权限收敛到 0600 或 0644，不保留执行位与组/其他写位。"""
    return 0o600 if mode & 0o077 == 0 else 0o644


def ensure_target_within(destination: Path, target: Path, name: str) -> None:
    """确认落点仍在暂存目录内，拦截盘符与备选数据流写法。"""
    if not target.resolve().is_relative_to(destination.resolve()):
        raise RestoreError(f"归档路径越出暂存目录：{name}")


def extract_entries(
    tar: tarfile.TarFile,
    regular: dict[str, tarfile.TarInfo],
    entries: list[tuple[str, str]],
    destination: Path,
) -> None:
    for name, expected in entries:
        member = regular[name]
        target = destination.joinpath(*PurePosixPath(name).parts)
        ensure_target_within(destination, target, name)
        target.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        size = 0
        try:
            handle = tar.extractfile(member)
            if handle is None:
                raise RestoreError(f"无法读取归档文件：{name}")
            with handle, target.open("wb") as output:
                while True:
                    chunk = handle.read(CHUNK_SIZE)
                    if not chunk:
                        break
                    digest.update(chunk)
                    size += len(chunk)
                    output.write(chunk)
            if size != member.size:
                raise RestoreError(f"归档文件长度与头部记录不符：{name}")
            if digest.hexdigest() != expected:
                raise RestoreError(f"解包时校验失败：{name} 的 SHA-256 与清单不符")
            os.chmod(target, clamp_mode(member.mode))
        except OSError as exc:
            raise RestoreError(f"解包文件失败 {name}：{exc}") from exc


def directory_has_entries(path: Path) -> bool:
    if not path.exists():
        return False
    if not path.is_dir():
        raise RestoreError(f"目标路径不是目录：{path}")
    return next(path.iterdir(), None) is not None


def inspect_targets(
    args: argparse.Namespace,
    manifest: dict[str, object],
    entries: list[tuple[str, str]],
) -> tuple[int, list[str], list[tuple[str, str]], list[tuple[str, str]]]:
    """核对目标库版本与覆盖风险，返回版本、冲突项与固件/配置清单。"""
    if args.driver == "sqlite":
        target_version, non_empty = sqlite_target_state(Path(args.db_path))
        database_label = f"SQLite 数据库 {args.db_path}"
    else:
        target_version, non_empty = mysql_target_state(args)
        database_label = f"MySQL 数据库 {args.db_name}"
    if target_version > int(manifest["migrationVersion"]):
        raise RestoreError(
            f"目标数据库迁移版本 {target_version} 高于归档版本 "
            f"{manifest['migrationVersion']}，拒绝降级恢复"
        )
    conflicts = [database_label] if non_empty else []

    firmware_entries = [entry for entry in entries if entry[0].startswith("firmware/")]
    config_entries = [entry for entry in entries if entry[0].startswith("config/")]
    if firmware_entries:
        if not args.firmware_dir:
            raise RestoreError("归档包含固件，请用 --firmware-dir 指定恢复目录")
        firmware_dir = Path(args.firmware_dir)
        if directory_has_entries(firmware_dir):
            conflicts.append(str(firmware_dir))
        backup = firmware_dir.with_name(firmware_dir.name + ".bak")
        if backup.exists():
            raise RestoreError(f"固件旧目录已存在，请先人工处理：{backup}")
    if config_entries:
        if not args.config_dir:
            raise RestoreError("归档包含配置，请用 --config-dir 指定恢复目录")
        config_dir = Path(args.config_dir)
        for name, _ in config_entries:
            relative = PurePosixPath(name).relative_to("config")
            destination = config_dir.joinpath(*relative.parts)
            if destination.exists():
                conflicts.append(str(destination))
    return target_version, conflicts, firmware_entries, config_entries


def sqlite_sidecar_paths(target: Path) -> list[Path]:
    return [target.with_name(target.name + suffix) for suffix in SQLITE_SIDECAR_SUFFIXES]


def remove_sqlite_sidecars(target: Path) -> None:
    """删除目标库旁的 WAL、共享内存与回滚日志。"""
    for sidecar in sqlite_sidecar_paths(target):
        try:
            sidecar.unlink(missing_ok=True)
        except OSError as exc:
            raise RestoreError(
                f"无法删除目标数据库的边车文件 {sidecar}：{exc}；"
                "请先停止 EdgePulse 服务再重试"
            ) from exc


def ensure_sqlite_sidecars_absent(target: Path) -> None:
    remaining = [sidecar for sidecar in sqlite_sidecar_paths(target) if sidecar.exists()]
    if remaining:
        raise RestoreError(
            "恢复后仍存在数据库边车文件："
            + "、".join(str(path) for path in remaining)
            + "；请停止 EdgePulse 服务后重试"
        )


def replace_sqlite(source: Path, target: Path, force: bool) -> None:
    if target.exists():
        if not target.is_file():
            raise RestoreError(f"目标数据库路径不是文件：{target}")
        if target.stat().st_size > 0 and not force:
            raise RestoreError(f"目标数据库非空，需要 --force 才能覆盖：{target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{target.name}.restore-", dir=target.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output, source.open("rb") as handle:
            shutil.copyfileobj(handle, output, CHUNK_SIZE)
            output.flush()
            os.fsync(output.fileno())
        # 数据库固定 0600：沿用目标旧权限会把 0640 收敛成 0644，反而放宽到组与其他用户可读
        os.chmod(temporary, 0o600)
        # 引擎固定 journal_mode=WAL。残留的 -wal/-shm/-journal 会在下次打开时按旧日志重放，
        # 把恢复前的页盖回刚替换的快照；删不掉说明服务仍在运行，必须直接失败。
        remove_sqlite_sidecars(target)
        os.replace(temporary, target)
        ensure_sqlite_sidecars_absent(target)
    except OSError as exc:
        raise RestoreError(f"替换 SQLite 数据库失败：{exc}") from exc
    finally:
        temporary.unlink(missing_ok=True)


def import_mysql(source: Path, args: argparse.Namespace) -> None:
    command = mysql_connection_arguments(args) + ["--database", args.db_name]
    try:
        with source.open("rb") as handle:
            result = subprocess.run(
                command, stdin=handle, capture_output=True, text=True, env=mysql_environment()
            )
    except FileNotFoundError as exc:
        raise RestoreError("找不到 mysql 客户端，无法导入 MySQL dump") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or f"退出码 {result.returncode}"
        raise RestoreError(f"导入 MySQL 失败：{detail}")


def stage_firmware(
    source_dir: Path, target: Path, entries: list[tuple[str, str]]
) -> Path:
    target.parent.mkdir(parents=True, exist_ok=True)
    staged = Path(tempfile.mkdtemp(prefix=f".{target.name}.restore-", dir=target.parent))
    try:
        for name, _ in entries:
            relative = PurePosixPath(name).relative_to("firmware")
            source = source_dir.joinpath(*relative.parts)
            destination = staged.joinpath(*relative.parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
    except OSError as exc:
        shutil.rmtree(staged, ignore_errors=True)
        raise RestoreError(f"暂存固件目录失败：{exc}") from exc
    return staged


def swap_firmware(staged: Path, target: Path) -> Path | None:
    backup = target.with_name(target.name + ".bak")
    had_target = target.exists()
    if had_target:
        try:
            os.replace(target, backup)
        except OSError as exc:
            raise RestoreError(f"无法把旧固件目录改名为 {backup}：{exc}") from exc
    try:
        os.replace(staged, target)
    except OSError as exc:
        rollback_failed = False
        if had_target and backup.exists() and not target.exists():
            try:
                os.replace(backup, target)
            except OSError:
                rollback_failed = True
        if rollback_failed:
            raise RestoreError(
                f"替换固件目录失败且回滚失败：{exc}；旧目录在 {backup}，新目录在 {staged}"
            ) from exc
        raise RestoreError(f"替换固件目录失败：{exc}；新固件暂存于 {staged}") from exc
    return backup if had_target else None


def restore_config(
    source_dir: Path,
    config_dir: Path,
    entries: list[tuple[str, str]],
    force: bool,
) -> int:
    staged: list[tuple[Path, Path]] = []
    replaced: list[Path] = []
    try:
        for name, _ in entries:
            relative = PurePosixPath(name).relative_to("config")
            source = source_dir.joinpath(*relative.parts)
            destination = config_dir.joinpath(*relative.parts)
            if destination.exists() and not destination.is_file():
                raise RestoreError(f"配置目标不是文件：{destination}")
            if destination.exists() and not force:
                raise RestoreError(f"配置文件已存在，需要 --force 才能覆盖：{destination}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{destination.name}.restore-", dir=destination.parent
            )
            temporary = Path(temporary_name)
            staged.append((temporary, destination))
            with os.fdopen(descriptor, "wb") as output, source.open("rb") as handle:
                shutil.copyfileobj(handle, output, CHUNK_SIZE)
                output.flush()
                os.fsync(output.fileno())
            os.chmod(temporary, clamp_mode(source.stat().st_mode))
        for temporary, destination in staged:
            try:
                os.replace(temporary, destination)
            except OSError as exc:
                raise RestoreError(
                    f"恢复配置失败：无法替换 {destination}：{exc}；"
                    f"已替换 {len(replaced)} 个文件，请核对后重跑恢复"
                ) from exc
            replaced.append(destination)
    except OSError as exc:
        raise RestoreError(f"恢复配置失败：{exc}") from exc
    finally:
        for temporary, _ in staged:
            temporary.unlink(missing_ok=True)
    return len(staged)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="校验并恢复 EdgePulse 备份归档。")
    parser.add_argument("--archive", required=True, help="备份 tar.gz 归档")
    parser.add_argument("--driver", choices=("sqlite", "mysql"), required=True, help="数据库驱动")
    parser.add_argument("--db-path", help="SQLite 数据库文件（sqlite 必填）")
    parser.add_argument("--db-host", help="MySQL 主机（mysql 必填）")
    parser.add_argument("--db-port", type=int, default=3306, help="MySQL 端口，默认 3306")
    parser.add_argument("--db-name", help="MySQL 数据库名（mysql 必填）")
    parser.add_argument("--db-user", help="MySQL 用户名（mysql 必填）")
    parser.add_argument("--firmware-dir", help="固件恢复目录")
    parser.add_argument("--config-dir", help="配置恢复目录")
    parser.add_argument("--force", action="store_true", help="允许覆盖非空目标")
    parser.add_argument("--dry-run", action="store_true", help="只校验，不写入任何文件")
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
    if args.firmware_dir:
        firmware_dir = Path(args.firmware_dir)
        if not firmware_dir.name or firmware_dir.name in (".", ".."):
            parser.error("--firmware-dir 不能是当前目录、上级目录或根目录，请指定专用目录")


def restore(args: argparse.Namespace) -> int:
    archive_path = Path(args.archive)
    if not archive_path.is_file():
        raise RestoreError(f"归档文件不存在：{archive_path}")
    try:
        tar = tarfile.open(archive_path, "r:gz")
    except (OSError, tarfile.TarError) as exc:
        raise RestoreError(f"无法打开归档 {archive_path}：{exc}") from exc
    with tar:
        manifest, regular, entries = load_manifest(tar, args.driver)
        verify_entries(tar, regular, entries)
        target_version, conflicts, firmware_entries, config_entries = inspect_targets(
            args, manifest, entries
        )

        version = int(manifest["migrationVersion"])
        print(
            f"归档校验通过：{archive_path}，驱动 {args.driver}，"
            f"迁移版本 {version}，文件 {len(entries)} 个"
        )
        print(f"目标数据库迁移版本：{target_version}")
        if conflicts:
            print("将覆盖非空目标：" + "、".join(conflicts))
        if args.dry_run:
            if conflicts and not args.force:
                print("目标非空且未给 --force，正式恢复会拒绝覆盖。")
            print("dry-run 仅校验，未修改任何文件。")
            return 0
        if conflicts and not args.force:
            raise RestoreError("目标非空，需要 --force 才能覆盖：" + "、".join(conflicts))

        with tempfile.TemporaryDirectory(prefix="edgepulse-restore-") as directory:
            payload = Path(directory) / "payload"
            payload.mkdir()
            extract_entries(tar, regular, entries, payload)
            database_source = payload.joinpath(*PurePosixPath(DATABASE_ARCHIVE[args.driver]).parts)
            if args.driver == "sqlite":
                replace_sqlite(database_source, Path(args.db_path), args.force)
                print(f"已恢复 SQLite 数据库：{args.db_path}")
            else:
                import_mysql(database_source, args)
                print(f"已导入 MySQL 数据库：{args.db_name}")

            firmware_backup: Path | None = None
            if firmware_entries:
                firmware_dir = Path(args.firmware_dir)
                staged = stage_firmware(payload / "firmware", firmware_dir, firmware_entries)
                firmware_backup = swap_firmware(staged, firmware_dir)
                print(f"已替换固件目录：{firmware_dir}")
            else:
                print("归档不含固件文件，已跳过固件恢复")

            if config_entries:
                restored = restore_config(
                    payload / "config", Path(args.config_dir), config_entries, args.force
                )
                print(f"已恢复配置文件：{restored} 个")

            if firmware_backup is not None:
                try:
                    shutil.rmtree(firmware_backup)
                except OSError as exc:
                    print(f"警告：旧固件目录未能删除，保留在 {firmware_backup}：{exc}", file=sys.stderr)
    print("恢复完成；启动服务后检查 /readyz。")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    validate_arguments(parser, args)
    if args.driver == "mysql":
        warn_if_mysql_password_missing()
    try:
        return restore(args)
    except RestoreError as exc:
        print(f"恢复失败：{exc}", file=sys.stderr)
        return 1
    except (OSError, tarfile.TarError, sqlite3.Error) as exc:
        print(f"恢复失败：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
