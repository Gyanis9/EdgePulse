"""备份与恢复的 SQLite 往返测试；MySQL 由部署环境单独验证。"""

from __future__ import annotations

import io
import json
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import unittest
from contextlib import closing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class BackupRestoreTest(unittest.TestCase):
    def test_sqlite_round_trip_and_tamper_rejection(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            db = base / "edgepulse.db"
            firmware = base / "firmware"
            firmware.mkdir()
            (firmware / "fw.bin").write_bytes(b"firmware")
            # sqlite3 的上下文管理器只提交事务，不关闭连接；不显式关闭会锁住文件
            with closing(sqlite3.connect(db)) as connection, connection:
                connection.execute(
                    "CREATE TABLE schema_migrations (version INTEGER PRIMARY KEY, checksum TEXT)"
                )
                connection.execute("INSERT INTO schema_migrations VALUES (18, 'x')")
                connection.execute("CREATE TABLE probe (id INTEGER PRIMARY KEY, value TEXT)")
                connection.execute("INSERT INTO probe VALUES (1, 'before')")
            archive = base / "backup.tar.gz"
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "backup.py"),
                    "--driver",
                    "sqlite",
                    "--db-path",
                    str(db),
                    "--firmware-dir",
                    str(firmware),
                    "--output",
                    str(archive),
                ],
                check=True,
            )
            with closing(sqlite3.connect(db)) as connection, connection:
                connection.execute("UPDATE probe SET value = 'after'")
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "restore.py"),
                    "--archive",
                    str(archive),
                    "--driver",
                    "sqlite",
                    "--db-path",
                    str(db),
                    "--firmware-dir",
                    str(firmware),
                    "--force",
                ],
                check=True,
            )
            with closing(sqlite3.connect(db)) as connection:
                value = connection.execute("SELECT value FROM probe WHERE id = 1").fetchone()[0]
            self.assertEqual("before", value)
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
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "restore.py"),
                    "--archive",
                    str(archive),
                    "--driver",
                    "sqlite",
                    "--db-path",
                    str(db),
                    "--firmware-dir",
                    str(firmware),
                    "--force",
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
            )
            self.assertNotEqual(0, result.returncode)


if __name__ == "__main__":
    unittest.main()
