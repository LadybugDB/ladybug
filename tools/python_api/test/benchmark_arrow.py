from __future__ import annotations

import argparse
import contextlib
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import numpy as np
import pyarrow as pa

python_build_dir = Path(__file__).parent.parent / "build"
try:
    import real_ladybug as lb
except ModuleNotFoundError:
    sys.path.append(str(python_build_dir))
    import real_ladybug as lb


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Create a large in-memory Arrow-backed table, run a CPU-intensive Cypher filter query, "
            "and validate deterministic results."
        )
    )
    parser.add_argument(
        "--target-gb", type=float, default=8.0, help="Target Arrow table size in GiB."
    )
    parser.add_argument(
        "--chunk-rows",
        type=int,
        default=1_000_000,
        help="Rows per generated Arrow record batch.",
    )
    parser.add_argument(
        "--filter-cutoff",
        type=int,
        default=25,
        help="Filter predicate uses n.filter_key < cutoff where filter_key is in [0, 999].",
    )
    parser.add_argument(
        "--threads",
        type=int,
        default=max(2, os.cpu_count() or 2),
        help="Ladybug query worker threads.",
    )
    parser.add_argument(
        "--db-path", type=str, default="", help="Optional database path."
    )
    parser.add_argument(
        "--query-runs",
        type=int,
        default=3,
        help="How many times to execute the Cypher query for timing measurement.",
    )
    return parser.parse_args()


def read_process_cpu_percent(pid: int) -> float:
    output = subprocess.check_output(
        ["ps", "-o", "%cpu=", "-p", str(pid)], text=True
    ).strip()
    if not output:
        return 0.0
    return float(output)


def build_large_arrow_table(
    target_bytes: int, chunk_rows: int, filter_cutoff: int
) -> tuple[pa.Table, int, int]:
    batches: list[pa.RecordBatch] = []
    total_bytes = 0
    row_start = 0
    expected_count = 0
    expected_checksum = 0

    while total_bytes < target_bytes:
        row_end = row_start + chunk_rows
        ids = np.arange(row_start, row_end, dtype=np.int64)
        filter_key = ((ids * 37 + 17) % 1000).astype(np.int32)

        x0 = ids % 997
        x1 = (ids * 7 + 3) % 991
        x2 = (ids * 11 + 5) % 983
        x3 = (ids * 13 + 7) % 977
        x4 = (ids * 17 + 11) % 971
        x5 = (ids * 19 + 13) % 967
        x6 = (ids * 23 + 17) % 953
        x7 = (ids * 29 + 19) % 947
        x8 = (ids * 31 + 23) % 941
        x9 = (ids * 37 + 29) % 937
        x10 = (ids * 41 + 31) % 929
        x11 = (ids * 43 + 37) % 919

        mask = filter_key < filter_cutoff
        expected_count += int(mask.sum())
        expected_checksum += int(
            (
                (x0[mask] * x1[mask])
                + (x2[mask] * x3[mask])
                - (x4[mask] * x5[mask])
                + (x6[mask] % 97)
                + (x7[mask] % 89)
                + (x8[mask] * 3)
                - (x9[mask] * 5)
                + (x10[mask] % 71)
                - (x11[mask] % 67)
            ).sum()
        )

        batch = pa.record_batch(
            {
                "id": pa.array(ids),
                "filter_key": pa.array(filter_key),
                "x0": pa.array(x0),
                "x1": pa.array(x1),
                "x2": pa.array(x2),
                "x3": pa.array(x3),
                "x4": pa.array(x4),
                "x5": pa.array(x5),
                "x6": pa.array(x6),
                "x7": pa.array(x7),
                "x8": pa.array(x8),
                "x9": pa.array(x9),
                "x10": pa.array(x10),
                "x11": pa.array(x11),
            }
        )
        batches.append(batch)
        total_bytes += batch.nbytes
        row_start = row_end

    return pa.Table.from_batches(batches), expected_count, expected_checksum


def measure_query_once(
    conn: lb.Connection, query: str
) -> tuple[float, int, int, float, float]:
    samples: list[float] = []
    stop_event = threading.Event()

    def cpu_sampler() -> None:
        pid = os.getpid()
        while not stop_event.is_set():
            with contextlib.suppress(Exception):
                samples.append(read_process_cpu_percent(pid))
            time.sleep(0.2)

    sampler_thread = threading.Thread(target=cpu_sampler, daemon=True)
    sampler_thread.start()

    query_start = time.perf_counter()
    result = conn.execute(query)
    row = result.get_next()
    elapsed = time.perf_counter() - query_start

    stop_event.set()
    sampler_thread.join(timeout=1.0)

    actual_count = int(row[0])
    actual_checksum = int(row[1])
    max_cpu = max(samples) if samples else 0.0
    avg_cpu = (sum(samples) / len(samples)) if samples else 0.0

    return elapsed, actual_count, actual_checksum, avg_cpu, max_cpu


def main() -> int:
    args = parse_args()
    if not (0 < args.filter_cutoff <= 1000):
        msg = "--filter-cutoff must be in [1, 1000]."
        raise ValueError(msg)
    if args.chunk_rows <= 0:
        msg = "--chunk-rows must be positive."
        raise ValueError(msg)
    if args.query_runs <= 0:
        msg = "--query-runs must be positive."
        raise ValueError(msg)

    target_bytes = int(args.target_gb * (1024**3))
    db_path_value = args.db_path

    temp_dir: tempfile.TemporaryDirectory[str] | None = None
    if not db_path_value:
        temp_dir = tempfile.TemporaryDirectory(prefix="ladybug_arrow_bench_")
        db_path_value = str(Path(temp_dir.name) / "bench.lbdb")

    print(
        f"Building Arrow table (target ~{args.target_gb:.2f} GiB)... and {args.threads} query threads"
    )
    build_start = time.perf_counter()
    table, expected_count, expected_checksum = build_large_arrow_table(
        target_bytes=target_bytes,
        chunk_rows=args.chunk_rows,
        filter_cutoff=args.filter_cutoff,
    )
    build_secs = time.perf_counter() - build_start
    print(
        f"Built table with {table.num_rows:,} rows, {table.nbytes / (1024**3):.2f} GiB in {build_secs:.2f}s"
    )

    db = lb.Database(
        database_path=db_path_value, buffer_pool_size=256 * 1024 * 1024, read_only=False
    )
    conn = lb.Connection(db, num_threads=args.threads)

    table_name = "arrow_cpu_bench"
    using_arrow_memory_table = hasattr(conn._connection, "create_arrow_table")
    if using_arrow_memory_table:
        print(f"Registering Arrow memory-backed table '{table_name}'...")
        conn.create_arrow_table(table_name, table)
    else:
        print(f"Creating node table '{table_name}' and loading from Arrow...")
        conn.execute(f"""
            CREATE NODE TABLE {table_name}(
                id INT64,
                filter_key INT32,
                x0 INT64,
                x1 INT64,
                x2 INT64,
                x3 INT64,
                x4 INT64,
                x5 INT64,
                x6 INT64,
                x7 INT64,
                x8 INT64,
                x9 INT64,
                x10 INT64,
                x11 INT64,
                PRIMARY KEY(id)
            )
            """)
        conn.execute(f"COPY {table_name} FROM $df", {"df": table})

    query = f"""
        MATCH (n:{table_name})
        WHERE n.filter_key < {args.filter_cutoff}
        RETURN
            COUNT(*) AS cnt,
            SUM(
                (n.x0 * n.x1) +
                (n.x2 * n.x3) -
                (n.x4 * n.x5) +
                (n.x6 % 97) +
                (n.x7 % 89) +
                (n.x8 * 3) -
                (n.x9 * 5) +
                (n.x10 % 71) -
                (n.x11 % 67)
            ) AS checksum
    """

    run_stats: list[tuple[float, float, float]] = []
    for run_idx in range(1, args.query_runs + 1):
        print(f"Running CPU-intensive Cypher query (run {run_idx})...")
        elapsed, actual_count, actual_checksum, avg_cpu, max_cpu = measure_query_once(
            conn, query
        )
        print(f"Query time: {elapsed:.2f}s")
        print(f"CPU usage during query: avg={avg_cpu:.1f}% max={max_cpu:.1f}%")
        print(f"Expected cnt={expected_count:,}, actual cnt={actual_count:,}")
        print(
            f"Expected checksum={expected_checksum:,}, actual checksum={actual_checksum:,}"
        )

        if actual_count != expected_count or actual_checksum != expected_checksum:
            if using_arrow_memory_table:
                conn.drop_arrow_table(table_name)
            else:
                conn.execute(f"DROP TABLE {table_name}")
            conn.close()
            if temp_dir:
                temp_dir.cleanup()
            msg = "Query result validation failed."
            raise AssertionError(msg)

        run_stats.append((elapsed, avg_cpu, max_cpu))

    avg_elapsed = sum(stat[0] for stat in run_stats) / len(run_stats)
    max_cpu_overall = max(stat[2] for stat in run_stats)
    print(f"Average query time over {len(run_stats)} runs: {avg_elapsed:.2f}s")
    print(f"Maximum observed CPU across runs: {max_cpu_overall:.1f}%")

    # >100% indicates more than one core on ps-based accounting.
    if max_cpu_overall <= 100.0:
        print(
            "Warning: max CPU did not exceed 100%; try larger target-gb/chunk-rows or more threads."
        )
    else:
        print("Observed CPU > 100%, indicating multi-core usage.")

    if using_arrow_memory_table:
        conn.drop_arrow_table(table_name)
    else:
        conn.execute(f"DROP TABLE {table_name}")
    conn.close()
    if temp_dir:
        temp_dir.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
