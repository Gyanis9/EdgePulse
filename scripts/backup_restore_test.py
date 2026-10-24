"""备份与恢复的 SQLite 往返、WAL 边车、路径与私钥过滤测试；MySQL 由部署环境单独验证。"""

from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import sqlite3
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
from contextlib import closing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run_script(
    script: str, *arguments: str, check: bool = True
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(ROOT / "scripts" / script), *arguments],
        check=check,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )


def create_probe_database(path: Path, value: str, *, wal: bool = False) -> None:
    # sqlite3 的上下文管理器只提交事务，不关闭连接；不显式关闭会锁住文件
    with closing(sqlite3.connect(path)) as connection, connection:
        if wal:
            connection.execute("PRAGMA journal_mode=WAL")
            connection.execute("PRAGMA wal_autocheckpoint=0")
        connection.execute(
            "CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, checksum TEXT)"
        )
        connection.execute("INSERT INTO schema_migrations VALUES (18, 'x')")
        connection.execute("CREATE TABLE probe (id INTEGER PRIMARY KEY, value TEXT)")
        connection.execute("INSERT INTO probe VALUES (1, ?)", (value,))


def probe_value(path: Path) -> str:
    with closing(sqlite3.connect(path)) as connection:
        return connection.execute("SELECT value FROM probe WHERE id = 1").fetchone()[0]


def add_manifest_entries(archive: Path, extras: dict[str, bytes]) -> None:
    """向归档追加成员并同步清单哈希，用于构造恶意或特殊路径。"""
    with tarfile.open(archive, "r:gz") as source:
        members = [
            (member, source.extractfile(member).read() if member.isfile() else None)
            for member in source.getmembers()
        ]
    with tarfile.open(archive, "w:gz") as target:
        for member, data in members:
            if data is None:
                target.addfile(member)
                continue
            if member.name == "manifest.json":
                manifest = json.loads(data)
                manifest["files"].extend(
                    {"path": name, "sha256": hashlib.sha256(content).hexdigest()}
                    for name, content in extras.items()
                )
                data = json.dumps(manifest).encode("utf-8")
                member.size = len(data)
            target.addfile(member, io.BytesIO(data))
        for name, content in extras.items():
            info = tarfile.TarInfo(name)
            info.size = len(content)
            info.mode = 0o600
            target.addfile(info, io.BytesIO(content))


class BackupRestoreTest(unittest.TestCase):
    def test_sqlite_round_trip_and_tamper_rejection(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            db = base / "edgepulse.db"
            firmware = base / "firmware"
            firmware.mkdir()
            (firmware / "fw.bin").write_bytes(b"firmware")
            create_probe_database(db, "before")
            archive = base / "backup.tar.gz"
            run_script(
                "backup.py",
                "--driver",
                "sqlite",
                "--db-path",
                str(db),
                "--firmware-dir",
                str(firmware),
                "--output",
                str(archive),
            )
            with closing(sqlite3.connect(db)) as connection, connection:
                connection.execute("UPDATE probe SET value = 'after'")

            dry_run = run_script(
                "restore.py",
                "--archive",
                str(archive),
                "--driver",
                "sqlite",
                "--db-path",
                str(db),
                "--firmware-dir",
                str(firmware),
                "--dry-run",
                check=False,
            )
            self.assertEqual(0, dry_run.returncode)
            self.assertIn("正式恢复会拒绝覆盖", dry_run.stdout)
            self.assertEqual("after", probe_value(db))

            refused = run_script(
                "restore.py",
                "--archive",
                str(archive),
                "--driver",
                "sqlite",
                "--db-path",
                str(db),
                "--firmware-dir",
                str(firmware),
                check=False,
            )
            self.assertNotEqual(0, refused.returncode)
            self.assertIn("--force", refused.stderr)
            self.assertEqual("after", probe_value(db))

            run_script(
                "restore.py",
                "--archive",
                str(archive),
                "--driver",
                "sqlite",
                "--db-path",
                str(db),
                "--firmware-dir",
                str(firmware),
                "--force",
            )
            self.assertEqual("before", probe_value(db))
            self.assertEqual(b"firmware", (firmware / "fw.bin").read_bytes())

            # 篡改归档：改写 manifest 里的哈希，restore 必须拒绝
            with tarfile.open(archive, "r:gz") as source:
                members = [
                    (member, source.extractfile(member).read() if member.isfile() else None)
                    for member in source.getmembers()
                ]
            with tarfile.open(archive, "w:gz") as target:
                for member, data in members:
                    if data is None:
                        target.addfile(member)
                        continue
                    if member.name.endswith("manifest.json"):
                        manifest = json.loads(data)
                        manifest["files"][0]["sha256"] = "0" * 64
                        data = json.dumps(manifest).encode("utf-8")
                        member.size = len(data)
                    target.addfile(member, io.BytesIO(data))
            result = run_script(
                "restore.py",
                "--archive",
                str(archive),
                "--driver",
                "sqlite",
                "--db-path",
                str(db),
                "--firmware-dir",
                str(firmware),
                "--force",
                check=False,
            )
            self.assertNotEqual(0, result.returncode)

    def test_sqlite_restore_removes_stale_wal_sidecars(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            live_dir = base / "live"
            live_dir.mkdir()
            live_db = live_dir / "edgepulse.db"
            create_probe_database(live_db, "before", wal=True)
            firmware = base / "firmware"
            firmware.mkdir()
            (firmware / "fw.bin").write_bytes(b"firmware")
            archive = base / "backup.tar.gz"
            run_script(
                "backup.py",
                "--driver",
                "sqlite",
                "--db-path",
                str(live_db),
                "--firmware-dir",
                str(firmware),
                "--output",
                str(archive),
            )

            target_dir = base / "target"
            target_dir.mkdir()
            target_db = target_dir / "edgepulse.db"
            writer = sqlite3.connect(live_db)
            try:
                writer.execute("PRAGMA wal_autocheckpoint=0")
                writer.execute("UPDATE probe SET value = 'after'")
                writer.commit()
                for suffix in ("", "-wal", "-shm"):
                    source = live_dir / ("edgepulse.db" + suffix)
                    self.assertTrue(source.is_file(), f"缺少源文件：{source}")
                    shutil.copyfile(source, target_dir / ("edgepulse.db" + suffix))
                self.assertGreater((target_dir / "edgepulse.db-wal").stat().st_size, 0)
            finally:
                writer.close()

            # 只读打开只回放 WAL，不 checkpoint，崩溃镜像保留 after 与边车
            with closing(
                sqlite3.connect(target_db.resolve().as_uri() + "?mode=ro", uri=True)
            ) as connection:
                value = connection.execute("SELECT value FROM probe WHERE id = 1").fetchone()[0]
            self.assertEqual("after", value)
            self.assertTrue((target_dir / "edgepulse.db-wal").exists())

            run_script(
                "restore.py",
                "--archive",
                str(archive),
                "--driver",
                "sqlite",
                "--db-path",
                str(target_db),
                "--firmware-dir",
                str(firmware),
                "--force",
            )
            for suffix in ("-wal", "-shm", "-journal"):
                self.assertFalse(
                    (target_dir / ("edgepulse.db" + suffix)).exists(),
                    f"恢复后残留数据库边车文件：{suffix}",
                )
            self.assertEqual("before", probe_value(target_db))

    def test_restore_rejects_windows_drive_relative_member(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            db = base / "edgepulse.db"
            create_probe_database(db, "before")
            firmware = base / "firmware"
            firmware.mkdir()
            (firmware / "fw.bin").write_bytes(b"firmware")
            archive = base / "backup.tar.gz"
            run_script(
                "backup.py",
                "--driver",
                "sqlite",
                "--db-path",
                str(db),
                "--firmware-dir",
                str(firmware),
                "--output",
                str(archive),
            )
            add_manifest_entries(archive, {"firmware/C:/evil": b"evil"})

            target_db = base / "target" / "edgepulse.db"
            target_firmware = base / "restored-firmware"
            result = run_script(
                "restore.py",
                "--archive",
                str(archive),
                "--driver",
                "sqlite",
                "--db-path",
                str(target_db),
                "--firmware-dir",
                str(target_firmware),
                "--force",
                check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("Windows 非法字符", result.stderr)
            self.assertFalse(target_db.exists())
            self.assertFalse(target_firmware.exists())
            self.assertEqual([], [path for path in base.rglob("evil") if path.is_file()])

    @unittest.skipIf(
        os.name == "nt", "Windows 不保留 POSIX 权限位；数据库权限由宿主 ACL 负责"
    )
    def test_sqlite_restore_forces_private_database_mode(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            db = base / "edgepulse.db"
            firmware = base / "firmware"
            firmware.mkdir()
            create_probe_database(db, "before")
            archive = base / "backup.tar.gz"
            run_script(
                "backup.py",
                "--driver",
                "sqlite",
                "--db-path",
                str(db),
                "--firmware-dir",
                str(firmware),
                "--output",
                str(archive),
            )
            # 目标带着 0640 权限：旧实现会把它收敛成 0644，等于放宽到组与其他用户可读
            os.chmod(db, 0o640)
            run_script(
                "restore.py",
                "--archive",
                str(archive),
                "--driver",
                "sqlite",
                "--db-path",
                str(db),
                "--force",
            )
            self.assertEqual("before", probe_value(db))
            self.assertEqual(0o600, stat.S_IMODE(db.stat().st_mode))

    def test_restore_rejects_current_directory_as_firmware_target(self) -> None:
        result = run_script(
            "restore.py",
            "--archive",
            "unused.tar.gz",
            "--driver",
            "sqlite",
            "--db-path",
            "edgepulse.db",
            "--firmware-dir",
            ".",
            check=False,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("--firmware-dir", result.stderr)

    def test_backup_excludes_private_key_after_certificate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            db = base / "edgepulse.db"
            create_probe_database(db, "before")
            firmware = base / "firmware"
            firmware.mkdir()
            config = base / "config"
            config.mkdir()
            certificate = (
                "-----BEGIN CERTIFICATE-----\n"
                + "A" * 400
                + "\n-----END CERTIFICATE-----\n"
            )
            private_key = (
                "-----BEGIN PRIVATE KEY-----\n"
                + "B" * 120
                + "\n-----END PRIVATE KEY-----\n"
            )
            (config / "bundle.pem").write_text(
                certificate + private_key, encoding="utf-8"
            )
            (config / "public.pem").write_text(certificate, encoding="utf-8")
            archive = base / "backup.tar.gz"
            run_script(
                "backup.py",
                "--driver",
                "sqlite",
                "--db-path",
                str(db),
                "--firmware-dir",
                str(firmware),
                "--config-dir",
                str(config),
                "--output",
                str(archive),
            )
            with tarfile.open(archive, "r:gz") as tar:
                names = tar.getnames()
            self.assertNotIn("config/bundle.pem", names)
            self.assertIn("config/public.pem", names)


if __name__ == "__main__":
    unittest.main()
