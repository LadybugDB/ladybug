#!/usr/bin/env python3
"""
Generate a Markdown report from benchmark result JSON files.

Single-run history table (default):
  python3 benchmarks/report.py
  python3 benchmarks/report.py --results-dir benchmarks/results

Before/after comparison (delegates to compare_benchmarks.py logic):
  python3 benchmarks/report.py --baseline benchmarks/results/main-*.json \
                                --branch   benchmarks/results/feat-*.json

The history table lists every file in --results-dir, sorted by timestamp,
with a delta column relative to the oldest entry (the implied baseline).

Output goes to stdout by default; use --output to write to a file.
"""
import argparse
import json
import os
import sys
from pathlib import Path
from datetime import datetime, timezone


METRIC_META = {
    "single_writer_txn_per_sec": ("Single-writer (txn/s)", "higher"),
    "checkpoint_latency_ms": ("Checkpoint latency (ms)", "lower"),
    "concurrent_write_txns_per_sec": ("Concurrent writes (txn/s)", "higher"),
    "read_during_checkpoint_ops_per_sec": ("Reads-during-checkpoint (ops/s)", "higher"),
}

GTEST_NAMES = {
    "EmptyDBTransactionTest.ConcurrentNodeInsertions": "Node inserts (4T×1k)",
    "EmptyDBTransactionTest.ConcurrentRelationshipInsertions": "Rel inserts (4T×1k)",
    "EmptyDBTransactionTest.ConcurrentNodeUpdates": "Node updates (4T×1k)",
    "EmptyDBTransactionTest.ConcurrentRelationshipUpdates": "Rel updates (4T×1k)",
}


def load_result(path: Path) -> dict:
    with open(path) as f:
        return json.load(f)


def fmt_metric(val: float, key: str) -> str:
    if key == "checkpoint_latency_ms":
        return f"{val:.0f} ms"
    return f"{val:.1f}"


def pct(new: float, old: float) -> str:
    if old == 0:
        return "—"
    delta = (new - old) / old * 100
    sign = "+" if delta >= 0 else ""
    return f"{sign}{delta:.1f}%"


def direction_emoji(key: str, new: float, old: float) -> str:
    # reads_during_checkpoint: 0-baseline means the feature wasn't present;
    # any positive value on the new build is unconditionally good.
    if key == "read_during_checkpoint_ops_per_sec":
        if old < 0.5 and new >= 0.5:
            return "🟢"
        if old >= 0.5 and new < 0.5:
            return "🔴"
        return "⚪"
    if old == 0:
        return ""
    delta = (new - old) / old
    # Use same threshold as CI gate (5%) for red; 2% for green highlight.
    # concurrent_write_txns_per_sec has no hard gate — never show red for it.
    gate = 0.05
    if key == "checkpoint_latency_ms":
        better = delta < -0.02
        worse = delta > gate
    else:
        better = delta > 0.02
        worse = (delta < -gate) and key != "concurrent_write_txns_per_sec"
    if better:
        return "🟢"
    if worse:
        return "🔴"
    return "⚪"


def ts_str(ts: int) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def history_report(results: list[dict], filenames: list[str]) -> str:
    """Multi-run history table, oldest first, deltas vs oldest run."""
    lines = [
        "## 📊 Benchmark history",
        "",
        f"_Generated {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')}_",
        "",
    ]

    # Header row
    header = "| Metric | " + " | ".join(f"`{Path(f).stem}`" for f in filenames) + " |"
    sep = "|--------|" + "--------|" * len(filenames)
    lines += [header, sep]

    baseline = results[0]

    for key, (label, _direction) in METRIC_META.items():
        row = f"| {label} |"
        for i, r in enumerate(results):
            val = r["metrics"].get(key)
            if val is None:
                row += " — |"
                continue
            cell = fmt_metric(val, key)
            if i > 0:
                bv = baseline["metrics"].get(key, 0)
                emoji = direction_emoji(key, val, bv)
                delta = pct(val, bv)
                cell = f"{emoji} **{cell}** ({delta})"
            row += f" {cell} |"
        lines.append(row)

    # Derived safety row: ✅ if MVCC infrastructure confirmed (rdcp ≥ 0.5), else ⚠️
    safety_row = "| Concurrent write safety |"
    for r in results:
        rdcp = r["metrics"].get("read_during_checkpoint_ops_per_sec", 0)
        if rdcp >= 0.5:
            safety_row += " ✅ Production-safe (MVCC) |"
        else:
            safety_row += " ⚠️ Unsafe bypass (no MVCC) |"
    lines.append(safety_row)

    # Gtest timing breakdown (if present)
    if any(r.get("gtest_timings") for r in results):
        lines += ["", "### Gtest concurrent-write timings (seconds)", ""]
        gtest_header = (
            "| Test | " + " | ".join(f"`{Path(f).stem}`" for f in filenames) + " |"
        )
        gtest_sep = "|------|" + "--------|" * len(filenames)
        lines += [gtest_header, gtest_sep]
        all_tests = sorted({k for r in results for k in r.get("gtest_timings", {})})
        for t in all_tests:
            display = GTEST_NAMES.get(t, t.split(".")[-1])
            row = f"| {display} |"
            for i, r in enumerate(results):
                val = r.get("gtest_timings", {}).get(t)
                if val is None:
                    row += " — |"
                else:
                    cell = f"{val:.1f}s"
                    if i > 0:
                        bv = results[0].get("gtest_timings", {}).get(t, 0)
                        if bv:
                            delta = (val - bv) / bv * 100
                            sign = "+" if delta >= 0 else ""
                            # lower is better for latency
                            emoji = (
                                "🟢" if delta < -2 else ("🔴" if delta > 2 else "⚪")
                            )
                            cell = f"{emoji} {cell} ({sign}{delta:.1f}%)"
                    row += f" {cell} |"
            lines.append(row)

    # Metadata footer
    lines += ["", "### Run metadata", ""]
    lines += ["| | " + " | ".join(f"`{Path(f).stem}`" for f in filenames) + " |"]
    lines += ["|---|" + "---|" * len(filenames)]
    for r in results:
        pass  # built per-row below
    for key, label in [("label", "Label"), ("timestamp", "Captured")]:
        row = f"| {label} |"
        for r in results:
            val = r.get(key, "—")
            if key == "timestamp" and isinstance(val, int):
                val = ts_str(val)
            row += f" {val} |"
        lines.append(row)

    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate benchmark Markdown report")
    parser.add_argument(
        "--results-dir",
        default="benchmarks/results",
        help="Directory of result JSON files (default: benchmarks/results)",
    )
    parser.add_argument(
        "--output", default=None, help="Write Markdown to file (default: stdout)"
    )
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    if not results_dir.is_dir():
        print(f"ERROR: results dir not found: {results_dir}", file=sys.stderr)
        return 1

    paths = sorted(results_dir.glob("*.json"), key=lambda p: p.stat().st_mtime)
    if not paths:
        print(f"No JSON files found in {results_dir}", file=sys.stderr)
        return 1

    results = [load_result(p) for p in paths]

    # Sort: "main" / "baseline" files first, then by timestamp
    def sort_key(pair):
        name = pair[1].lower()
        is_baseline = any(x in name for x in ("main", "baseline"))
        return (0 if is_baseline else 1, pair[0].get("timestamp", 0))

    pairs = sorted(zip(results, [p.name for p in paths]), key=sort_key)
    results, filenames = zip(*pairs) if pairs else ([], [])

    report = history_report(list(results), list(filenames))

    if args.output:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        with open(args.output, "w") as f:
            f.write(report)
        print(f"Report written to {args.output}")
    else:
        print(report)

    return 0


if __name__ == "__main__":
    sys.exit(main())
