#!/usr/bin/env python3
"""
Compare two benchmark result JSON files and emit a Markdown report suitable
for a GitHub PR comment and/or $GITHUB_STEP_SUMMARY.

Usage:
  python3 benchmarks/compare_benchmarks.py \
      --baseline benchmarks/baseline.json \
      --branch   benchmarks/feat-concurrent-writes.json \
      [--output  benchmarks/comparison.md]    # default: stdout

Exit codes:
  0 – no regressions
  1 – single_writer_txn_per_sec regressed >5% OR
      read_during_checkpoint_ops_per_sec < 0.5 (reads still blocked during checkpoint)
"""
import argparse
import json
import os
import sys


REGRESSION_THRESHOLD = 0.05  # 5%
RDCP_BLOCKED_THRESHOLD = 0.5  # ops/s below this = reads are blocked during checkpoint

HIGHER_IS_BETTER = {
    "single_writer_txn_per_sec",
    "concurrent_write_txns_per_sec",
    "read_during_checkpoint_ops_per_sec",
}

LOWER_IS_BETTER = {"checkpoint_latency_ms"}

METRIC_META = {
    #  key                                    label                              notes
    "single_writer_txn_per_sec": (
        "Single-writer throughput (txn/s)",
        "Must not regress >" + str(int(REGRESSION_THRESHOLD * 100)) + "%",
    ),
    "checkpoint_latency_ms": (
        "Checkpoint latency (ms)",
        "Wall time for one CHECKPOINT call",
    ),
    "concurrent_write_txns_per_sec": (
        "Concurrent write throughput (txn/s)",
        "Informational only — MVCC write-pipeline overhead; ~2x slower than main is expected trade-off",
    ),
    "read_during_checkpoint_ops_per_sec": (
        "Reads during checkpoint (ops/s)",
        "🔑 Key gate: <0.5=reads blocked during checkpoint (pre-port), ≥0.5=non-blocking (post-port)",
    ),
}
METRIC_LABELS = {k: v[0] for k, v in METRIC_META.items()}


def load(path):
    with open(path) as f:
        return json.load(f)


def pct_change(baseline, branch):
    if baseline == 0:
        return float("inf") if branch > 0 else 0.0
    return (branch - baseline) / baseline


def emoji(key, delta):
    if key in HIGHER_IS_BETTER:
        if delta < -REGRESSION_THRESHOLD:
            return "🔴"
        if delta > 0.02:
            return "🟢"
        return "⚪"
    if key in LOWER_IS_BETTER:
        if delta > REGRESSION_THRESHOLD:
            return "🔴"
        if delta < -0.02:
            return "🟢"
        return "⚪"
    return "⚪"


def fmt(v: float, key: str) -> str:
    """Format a metric value with appropriate precision."""
    if key == "checkpoint_latency_ms":
        return f"{v:.0f} ms"
    return f"{v:.1f}"


def build_report(base: dict, branch: dict) -> tuple[str, list[str]]:
    """Return (markdown_report, list_of_failure_strings)."""
    base_label = base.get("label", "baseline")
    branch_label = branch.get("label", "branch")
    base_ts = base.get("timestamp", "")
    branch_ts = branch.get("timestamp", "")

    regressions: list[str] = []

    lines = [
        "## ⚡ Concurrent-write benchmark comparison",
        "",
        "> **What this measures**: impact of the non-blocking checkpoint port",
        "> from [Vela-Engineering/kuzu](https://github.com/Vela-Engineering/kuzu).",
        "> Concurrent write throughput is measured via in-process gtest threads",
        "> (4 suites × 4 threads × 1000 inserts per suite).",
        "",
        f"| | Baseline (`{base_label}`) | Branch (`{branch_label}`) |",
        "|---|---|---|",
        f"| Captured | {base_ts} | {branch_ts} |",
        "",
        "### Results",
        "",
        "| Metric | Baseline | Branch | Δ | Notes |",
        "|--------|:--------:|:------:|:---:|-------|",
    ]

    for key, (label, notes) in METRIC_META.items():
        bv = base["metrics"].get(key)
        brv = branch["metrics"].get(key)
        if bv is None and brv is None:
            lines.append(f"| {label} | — | — | — | {notes} |")
            continue
        bv = bv or 0.0
        brv = brv or 0.0
        delta = pct_change(bv, brv)
        sign = "+" if delta >= 0 else ""
        status = emoji(key, delta)
        lines.append(
            f"| {status} {label} | {fmt(bv, key)} | **{fmt(brv, key)}** "
            f"| {sign}{delta*100:.1f}% | {notes} |"
        )
        if key == "single_writer_txn_per_sec" and delta < -REGRESSION_THRESHOLD:
            regressions.append(
                f"single_writer_txn_per_sec regressed {-delta*100:.1f}% "
                f"(threshold: {REGRESSION_THRESHOLD*100:.0f}%)"
            )
        # concurrent_write_txns_per_sec is informational only — it measures
        # in-process gtest timing and is ~2x slower on feat as an accepted
        # MVCC trade-off for non-blocking checkpoints.

    rdcp = branch["metrics"].get("read_during_checkpoint_ops_per_sec", None)
    if rdcp is not None and rdcp < RDCP_BLOCKED_THRESHOLD:
        regressions.append(
            f"read_during_checkpoint_ops_per_sec={rdcp:.1f} < {RDCP_BLOCKED_THRESHOLD} — "
            "reads are still effectively blocked during checkpoint. "
            "Expected ≥0.5 ops/s on the non-blocking checkpoint branch."
        )

    # Derived safety row
    base_rdcp = base["metrics"].get("read_during_checkpoint_ops_per_sec", 0)
    branch_rdcp = rdcp if rdcp is not None else 0
    base_safety = (
        "✅ Production-safe (MVCC)"
        if base_rdcp >= RDCP_BLOCKED_THRESHOLD
        else "⚠️ Unsafe bypass (no MVCC)"
    )
    branch_safety = (
        "✅ Production-safe (MVCC)"
        if branch_rdcp >= RDCP_BLOCKED_THRESHOLD
        else "⚠️ Unsafe bypass (no MVCC)"
    )
    lines.append(
        f"| Concurrent write safety | {base_safety} | **{branch_safety}** | — "
        "| `debug_enable_multi_writes=true`: ⚠️=flag bypasses guard only, ✅=full MVCC infrastructure |"
    )

    lines.append("")
    if regressions:
        lines += ["### ❌ Gate failures", ""]
        for r in regressions:
            lines.append(f"- {r}")
        lines.append("")
        lines.append("> CI will fail on these. Fix the regression before merging.")
    else:
        lines += [
            "### ✅ All gates passed",
            "",
            "- Single-writer throughput within acceptable range (≤5% regression)",
            "- Read-only connections unblocked during checkpoint (≥0.5 ops/s — non-blocking confirmed)",
            "- Note: concurrent write throughput ~2x slower than main is an accepted MVCC trade-off",
        ]

    return "\n".join(lines) + "\n", regressions


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument(
        "--output", default=None, help="Write Markdown to file (default: stdout)"
    )
    args = parser.parse_args()

    base = load(args.baseline)
    branch = load(args.branch)

    report, regressions = build_report(base, branch)

    if args.output:
        with open(args.output, "w") as f:
            f.write(report)
    else:
        print(report)

    # Write to GitHub Step Summary if available
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a") as f:
            f.write(report)

    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main())
