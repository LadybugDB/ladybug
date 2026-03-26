#!/usr/bin/env python3
"""
Run ladybug gtest binaries, capture results as JSONL + Markdown summary.

Usage:
  python3 benchmarks/run_tests.py \
      --build-dir build/relwithdebinfo \
      --dataset-dir dataset \
      --output     benchmarks/test-results.jsonl \
      --summary    benchmarks/test-summary.md \
      [--filter    "Checkpoint|WAL|Transaction"]   # optional gtest filter

The JSONL file has one record per test case:
  {"binary":"transaction_test","suite":"FlakyCheckpointerTest",
   "name":"RecoverFromCheckpointStorageFailure","status":"PASSED",
   "duration_ms":12708,"error":null,"skipped":false}

The Markdown summary is suitable for posting as a PR comment or writing to
$GITHUB_STEP_SUMMARY.

Exit codes:
  0 — all tests passed (or only skipped)
  1 — at least one test failed
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


# gtest binaries to run in order. Paths relative to the build test/ dir.
TEST_BINARIES = [
    ("transaction/transaction_test", "Transaction & Checkpoint"),
    ("storage/buffer_manager_test", "Buffer Manager"),
    ("storage/compression_test", "Compression"),
    ("storage/column_chunk_metadata_test", "Column Chunk Metadata"),
    ("storage/local_hash_index_test", "Local Hash Index"),
    ("storage/node_update_test", "Node Update"),
    ("storage/node_insertion_deletion_test", "Node Insert/Delete"),
    ("storage/detach_delete_test", "Detach Delete"),
    ("common/types_test", "Common Types"),
    ("api/api_test", "API"),
]


def parse_xml(xml_path: Path, binary_name: str) -> list[dict]:
    """Parse a gtest XML file into a list of result dicts."""
    results = []
    try:
        tree = ET.parse(xml_path)
    except ET.ParseError:
        return results
    root = tree.getroot()
    for suite in root.iter("testsuite"):
        suite_name = suite.get("name", "")
        for tc in suite.iter("testcase"):
            name = tc.get("name", "")
            status = tc.get("result", "run")  # "run", "skipped", "suppressed"
            duration_s = float(tc.get("time", 0))
            duration_ms = int(duration_s * 1000)
            skipped = status in ("skipped", "suppressed")
            failure_nodes = list(tc.iter("failure")) + list(tc.iter("error"))
            if failure_nodes:
                error_text = "\n".join(
                    f.get("message", f.text or "") for f in failure_nodes
                )
                test_status = "FAILED"
            elif skipped:
                error_text = None
                test_status = "SKIPPED"
            else:
                error_text = None
                test_status = "PASSED"
            results.append(
                {
                    "binary": binary_name,
                    "suite": suite_name,
                    "name": name,
                    "status": test_status,
                    "duration_ms": duration_ms,
                    "error": error_text,
                    "skipped": skipped,
                }
            )
    return results


def run_binary(exe: Path, xml_out: Path, env: dict, gtest_filter: str | None) -> int:
    """Run one gtest binary; return exit code."""
    cmd = [str(exe), f"--gtest_output=xml:{xml_out}"]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
    result = subprocess.run(
        cmd, env={**os.environ, **env}, capture_output=True, text=True, timeout=600
    )
    return result.returncode


def summary_markdown(all_results: list[dict], label: str) -> str:
    """Produce a Markdown summary table from all test results."""
    total = len(all_results)
    passed = sum(1 for r in all_results if r["status"] == "PASSED")
    failed = sum(1 for r in all_results if r["status"] == "FAILED")
    skipped = sum(1 for r in all_results if r["status"] == "SKIPPED")

    # Top-level badge line
    if failed == 0:
        badge = f"✅ **{passed}/{total} passed**"
    else:
        badge = f"❌ **{failed} failed** / {passed} passed / {skipped} skipped (total {total})"

    lines = [
        f"## Test results — {label}",
        "",
        badge,
        "",
    ]

    # Per-binary summary table
    lines += [
        "| Binary | Tests | ✅ Pass | ❌ Fail | ⏭ Skip |",
        "|--------|------:|-------:|-------:|-------:|",
    ]
    by_binary: dict[str, list] = {}
    for r in all_results:
        by_binary.setdefault(r["binary"], []).append(r)
    for bin_name, recs in sorted(by_binary.items()):
        t = len(recs)
        p = sum(1 for r in recs if r["status"] == "PASSED")
        f = sum(1 for r in recs if r["status"] == "FAILED")
        s = sum(1 for r in recs if r["status"] == "SKIPPED")
        icon = "✅" if f == 0 else "❌"
        lines.append(f"| {icon} `{bin_name}` | {t} | {p} | {f} | {s} |")

    # Failed test details
    failures = [r for r in all_results if r["status"] == "FAILED"]
    if failures:
        lines += ["", "### ❌ Failed tests", ""]
        for r in failures:
            lines.append(
                f"<details><summary><code>{r['binary']} :: {r['suite']}.{r['name']}</code></summary>"
            )
            lines.append("")
            lines.append("```")
            lines.append((r["error"] or "").strip()[:2000])
            lines.append("```")
            lines.append("")
            lines.append("</details>")

    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run ladybug tests and capture JSONL output"
    )
    parser.add_argument("--build-dir", default="build/relwithdebinfo")
    parser.add_argument("--dataset-dir", default="dataset")
    parser.add_argument(
        "--output", default="benchmarks/test-results.jsonl", help="JSONL output file"
    )
    parser.add_argument(
        "--summary",
        default="benchmarks/test-summary.md",
        help="Markdown summary output file",
    )
    parser.add_argument(
        "--filter", default=None, help="gtest filter string (applied to all binaries)"
    )
    parser.add_argument(
        "--label", default="branch", help="Label shown in the markdown summary"
    )
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    test_dir = build_dir / "test"
    dataset_dir = Path(args.dataset_dir).resolve()

    # Environment variables that tests need
    env = {
        "E2E_TEST_FILES_DIRECTORY": str(Path("test/test_files").resolve()),
        "KUZU_DATASET_PATH": str(dataset_dir),
        "LBUG_DATASET_PATH": str(dataset_dir),
    }

    all_results: list[dict] = []
    any_failure = False

    os.makedirs(Path(args.output).parent, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmpdir:
        for rel_path, display_name in TEST_BINARIES:
            exe = test_dir / rel_path
            if not exe.is_file():
                print(f"  skip (not built): {rel_path}", file=sys.stderr)
                continue
            xml_out = Path(tmpdir) / f"{exe.name}.xml"
            print(f"  running {display_name} ({exe.name}) ...", flush=True)
            try:
                rc = run_binary(exe, xml_out, env, args.filter)
            except subprocess.TimeoutExpired:
                print(f"    TIMEOUT", file=sys.stderr)
                all_results.append(
                    {
                        "binary": exe.name,
                        "suite": "",
                        "name": "__TIMEOUT__",
                        "status": "FAILED",
                        "duration_ms": 600000,
                        "error": "Test binary timed out after 600s",
                        "skipped": False,
                    }
                )
                any_failure = True
                continue

            results = parse_xml(xml_out, exe.name) if xml_out.exists() else []
            all_results.extend(results)
            n_fail = sum(1 for r in results if r["status"] == "FAILED")
            n_pass = sum(1 for r in results if r["status"] == "PASSED")
            n_skip = sum(1 for r in results if r["status"] == "SKIPPED")
            print(f"    pass={n_pass} fail={n_fail} skip={n_skip}", flush=True)
            if n_fail > 0 or (rc != 0 and not results):
                any_failure = True

    # Write JSONL
    with open(args.output, "w") as f:
        for r in all_results:
            f.write(json.dumps(r) + "\n")

    # Write markdown summary
    summary = summary_markdown(all_results, args.label)
    with open(args.summary, "w") as f:
        f.write(summary)

    # Write to GitHub Step Summary if available
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a") as f:
            f.write(summary)

    print(f"\nResults: {args.output}")
    print(f"Summary: {args.summary}")
    total = len(all_results)
    passed = sum(1 for r in all_results if r["status"] == "PASSED")
    failed = sum(1 for r in all_results if r["status"] == "FAILED")
    print(f"Total: {total}  Passed: {passed}  Failed: {failed}")

    return 1 if any_failure else 0


if __name__ == "__main__":
    sys.exit(main())
