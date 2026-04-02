# mvcc-bank MVCC Anomaly Test Results

**Branch:** `feat/concurrent-writes` (commit `b506ad5`)  
**Date:** 2026-04-02  
**Harness:** [adsharma/mvcc-bank](https://github.com/adsharma/mvcc-bank)  
**Build:** `RelWithDebInfo`, macOS arm64 (Apple M-series), Python 3.11

## What is tested

A graph-based port of the [Jepsen Bank test](https://github.com/jepsen-io/jepsen/blob/main/jepsen/src/jepsen/tests/bank.clj).
Multiple writer threads transfer balances between graph nodes concurrently;
multiple reader threads verify invariants within snapshot-isolated
`BEGIN TRANSACTION READ ONLY` transactions.

Anomalies checked:

| Anomaly                | Description                                                                     |
| ---------------------- | ------------------------------------------------------------------------------- |
| `balance_conservation` | `sum(balance)` must always equal the initial total                              |
| `negative_balance`     | No account may go below zero                                                    |
| `repeatable_read`      | Two identical MATCH queries in the same READ ONLY txn must return the same rows |
| `phantom_read`         | Aggregate predicates (count, sum) must be stable within one READ ONLY txn       |
| `graph_phantom_read`   | Edge-neighbourhood traversal must be stable within one READ ONLY txn            |

## Baseline (no `--multi-writes`, single-writer OCC)

```bash
# tools/python_api/
python -m pytest test/test_mvcc_bank.py::test_single_writer_no_anomalies -v
```

```
writes committed  : 5532
writes skipped    : 2456  (insufficient funds)
write conflicts   : 0     (retried)
writes failed     : 0     (gave up after retries)
reads ok          : 5491
reads failed      : 0
────────────────────────────────────────────────────
anomalies         : 0
```

✅ PASSED — no MVCC anomalies detected

## Multi-write (4 writers / 2 readers, 30 s)

```bash
# tools/python_api/
python -m pytest test/test_mvcc_bank.py::test_multi_writer_no_anomalies -v
```

```
writes committed  : 6593
writes skipped    : 2740  (insufficient funds)
write conflicts   : 4533  (retried — OCC, expected)
writes failed     : 0     (gave up after retries)
reads ok          : 6492
reads failed      : 0
────────────────────────────────────────────────────
anomalies         : 0
```

✅ PASSED — no MVCC anomalies detected

## Multi-write stress (8 writers / 4 readers, 60 s) — README example

```bash
# tools/python_api/
python -m pytest test/test_mvcc_bank.py::test_multi_writer_stress_no_anomalies -v -m slow
```

```
writes committed  : 8595
writes skipped    : 4297  (insufficient funds)
write conflicts   : 12203 (retried — OCC, expected)
writes failed     : 13    (gave up after retries)
reads ok          : 13287
reads failed      : 0
────────────────────────────────────────────────────
anomalies         : 0
```

✅ PASSED — no MVCC anomalies detected

## Notes

- Tests live in `tools/python_api/test/test_mvcc_bank.py` and run via `make pytest`
  or `python -m pytest test/test_mvcc_bank.py` from `tools/python_api/`.
- The `test_multi_writer_stress_no_anomalies` test is marked `@pytest.mark.slow`
  and is deselected by default; pass `-m slow` to include it.
- Write-write conflicts (`UpdateInfo::update` throws `Runtime exception: Write-write conflict`) are
  caught and retried by the application, consistent with ladybug's OCC model.
- The 13 "gave up after retries" in the stress run are transfers that exhausted their retry
  budget due to sustained contention — not data corruption.
- `enable_multi_writes=True` is passed via `lb.Database(path, enable_multi_writes=True)`.
  This requires the C++ and Python binding changes in this PR (gist p1 + p2).
