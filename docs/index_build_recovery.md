# Index Build Recovery and Online Build Notes

## Current recovery invariant

A committed valid index must have recoverable physical index storage. For ART indexes created after
the last checkpoint, `CREATE INDEX` writes a physical `CREATE_INDEX_RECORD` to the WAL. The record
contains the index catalog entry, `IndexInfo`, and serialized ART tree bytes. WAL replay restores the
catalog entry if needed, installs the ART bytes into data-file pages, and attaches the index to the
node table.

This matches the small/normal-index shape used by DuckDB and PostgreSQL: recovery replays physical
index storage instead of rescanning the base table.

## Large index optimization

For very large ART indexes, writing the full serialized tree into the WAL can duplicate many
gigabytes of data. `CREATE ART INDEX` switches to a blocking checkpoint-instead-of-WAL path when the
serialized ART tree is larger than the configured threshold. The statement returns a message saying
this path is active, commits a small catalog WAL record, and forces a blocking checkpoint before
returning to the user. If the process crashes before that checkpoint completes, WAL replay sees the
catalog index entry and rebuilds the physical ART index by scanning the base table.

The default threshold is 256 MiB. Tests can override it with
`LBUG_CREATE_INDEX_WAL_THRESHOLD=<bytes>`.

The key safety rule is that there must be no window where a valid catalog index entry is committed
without durable physical index pages or a WAL record/recovery path that can recreate them.

## Future abandoned-build GC

A cheaper crash-recovery path for large index builds would write index pages before commit and keep
explicit build state:

1. Build the index into private storage while the catalog entry is not visible to queries.
2. At commit, hold the write gate and persist the index pages through the checkpoint/shadow protocol.
3. Publish the catalog entry only after the physical pages are durable.
4. Commit with a small WAL record that points at the durable index storage, or with no index WAL
   payload if the checkpoint record fully covers the transaction.

If a crash happens during the build, recovery should ignore incomplete build metadata and let normal
space reclamation garbage collect the abandoned index pages later.

## Future online build

Online index creation should use an explicit index state machine:

- `BUILDING`: catalog entry exists for coordination, but the optimizer ignores it.
- `CATCHING_UP`: the base snapshot scan has finished and pending writes are being merged.
- `VALID`: the optimizer can use the index.
- `INVALID`: build failed or recovery found an incomplete build; queries ignore it until dropped or
  rebuilt.

The build should scan a snapshot of the table in parallel and maintain a side delta for concurrent
writes. At validation time, merge the delta, wait for conflicting snapshots when needed, and atomically
mark the index `VALID`.

## Parallelism and read latency

Parallel index creation should use a global memory budget for the whole build, not one full budget per
worker. The planner/build scheduler should choose worker count from table size, available cores, and
the configured memory budget.

To reduce read latency impact:

- Prefer sequential table scans and sequential index page writes.
- Avoid loading newly built index pages through the normal buffer-cache hot path.
- Throttle build I/O when foreground read latency rises.
- Keep generated index pages in private build buffers until they are published.
- Make the optimizer ignore `BUILDING`/`INVALID` indexes.

PostgreSQL's standard `CREATE INDEX` allows reads while blocking writes. Its `CREATE INDEX
CONCURRENTLY` keeps writes running at the cost of extra scans and validation waits. Ladybug can follow
that split: a cheaper blocking-write build first, then a fully online build path.
