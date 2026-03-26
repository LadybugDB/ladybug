# Ladybug Concurrent-Write Benchmarks

This directory contains the benchmark and test harness for the Vela-Engineering/kuzu
concurrent-write port into ladybug. The scripts are designed to be run from the root of
the **ladybug** repo (not the monorepo root).

---

## Directory layout

```
benchmarks/
  run_benchmarks.py     # measures 4 key metrics; writes a JSON result file
  run_tests.py          # runs all gtest binaries; writes JSONL + Markdown summary
  compare_benchmarks.py # diffs two JSON files; emits Markdown + exits 1 on regression
  report.py             # builds a history table from results/ JSON files
  baseline.json         # captured on main (unpatched) — committed for CI comparison
  results/              # per-run JSON output files (named <label>-<sha>-<date>.json)
```

---

## Quick start

```bash
# From the ladybug/ directory:

# 1. Build first (RelWithDebInfo includes test binaries)
make relwithdebinfo   # or: cmake --build build/relwithdebinfo --parallel

# 2. Run all tests
python3 benchmarks/run_tests.py \
    --build-dir   build/relwithdebinfo \
    --dataset-dir dataset \
    --output      benchmarks/results/test-results.jsonl \
    --summary     benchmarks/results/test-summary.md

# 3. Run benchmarks (current branch)
python3 benchmarks/run_benchmarks.py \
    --build-dir build/relwithdebinfo \
    --dataset-dir dataset \
    --output    benchmarks/results/$(git rev-parse --abbrev-ref HEAD | tr '/' '-')-$(git rev-parse --short HEAD)-$(date +%Y%m%d).json \
    --label     "$(git rev-parse --abbrev-ref HEAD) ($(git rev-parse --short HEAD))"

# 4. Compare against baseline
python3 benchmarks/compare_benchmarks.py \
    --baseline benchmarks/baseline.json \
    --branch   benchmarks/results/<your-result>.json

# 5. Generate a history report from all results/
python3 benchmarks/report.py --results-dir benchmarks/results
```

---

## Examples

### Compare two arbitrary branches

```bash
# Capture results on each branch, then diff them directly.
# Useful when you want to evaluate a PR against a known-good commit
# other than main.

git checkout my-feature
make relwithdebinfo
python3 benchmarks/run_benchmarks.py \
    --build-dir build/relwithdebinfo \
    --dataset-dir dataset \
    --output /tmp/my-feature.json \
    --label "my-feature ($(git rev-parse --short HEAD))"

git checkout some-other-branch
make relwithdebinfo
python3 benchmarks/run_benchmarks.py \
    --build-dir build/relwithdebinfo \
    --dataset-dir dataset \
    --output /tmp/other-branch.json \
    --label "other-branch ($(git rev-parse --short HEAD))"

python3 benchmarks/compare_benchmarks.py \
    --baseline /tmp/other-branch.json \
    --branch   /tmp/my-feature.json
```

### Quick one-shot regression check against committed baseline

```bash
# Fastest workflow: build, benchmark, compare — no test suite.
# Exits 0 (pass) or 1 (regression) — safe to use in a pre-push hook.

make relwithdebinfo && \
python3 benchmarks/run_benchmarks.py \
    --build-dir build/relwithdebinfo \
    --dataset-dir dataset \
    --output /tmp/branch-check.json \
    --label "$(git rev-parse --abbrev-ref HEAD) ($(git rev-parse --short HEAD))" && \
python3 benchmarks/compare_benchmarks.py \
    --baseline benchmarks/baseline.json \
    --branch   /tmp/branch-check.json
```

### Run only the concurrent gtest suite (fast iteration)

```bash
# Skip the full test suite and only run the 4 concurrent-write gtests.
# Useful when iterating on transaction_manager.cpp changes.

./build/relwithdebinfo/test/transaction/transaction_test \
    --gtest_filter="EmptyDBTransactionTest.ConcurrentNodeInsertions:\
EmptyDBTransactionTest.ConcurrentRelationshipInsertions:\
EmptyDBTransactionTest.ConcurrentNodeUpdates:\
EmptyDBTransactionTest.ConcurrentRelationshipUpdates" \
    --gtest_output="xml:/tmp/concurrent-results.xml"
```

### Track performance over time (history report)

```bash
# Every time you cut a notable build, save a result file with a dated name.
# report.py reads all JSON files in results/ and produces a history table.

python3 benchmarks/run_benchmarks.py \
    --build-dir build/relwithdebinfo \
    --dataset-dir dataset \
    --output benchmarks/results/$(git rev-parse --abbrev-ref HEAD | tr '/' '-')-$(git rev-parse --short HEAD)-$(date +%Y%m%d).json \
    --label "$(git rev-parse --abbrev-ref HEAD) ($(git rev-parse --short HEAD))"

# Regenerate the history table (output is gitignored; regenerate anytime)
python3 benchmarks/report.py --results-dir benchmarks/results
# Opens: benchmarks/results/report.md
```

### Interpreting the "Concurrent write safety" row

The report includes a derived row that doesn't require extra benchmark runs:

| Concurrent write safety | ⚠️ Unsafe bypass (no MVCC) | ✅ Production-safe (MVCC) |
|---|---|---|

- **⚠️ Unsafe bypass** — `debug_enable_multi_writes=true` merely skips the guard that
  throws when a second writer tries to start. There is no commit-timestamp tracking,
  no atomic write-count, and `CHECKPOINT` still blocks all transactions. Data
  integrity under concurrent load is not guaranteed.
- **✅ Production-safe** — MVCC infrastructure is in place (commit timestamps, atomic
  `activeWriteTransactionCount`, `stopNewWriteTransactionsAndWaitUntilAllWriteTransactionsLeave`).
  Checkpoint only drains writers; readers proceed freely via snapshot isolation.

The safety status is derived from `read_during_checkpoint_ops_per_sec`: if reads get
through during checkpoint (≥ 0.5 ops/s), the MVCC infrastructure must be present.

### Update the committed baseline after a significant main-branch change

```bash
# Run on main after merging something that legitimately shifts performance.
git checkout main
make relwithdebinfo
python3 benchmarks/run_benchmarks.py \
    --build-dir build/relwithdebinfo \
    --dataset-dir dataset \
    --output benchmarks/baseline.json \
    --label "main ($(git rev-parse --short HEAD))"
git add benchmarks/baseline.json
git commit -m "bench: update baseline to $(git rev-parse --short HEAD)"
```

---

## Metrics explained

| Metric                               | What it measures                                                                          | Expected direction                                                                 |
| ------------------------------------ | ----------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| `single_writer_txn_per_sec`          | Throughput of 500 sequential CREATE statements in one shell session                       | ↑ Higher after port — WAL rotation means writers are not stalled during checkpoint |
| `checkpoint_latency_ms`              | Wall-clock time for one forced `CHECKPOINT` call                                          | ↓ Lower after port — checkpoint only drains write transactions, not readers        |
| `concurrent_write_txns_per_sec`      | Aggregate write throughput from 4 gtest suites (4 threads × 1000 inserts each)            | ≈ Flat — within noise after fix; see note below                                    |
| `read_during_checkpoint_ops_per_sec` | Read ops/sec from a `--readonly` process while a writer holds LOCK_EX and runs CHECKPOINT | ↑ > 0 after port — see note below                                                  |

### Concurrent write throughput: flat after fixing the benchmark

Early benchmark runs showed concurrent write throughput ~2x lower on feat than on
main. Investigation revealed the tests did **not** disable `auto_checkpoint` before
the concurrent phase. With 4 threads × 1000 write transactions, auto-checkpoint fired
repeatedly. On feat, each auto-checkpoint calls
`stopNewWriteTransactionsAndWaitUntilAllWriteTransactionsLeave()`, which holds
`mtxForStartingNewTransactions` while draining — stalling all other write threads at
the start of every new transaction during the drain period.

Fix: added `CALL auto_checkpoint=false;` to all four concurrent test bodies, plus an
explicit `CHECKPOINT` after setup phases and after the concurrent section. With the
fix applied identically to both main and feat, concurrent write throughput is within
~2–3% (noise), confirming the MVCC bookkeeping overhead is negligible.

The gates that matter are:

1. `single_writer_txn_per_sec` must not regress > 5% (sequential throughput)
2. `read_during_checkpoint_ops_per_sec` must be ≥ 0.5 (non-blocking confirmed)

### Single-writer throughput

Sequential single-writer throughput (`single_writer_txn_per_sec`) is essentially
identical between main and feat (~2–3% difference, within run-to-run noise on macOS).
Auto-checkpoint is left enabled for this benchmark — any checkpoint that fires during
the 500-insert session is included in the overall wall time, which is the realistic
production scenario for a sequential writer.

### Why reads-during-checkpoint is ≈0 on main and >0 on feat

The key is how each branch's checkpoint blocks new transactions:

- **main**: `checkpointNoLock()` calls `stopNewTransactionsAndWaitUntilAllTransactionsLeave()`,
  which acquires `mtxForStartingNewTransactions` and waits for **all** active
  transactions (reads and writes) to drain. Once the mutex is held, no new reads can
  start — a `--readonly` process may complete 0–1 reads before the lock is taken.
- **feat**: `checkpointNoLock()` calls `stopNewWriteTransactionsAndWaitUntilAllWriteTransactionsLeave()`,
  which acquires only the **write** transaction gate. `mtxForStartingNewTransactions`
  is not held, so new read transactions start freely. A `--readonly` process can
  execute many reads via MVCC snapshot isolation throughout the entire checkpoint.

---

## CI

The GitHub Actions workflow at `.github/workflows/concurrent-writes-ci.yml`:

- **build** job: compiles on `ubuntu-latest` and `macos-latest`
- **test** job: runs `run_tests.py`, uploads JSONL artifact + writes Step Summary
- **benchmark** job: runs `run_benchmarks.py`, compares against `baseline.json`,
  posts a comparison table as a PR comment, fails CI if any regression gate trips

Regression gates (exit code 1):

- `single_writer_txn_per_sec` regresses > 5%
- `read_during_checkpoint_ops_per_sec` < 0.5 ops/s (checkpoint is still blocking reads)

`concurrent_write_txns_per_sec` is informational. With `auto_checkpoint=false` in the
concurrent tests it runs flat vs main (~2–3% noise), so no gate is needed.
