#!/usr/bin/env python3
"""
Concurrent-write benchmark harness for ladybug.

Strategy
--------
* single_writer_txn_per_sec, checkpoint_latency_ms
    → stdin-piped lbug shell session (one process, no lock contention)
* concurrent_write_txns_per_sec, per-test gtest timings
    → gtest XML timing from transaction_test binary
    (concurrent connections are in-process threads; separate CLI processes
    each hold an exclusive DB lock and cannot share a database via MVCC)

Usage
-----
  python3 benchmarks/run_benchmarks.py \
      --build-dir build/relwithdebinfo \
      --output benchmarks/results.json \
      [--label baseline]

The output JSON matches the schema expected by compare_benchmarks.py.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
from pathlib import Path


# ── CLI / shell-session helpers ──────────────────────────────────────────────


def run_shell_session(
    cli_bin: str, db_path: str, queries: list, timeout: int = 180
) -> float:
    """
    Open one lbug shell session, pipe all queries via stdin, return wall time.
    lbug reads one statement per line until EOF.
    """
    script = "\n".join(queries) + "\n"
    t0 = time.monotonic()
    result = subprocess.run(
        [cli_bin, db_path],
        input=script,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    elapsed = time.monotonic() - t0
    if result.returncode != 0:
        raise RuntimeError(f"Shell session failed:\n{result.stderr[:500]}")
    return elapsed


def measure_single_writer(cli_bin: str, db_path: str, num_txns: int = 500) -> float:
    """Single-writer transaction throughput (txn/s)."""
    run_shell_session(
        cli_bin,
        db_path,
        ["CREATE NODE TABLE IF NOT EXISTS bench_sw(id INT64 PRIMARY KEY);"],
    )
    # Warm-up
    run_shell_session(
        cli_bin, db_path, [f"CREATE (:bench_sw {{id: -{i}}});" for i in range(50)]
    )
    # Timed batch — all in one session to amortise startup cost
    queries = [f"CREATE (:bench_sw {{id: {i}}});" for i in range(num_txns)]
    elapsed = run_shell_session(cli_bin, db_path, queries)
    return num_txns / elapsed


def measure_checkpoint_latency(cli_bin: str, db_path: str) -> float:
    """Wall-clock time (ms) for one forced CHECKPOINT call."""
    setup = ["CREATE NODE TABLE IF NOT EXISTS bench_cp(id INT64 PRIMARY KEY);"] + [
        f"CREATE (:bench_cp {{id: {i}}});" for i in range(300)
    ]
    run_shell_session(cli_bin, db_path, setup)
    elapsed = run_shell_session(cli_bin, db_path, ["CHECKPOINT;"])
    return elapsed * 1000  # → ms


def measure_reads_during_checkpoint(
    cli_bin: str, db_path: str, duration_s: float = 5.0
) -> float:
    """
    Measure read ops/sec from a --readonly process while an active writer
    runs CHECKPOINT.

    Main (pre-port)  → 0.0
      --readonly tries to acquire LOCK_SH; the writer holds LOCK_EX.
      Every read attempt fails or blocks → zero ops complete.

    Feat (post-port) → >0
      --readonly skips the file lock entirely (readOnly=true path added by
      the Vela port). Read transactions use MVCC snapshot isolation and
      proceed concurrently with checkpoint → non-zero ops/sec.
    """
    run_shell_session(
        cli_bin,
        db_path,
        ["CREATE NODE TABLE IF NOT EXISTS bench_rdcp(id INT64 PRIMARY KEY);"]
        + [f"CREATE (:bench_rdcp {{id: {i}}});" for i in range(500)],
    )

    read_count = [0]
    done = [False]

    def reader_loop() -> None:
        while not done[0]:
            try:
                result = subprocess.run(
                    [cli_bin, db_path, "--readonly", "--nostats"],
                    input="MATCH (n:bench_rdcp) RETURN COUNT(n);\n",
                    capture_output=True,
                    text=True,
                    timeout=3.0,
                )
                if result.returncode == 0:
                    read_count[0] += 1
            except Exception:
                pass

    # Keep a writer session alive (holds LOCK_EX) and trigger CHECKPOINT
    writer = subprocess.Popen(
        [cli_bin, db_path, "--nostats"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    time.sleep(0.15)  # let writer open the DB
    writer.stdin.write("CHECKPOINT;\n")
    writer.stdin.flush()

    reader_thread = threading.Thread(target=reader_loop, daemon=True)
    reader_thread.start()
    time.sleep(duration_s)
    done[0] = True

    writer.stdin.close()
    writer.wait(timeout=10)
    return read_count[0] / duration_s


# ── gtest timing helpers ─────────────────────────────────────────────────────

CONCURRENT_FILTER = ":".join(
    [
        "EmptyDBTransactionTest.ConcurrentNodeInsertions",
        "EmptyDBTransactionTest.ConcurrentRelationshipInsertions",
        "EmptyDBTransactionTest.ConcurrentNodeUpdates",
        "EmptyDBTransactionTest.ConcurrentRelationshipUpdates",
    ]
)

# Each concurrent gtest spawns 4 writer threads × 1000 inserts
WRITES_PER_CONCURRENT_TEST = 4000


def run_gtest_timed(
    exe: Path, xml_out: Path, gtest_filter: str, env: dict, timeout: int = 600
) -> dict:
    """Run a gtest binary with a filter; return {SuiteName.TestName: duration_s}."""
    cmd = [str(exe), f"--gtest_output=xml:{xml_out}", f"--gtest_filter={gtest_filter}"]
    subprocess.run(
        cmd, env={**os.environ, **env}, capture_output=True, text=True, timeout=timeout
    )
    timings = {}
    if not xml_out.exists():
        return timings
    tree = ET.parse(xml_out)
    for suite in tree.getroot().iter("testsuite"):
        suite_name = suite.get("name", "")
        for tc in suite.iter("testcase"):
            duration = float(tc.get("time", 0))
            timings[f"{suite_name}.{tc.get('name', '')}"] = duration
    return timings


# ── Main ─────────────────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run ladybug concurrent-write benchmarks"
    )
    parser.add_argument("--build-dir", default="build/relwithdebinfo")
    parser.add_argument("--dataset-dir", default="dataset")
    parser.add_argument("--output", default="benchmarks/results.json")
    parser.add_argument("--label", default="branch")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    dataset_dir = Path(args.dataset_dir).resolve()

    # Locate lbug shell binary
    cli_candidates = [
        build_dir / "tools" / "shell" / "lbug",
        build_dir / "lbug",
    ]
    cli_bin = next((str(p) for p in cli_candidates if p.is_file()), None)
    if not cli_bin:
        print(f"ERROR: could not find lbug in {build_dir}", file=sys.stderr)
        sys.exit(1)

    txn_test_bin = build_dir / "test" / "transaction" / "transaction_test"
    if not txn_test_bin.is_file():
        print(f"ERROR: transaction_test not found at {txn_test_bin}", file=sys.stderr)
        sys.exit(1)

    results: dict = {
        "label": args.label,
        "timestamp": int(time.time()),
        "metrics": {},
        "gtest_timings": {},
    }

    gtest_env = {
        "E2E_TEST_FILES_DIRECTORY": str(Path("test/test_files").resolve()),
        "KUZU_DATASET_PATH": str(dataset_dir),
        "LBUG_DATASET_PATH": str(dataset_dir),
    }

    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = os.path.join(tmpdir, "bench.lbdb")

        print(f"Benchmarks — label={args.label!r}", flush=True)

        print("  1/3 single_writer_txn_per_sec ...", flush=True)
        results["metrics"]["single_writer_txn_per_sec"] = measure_single_writer(
            cli_bin, db_path
        )
        print(
            f"       → {results['metrics']['single_writer_txn_per_sec']:.1f} txn/s",
            flush=True,
        )

        print("  2/3 checkpoint_latency_ms ...", flush=True)
        results["metrics"]["checkpoint_latency_ms"] = measure_checkpoint_latency(
            cli_bin, db_path
        )
        print(
            f"       → {results['metrics']['checkpoint_latency_ms']:.0f} ms", flush=True
        )

        print(
            "  3/3 concurrent write gtests (4 tests × 4 threads × 1000 inserts) ...",
            flush=True,
        )
        xml_out = Path(tmpdir) / "concurrent.xml"
        timings = run_gtest_timed(txn_test_bin, xml_out, CONCURRENT_FILTER, gtest_env)
        results["gtest_timings"] = timings
        total_s = sum(timings.values())
        total_writes = WRITES_PER_CONCURRENT_TEST * len(timings)
        results["metrics"]["concurrent_write_txns_per_sec"] = (
            total_writes / total_s if total_s > 0 else 0.0
        )
        for k, v in sorted(timings.items()):
            print(f"       {k}: {v:.1f}s", flush=True)
        print(
            f"       → {results['metrics']['concurrent_write_txns_per_sec']:.1f} txn/s aggregate",
            flush=True,
        )

        print("  4/4 read_during_checkpoint_ops_per_sec ...", flush=True)
        rdcp = measure_reads_during_checkpoint(cli_bin, db_path)
        results["metrics"]["read_during_checkpoint_ops_per_sec"] = rdcp
        print(
            f"       → {rdcp:.2f} ops/s  (0 = blocked/locked, >0 = non-blocking)",
            flush=True,
        )

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w") as f:
        json.dump(results, f, indent=2)

    print(f"\nResults → {args.output}")
    print(json.dumps(results["metrics"], indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
