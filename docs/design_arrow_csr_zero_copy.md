# Design: Zero-copy CSR path for `ArrowResultCollector` / `ArrowQueryResult`

## 1. Problem

Reading ~0.75B edges from CSR columnar storage into Arrow CSR memory should
cost:

| array     | size                | bytes (0.75B edges, ~100M src nodes) |
|-----------|---------------------|--------------------------------------|
| `indices` | #edges              | 0.75B × 8 = **6.0 GB**               |
| `indptr`  | #sourceRows + 1     | 100M × 8 = **0.8 GB**                |
| `edgeIDs` | #edges (optional)   | 0.75B × 8 = **6.0 GB**               |

Expected total ≈ **6.8 GB** (no rel rowid) or **12.8 GB** (with rel rowid).
Observed: **~45 GB**. The query runs the `DirectArrowResultCollector` path
(`getArrowChunks().empty()` is asserted in `arrow_test.cpp`), so the per-column
`ArrowArray` materialization is *already* skipped. The 45 GB lives entirely in
`ArrowResultCollectorLocalState::csrMetadata`.

## 2. Root cause — per-batch dense-global `indptr`

`updateDirectCSRMetadata` builds, per local collector (one per thread/batch), a
CSR whose `indptr` is indexed by **global** source row id. To stay indexable it
fills `indptr` densely from global row 0 up to the batch's max source row, then
`executeInternal` pads the tail:

```cpp
while (localState.nextSourceRowID + 1 < metadata.numSourceRows) {
    localState.nextSourceRowID++;
    metadata.indptr.push_back(static_cast<int64_t>(metadata.indices.size()));
}
```

So **every batch's `indptr` has `numSourceRows + 1` entries** (≈800 MB each),
regardless of how many distinct source rows that batch actually touches. With
~49–56 parallel batches (one per thread over a 0.75B-edge scan), the indptr
memory alone is `B × 800 MB ≈ 40–45 GB`. The `indices` (6 GB) and `edgeIDs`
(6 GB) are *not* duplicated per batch — each edge lives in exactly one batch —
so they are already at their theoretical size. **The entire ~38 GB of bloat is
`indptr` duplicated across batches.**

The `kwayMergeCSRChunks` merge then copies the per-batch `indices`/`edgeIDs`
into a single flat `CSRMetadata` (cached in `combinedCSR`) while the per-batch
`csrChunks` vector stays alive in `ArrowQueryResult`, so the *transient* peak at
first consumer access is even higher (~per-batch 45 GB + merged 12.8 GB). The
45 GB the user sees is the steady-state pre-merge cost.

There is also a secondary issue: `kwayMergeCSRChunks` is
`O(maxSrcRow × numChunks)` (it loops every global source row × every chunk),
which is ~5 billion iterations for this query — slow, but dominated by the
memory problem.

## 3. The invariant we can exploit

Two properties of the scan make a sparse per-batch representation correct:

1. **CSR storage + monotonic morsel acquisition ⇒ per-thread source rows are
   non-decreasing.** The rel table is a `CSRNodeGroup`; a `ScanRelTable` extend
   emits each bound node's outgoing edges contiguously. The bound-node scan
   hands out morsels from an atomic counter
   (`ScanNodeTableSharedState::nextMorsel` does `currentCommittedGroupIdx++`),
   so each thread receives a *strictly increasing* sequence of morsels, hence a
   non-decreasing sequence of `srcRowID`s. (`sourceMode` is single-threaded and
   therefore not the 45 GB case; the parallel extend path is.)

2. **Each source row's edges land in exactly one batch.** A source row (node)
   is scanned in exactly one morsel, which goes to exactly one thread. So the
   per-batch sets of touched source rows are **disjoint** and each is sorted.

Consequence: a batch never needs a dense global `indptr`. It only needs to
record *which* source rows it touched and *how many edges* each has — a sparse
CSR. The global dense `indptr` (800 MB) is reconstructed **once**, at merge
time, by unioning the disjoint sorted per-batch source-row sets.

The existing code already enforces the precondition: if `srcRowID` ever
decreases within a batch (e.g. `ORDER BY` a non-src column reorders tuples),
`updateDirectCSRMetadata` sets `csrMetadataValid = false` and drops the
metadata. The sparse design inherits exactly this bail-out — no new ordering
requirement is introduced.

## 4. Proposed data structure — sparse per-batch CSR

Replace the dense-global `indptr` in the per-batch `CSRMetadata` with a sparse
run representation. While the source row is non-decreasing we record, per
distinct source row, its id and its edge count:

```
struct CSRMetadata {            // per-batch, as collected
    std::vector<int64_t> indices;     // dst rowids, source-sorted within batch  (6 GB total across batches)
    std::vector<int64_t> edgeIDs;     // optional, parallel to indices           (6 GB total)
    // SPARSE indptr — replaces the dense global std::vector<int64_t> indptr:
    std::vector<int64_t> srcRows;     // distinct source rows touched, increasing (100M total across batches = 0.8 GB)
    std::vector<int64_t> counts;      // #edges per srcRows[i]                    (100M total = 0.8 GB)
    bool hasEdgeIDs = false;
    int64_t numSourceRows = 0;
};
```

Per-batch overhead drops from `numSourceRows × 8` (800 MB) to
`distinctSrcRowsInBatch × 16` (≈ a few MB). **Summed across all batches the
sparse structure is 100M × 16 = 1.6 GB**, replacing the 40+ GB of duplicated
dense `indptr`.

### Build (replace `updateDirectCSRMetadata`)

State carried in `ArrowResultCollectorLocalState`: `currentSourceRowID`,
`currentRowCount` (edges seen for the current source row).

```
on each (src, dst [, edge]):
    if src < currentSourceRowID:          // ordering violated (ORDER BY non-src)
        csrMetadataValid = false; reset; return
    if src != currentSourceRowID:
        srcRows.push_back(currentSourceRowID)
        counts .push_back(currentRowCount)
        currentSourceRowID = src
        currentRowCount = 0
    indices.push_back(dst)                // (and edgeIDs.push_back(edge))
    currentRowCount++
on batch end:
    if currentRowCount > 0 (or we saw any edge):
        srcRows.push_back(currentSourceRowID)
        counts .push_back(currentRowCount)
```

No gap-filling, no trailing pad to `numSourceRows` — those are the merge's job.
The dense `indptr` member is removed from the per-batch struct entirely.

### Merge (replace `kwayMergeCSRChunks`)

Inputs: `chunks[]`, each `{srcRows, counts, indices, edgeIDs}` with disjoint,
sorted `srcRows`. Output: one flat `CSRMetadata{indptr, indices, edgeIDs}`.

Because the `srcRows` partition is disjoint and each chunk's edges are already
grouped by source row, this is a **union of sorted disjoint runs**, not a
general k-way merge:

1. One pass to sum `counts` → `totalEdges` (reserve `indices`/`edgeIDs`).
2. Build the merged `indices`/`edgeIDs` by emitting each chunk's edges in
   **global source-row order**. Concretely: a min-heap (or, since disjoint, a
   simple k-way finger) over the chunks' `srcRows` cursors; pop the smallest
   next source row, copy that source row's `counts[i]` edges from the owning
   chunk into the merged `indices`, and record `indptr[src]` = current merged
   offset. Source rows not present in any chunk get
   `indptr[src] = indptr[src-1]` (empty). This is
   `O(totalSrcRows + totalEdges × log(numChunks))`; with disjoint runs the heap
   is tiny and the `indices` copy is a sequential write per source row.

   *The `indices` copy is a 6 GB write — unavoidable while the API exposes a
   flat `indices` vector (see Phase 2 to eliminate it).*

3. Pad `indptr` to `numSourceRows + 1` (trailing empty nodes), as today.

`mergeCSRMetadata` (the FIXED_ORDER pairwise path) is folded into the same
sparse merge: FIXED_ORDER-by-src already preserves the non-decreasing
invariant, so it takes the same cheap path. FIXED_ORDER-by-non-src already
bails out with `csrMetadataValid = false` today and is unaffected.

## 5. Memory accounting

Steady state after collection, before merge (the 45 GB regime the user sees):

| component                       | before (dense indptr) | after (sparse)        |
|---------------------------------|-----------------------|-----------------------|
| per-batch `indices`             | 6.0 GB                | 6.0 GB                |
| per-batch `edgeIDs` (optional)  | 6.0 GB                | 6.0 GB                |
| per-batch `indptr` / sparse     | B × 0.8 ≈ **40 GB**   | 1.6 GB (srcRows+counts) |
| **total (no rel rowid)**        | **~46 GB**            | **~7.6 GB**           |
| **total (with rel rowid)**      | **~52 GB**            | **~13.6 GB**          |

Peak at first `combineCSRChunks()` (per-batch chunks still referenced by
`csrChunks` while merged `combinedCSR` is built):

| component                       | before                | after (Phase 1)       |
|---------------------------------|-----------------------|-----------------------|
| per-batch sparse + indices/edge | ~52 GB                | ~13.6 GB              |
| merged indptr + indices + edge  | +12.8 GB              | +12.8 GB              |
| **peak (with rel rowid)**       | **~65 GB**            | **~26 GB**            |

## 6. Phased plan

### Phase 1 — sparse per-batch CSR + flat merge (API-compatible, the big win)

- Introduce `CSRMetadata::{srcRows, counts}` (sparse) on the **per-batch** path;
  remove the dense `indptr` from per-batch collection.
- Rewrite `updateDirectCSRMetadata` / `updateCSRMetadata` to the run-based
  builder in §4. Drop the gap-fill and trailing-pad loops from
  `executeInternal` / `DirectArrowResultCollector::executeInternal`.
- Rewrite `kwayMergeCSRChunks` to the disjoint-union merge in §4. The merged
  `CSRMetadata` retains the *dense* `indptr` + flat `indices`/`edgeIDs` so
  `getCSRArrowArrays()`, `makeCSRArrowArray` (which already aliases the merged
  vector), and the existing test (`metadata.indices[idx]`,
  `metadata.indptr[src]`) are unchanged.
- Fold `mergeCSRMetadata` (FIXED_ORDER pairwise) into the sparse merge, or keep
  it as a thin sort-then-sparse-merge shim for the FIXED_ORDER-by-non-src bail
  case.
- **Result: 45 GB → ~7.6 GB steady-state (no rel rowid), ~13.6 GB with rel
  rowid. No API change, no consumer change.** This alone closes the reported
  gap.

### Phase 2 — chunked zero-copy `indices` (reach the theoretical floor)

The merged `indices`/`edgeIDs` are currently a 6 GB *copy* of the per-batch
vectors. Eliminate the copy and the transient peak by exposing the output as
**chunked** Arrow arrays (which is what the user described: "chunked array
indptr and chunked array indices"):

- `ArrowQueryResult` keeps the per-batch `indices`/`edgeIDs` vectors *as the
  Arrow chunks* (one `ArrowArray` per batch, aliasing each batch's vector via
  the existing `CSRArrowArrayHolder` mechanism — already zero-copy from vector
  to `ArrowArray`).
- `indptr` stays a single dense `ArrowArray` (800 MB), but its values are
  **global offsets into the logical concatenation of the indices chunks**.
  Because each source row's edges live in exactly one chunk, every
  `[indptr[i], indptr[i+1])` range falls within a single chunk; the consumer
  resolves `globalOffset → (chunkIdx, localOffset)` via a tiny chunk-offset
  table (numChunks entries).
- `getCSRArrowArrays()` returns `CSRArrowArrays` whose `indices`/`edgeIDs`
  become `ArrowChunkedArray` (the existing `ArrowChunkedArray` view type already
  models this). pyarrow consumers can use the chunks directly or call
  `combine_chunks()` on their own memory budget if they need a flat array.

Phase 2 peak = per-batch indices (6 GB) + edgeIDs (6 GB) + dense indptr
(0.8 GB) + sparse srcRows/counts (1.6 GB, freed once indptr is built) =
**~12.8 GB with rel rowid, ~6.8 GB without — the theoretical minimum.** No
tuple materialization at any point: `src` is never stored per edge, only the
run-length sparse structure (1.6 GB) which is derived from the scan's
non-decreasing source order.

Phase 2 is an API widening (flat `indices` → chunked `indices`); the C++ test
that does `metadata.indices[idx]` would move to a chunked-accessor. Land
Phase 1 first (pure win, no API change), then Phase 2 as a follow-on.

## 7. Edge cases & invariants preserved

- **Ordering violation (`ORDER BY` non-src):** `csrMetadataValid = false` as
  today; batch contributes no CSR. Unchanged.
- **Empty batch / no edges:** `srcRows` empty; merge skips it. `indptr` still
  padded to `numSourceRows + 1` of zeros.
- **Single batch (1 thread / source mode):** sparse struct has 100M srcRows +
  counts (1.6 GB) instead of dense 0.8 GB indptr — *slightly worse* for the
  single-threaded case, but the merge collapses it to the 0.8 GB dense indptr
  and frees the sparse struct. Net neutral vs. today for 1 thread, massive win
  for many threads. (If single-thread perf matters, the merge can special-case
  `chunks.size() == 1` to build the dense `indptr` directly from `counts`
  without copying `indices` — Phase 2 makes this free in general.)
- **Disjoint-srcRows invariant:** relied on for the union merge. It holds
  because a source node is scanned in exactly one morsel → one thread. If a
  future scan mode ever splits a source node's edges across morsels (it does
  not today), the merge must fall back to a general k-way merge by src; add a
  debug assert that per-batch `srcRows` sets are disjoint (sum of `counts` ==
  total `indices`).
- **`makeCSRArrowArray` aliasing:** unchanged — it already aliases the merged
  `CSRMetadata` vectors via a `shared_ptr` to `combinedCSR`. Phase 2 reuses the
  same holder to alias per-batch vectors instead.
- **Determinism / batch order:** the disjoint-union merge emits source rows in
  global id order regardless of batch index, so NO_ORDER / INSERTION_ORDER /
  FIXED_ORDER-by-src all produce identical output. Batch-index ordering only
  matters for the chunk *layout* in Phase 2, not for correctness.

## 8. Files touched

- `src/processor/operator/arrow_result_collector.cpp` (+ header) —
  `updateDirectCSRMetadata`, `updateCSRMetadata`, `mergeCSRMetadata`, the
  trailing-pad loops in both `executeInternal`s; `CSRMetadata` per-batch shape.
- `src/main/query_result/arrow_query_result.cpp` (+ header) —
  `kwayMergeCSRChunks`; `CSRMetadata` struct; `combineCSRChunks`; (Phase 2)
  `getCSRArrowArrays` / `ArrowChunkedArray` for `indices`.
- Tests: `test/api/arrow_test.cpp`, `test/api/arrow_csr_rel_table_test.cpp`,
  `test/api/fwd_arrow_repro_test.cpp` — unchanged for Phase 1 (they consume the
  merged dense API); new chunked-access tests for Phase 2.
