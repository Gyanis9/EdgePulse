#!/usr/bin/env python3
"""微基准：biz_telemetry 最新值查询在有无 (node_id,metric,id) 索引下的延迟。"""

import argparse
import sqlite3
import statistics
import tempfile
import time
from pathlib import Path

ROWS = 20000
METRICS = 20
ITERATIONS = 10


def build_database(path: Path, with_index: bool) -> None:
    connection = sqlite3.connect(path)
    try:
        connection.executescript(
            """
            CREATE TABLE biz_telemetry (
                id INTEGER PRIMARY KEY,
                tenant_id INTEGER NOT NULL,
                node_id INTEGER NOT NULL,
                metric VARCHAR(64) NOT NULL,
                value REAL NOT NULL,
                unit VARCHAR(32) NOT NULL DEFAULT '',
                collected_at TEXT NOT NULL,
                created_at TEXT NOT NULL
            );
            CREATE INDEX idx_biz_telemetry_node_metric ON biz_telemetry (node_id, metric, collected_at);
            """
        )
        rows = []
        for index in range(1, ROWS + 1):
            rows.append((index, 1, 1, f"metric.{index % METRICS:02d}", float(index % 100), "%", "2026-10-25 00:00:00", "2026-10-25 00:00:00"))
        connection.executemany("INSERT INTO biz_telemetry VALUES (?, ?, ?, ?, ?, ?, ?, ?)", rows)
        if with_index:
            connection.execute("CREATE INDEX idx_biz_telemetry_node_metric_latest ON biz_telemetry (node_id, metric, id)")
        connection.commit()
    finally:
        connection.close()


def measure(path: Path) -> list[float]:
    connection = sqlite3.connect(path)
    query = (
        "SELECT t.id, t.node_id, t.metric, t.value, t.unit, t.collected_at FROM biz_telemetry t "
        "JOIN (SELECT metric AS m, MAX(id) AS latest FROM biz_telemetry WHERE node_id = ? GROUP BY metric) x "
        "ON x.latest = t.id WHERE t.node_id = ? ORDER BY t.metric"
    )
    try:
        for _ in range(5):
            connection.execute(query, (1, 1)).fetchall()
        samples = []
        for _ in range(ITERATIONS):
            started = time.perf_counter()
            connection.execute(query, (1, 1)).fetchall()
            samples.append((time.perf_counter() - started) * 1000)
        return samples
    finally:
        connection.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--with-index", action="store_true", help="创建 (node_id,metric,id) 索引")
    parser.add_argument("--keep", type=Path, help="保留数据库到指定路径")
    args = parser.parse_args()

    path = args.keep or Path(tempfile.mkstemp(suffix=".db")[1])
    if args.keep and path.exists():
        path.unlink()
    build_database(path, args.with_index)
    samples = measure(path)
    p50 = statistics.median(samples)
    p95 = statistics.quantiles(samples, n=20)[18]
    print(f"index={'yes' if args.with_index else 'no'} rows={ROWS} iterations={ITERATIONS} p50={p50:.3f}ms p95={p95:.3f}ms")
    if not args.keep:
        path.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
