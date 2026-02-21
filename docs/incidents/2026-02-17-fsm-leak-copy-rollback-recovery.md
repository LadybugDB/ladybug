# Incident: FSM leak after COPY + ROLLBACK + RELOAD DB

**Date:** 2026-02-17
**Status:** Analysis
**Severity:** Medium (two e2e tests fail; FSM leak checker)
**Component:** Storage — FreeSpaceManager, PageManager, transaction rollback, COPY
**Affected tests:** `CreateNodeAndCopyRelRollbackRecovery`, `CopyNodeAndRelRollbackRecovery`

---

## Current state

- **Observed:** Both tests run: BEGIN TRANSACTION → CREATE tables + COPY data → ROLLBACK → RELOAD DB. After the test, `FSMLeakChecker::checkForLeakedPages` runs (drops all tables, checkpoint, then checks used pages). It expects `numUsedPages == 4` (header + catalog + metadata) but gets **95**.
- **Goal:** After ROLLBACK and RELOAD DB, the DB should have no user data and only 4 used pages; the free space manager (FSM) should treat all other pages as free.
- **Existing behaviour:**
  - COPY uses batch insert operators that take an `OptimisticAllocator` from the transaction’s `LocalStorage`. Allocations go through `OptimisticAllocator::allocatePageRange` (which calls `PageManager::allocatePageRange`; when the FSM is empty, `FileHandle::addNewPages` extends the file).
  - On transaction rollback, `LocalStorage::rollback()` calls `OptimisticAllocator::rollback()` for each allocator, which calls `PageManager::freeImmediatelyRewritablePageRange` → `FreeSpaceManager::evictAndAddFreePages` → `addFreePages`. So rolled-back COPY pages are added to the **main** free list (`freeLists`), not only to `uncheckpointedFreePageRanges`.
  - RELOAD DB in the test does `database.reset()` then `make_unique<Database>(databasePath, ...)`. So the old database is closed (destructor runs checkpoint if `forceCheckpointOnClose`), then a new database is opened from the same path. The checkpoint should serialize the FSM (including `freeLists`) and the new load should deserialize it.
- **Gap:** Either (1) some pages allocated during COPY are not tracked by `OptimisticAllocator` and are never returned to the FSM on rollback, or (2) the checkpoint/reload path does not persist or restore the FSM state so that those 91 pages are reflected as free after reload. The observed 95 used pages imply 91 pages are still counted as used after reload.

---

## Options and analysis

| Option | Description | Feasibility | Effort | Risk |
|--------|-------------|-------------|--------|------|
| A. Audit COPY/transaction allocation paths | Trace every page allocation during COPY and rollback; ensure all use OptimisticAllocator (or an equivalent that returns pages on rollback). Fix any path that allocates without tracking. | High | Medium | Low; targeted fix. |
| B. Ensure checkpoint-on-close persists FSM before reload | Verify that when we `database.reset()` for RELOAD, the checkpoint in `Database::~Database()` runs and that FSM serialization includes the free list written by rollback. Fix order or truncation if the tail free range is dropped or not serialized. | High | Low–Medium | Medium; may interact with truncation/serialization order. |
| C. Skip FSM leak check for ROLLBACK+RELOAD tests | Set `skipFSMLeakCheckerFlag` for these two test cases so CI passes while the leak is investigated. | High | Low | Does not fix the leak; test-only workaround. |
| D. Force checkpoint before RELOAD in test | In the test runner, when handling RELOAD DB, run an explicit checkpoint before `createDB()` so that in-memory FSM state (freed pages) is persisted before the database is destroyed. | Medium | Low | If the bug is “no checkpoint between rollback and reload”, this fixes it; if the bug is “some pages never freed”, it does not. |
| E. Merge free pages before FSM serialization in checkpointer | Ensure `finalizeCheckpoint` (or equivalent) runs before FSM is serialized so that any in-memory free ranges (including those added by rollback to `freeLists`) are merged and the tail is truncated before we write the FSM. | Medium | Medium | Current order is: serialize metadata (includes FSM) then `finalizeCheckpoint`; FSM already has rollback pages in `freeLists`, so order may be secondary; need to confirm truncation vs. serialization. |

**Conclusion:** Option A is the only one that directly addresses a possible root cause (untracked allocations). Option B is the other main candidate (persistence/restore of FSM). Option D is a small test-side change that might fix the symptom if the issue is missing checkpoint before reload. Option C is a temporary workaround; Option E needs validation that the current order is wrong.

---

## Validation

- **Option A:** Code paths: `node_batch_insert.cpp` and `rel_batch_insert.cpp` use `transaction->getLocalStorage()->addOptimisticAllocator()`. Other storage layers (e.g. WAL, catalog, metadata) may allocate pages via `PageManager::allocatePageRange` or `freePageRange` (which uses `addUncheckpointedFreePages`). If any COPY-related path allocates via the raw PageManager or another allocator that does not participate in `LocalStorage::rollback()`, those pages would not be returned on rollback. Evidence to gather: add logging or assert that all allocations during a COPY transaction go through an optimistic allocator that is rolled back.
- **Option B:** Checkpointer order: `writeCheckpoint()` calls `checkpointStorage()`, `serializeCatalogAndMetadata()` (which serializes the FSM in `serializeMetadata`), then `logCheckpointAndApplyShadowPages()`, then `finalizeCheckpoint()`. So FSM is serialized **before** `finalizeCheckpoint()`. The rollback-added pages are already in `freeLists` (not in `uncheckpointedFreePageRanges`), so they are included in serialization. After reload, the new Database loads from the checkpoint; the FSM deserializer adds all entries to `freeLists`. So in theory the 91 pages should be free. Evidence against: the test fails with 95 used. So either the file size after reload is 95 and the deserialized FSM has 0 free entries (or too few), or `getNumFreeEntries`/`getFreeEntries` omit something (e.g. only `freeLists` are counted, which is correct). Need to confirm whether reload actually loads the checkpoint written by the previous close and whether the data file is truncated as expected.
- **Option D:** The test does not run an explicit checkpoint between ROLLBACK and RELOAD DB. So the only checkpoint is in `~Database()` when we `database.reset()`. That should run; if it is skipped (e.g. exception swallowed) or fails, the new Database would load an older checkpoint. Adding an explicit checkpoint before reload would rule out “no checkpoint” as the cause.

---

## Consensus and recommendation

- **Primary:** **Option A** — Audit all page allocation paths used during COPY (and the rest of the transaction). Ensure every such allocation is made through an allocator that is rolled back (e.g. OptimisticAllocator via LocalStorage). Fix any path that allocates without rollback tracking so that rollback returns those pages to the FSM.
- **Next:** **Option B** — Verify checkpoint-on-close and reload: confirm that the checkpoint run in `~Database()` succeeds, that the FSM is serialized with the expected free list, and that after reload the FSM and file size match (e.g. tail truncation and deserialized free list). Add a temporary assert or test that after rollback the in-memory FSM has the expected free count before any reload.
- **Optional workaround:** If the leak is not fixed quickly, consider **Option C** (skip FSM leak check for these two cases) with a comment and issue reference; remove the skip once the leak is fixed.
- **Reconsider:** If Option A finds no untracked allocations, focus on Option B (checkpoint/reload and truncation/serialization order) and Option D (explicit checkpoint before reload in the test).

---

## Implementation sketch (Option A)

1. **Identify allocation sites:** In `src/processor/operator/persistent/` (node_batch_insert, rel_batch_insert) and any other COPY-related operators, confirm that every `allocatePageRange` (or equivalent) uses the transaction’s `OptimisticAllocator` from `getLocalStorage()->addOptimisticAllocator()` (or a shared allocator that is rolled back).
2. **Search for direct PageManager/FileHandle allocation:** Grep for `allocatePageRange`, `addNewPages`, `getPageManager()->allocatePageRange` outside of OptimisticAllocator and LocalStorage rollback path. Check catalog, WAL, and metadata serialization for allocations during a transaction that might not be rolled back.
3. **Fix:** For any allocation path that runs in a transaction and is not tracked by LocalStorage’s optimistic allocators, either switch it to use an optimistic allocator or register the allocated range so that rollback can call `freeImmediatelyRewritablePageRange` (or equivalent) for it.
4. **Test:** Re-run `CreateNodeAndCopyRelRollbackRecovery` and `CopyNodeAndRelRollbackRecovery`; FSM leak checker should report 4 used pages.

---

## References

- Failing tests: `test/test_files/transaction/copy/copy_rel.test` — cases `CreateNodeAndCopyRelRollbackRecovery`, `CopyNodeAndRelRollbackRecovery`.
- FSM leak checker: `test/test_runner/fsm_leak_checker.cpp` (assert at line 114–116); called from `test/graph_test/private_graph_test.cpp` after `runTest`.
- Rollback path: `src/storage/local_storage/local_storage.cpp` (LocalStorage::rollback), `src/storage/optimistic_allocator.cpp` (OptimisticAllocator::rollback), `src/storage/page_manager.cpp` (freeImmediatelyRewritablePageRange), `src/storage/free_space_manager.cpp` (evictAndAddFreePages, addFreePages).
- COPY allocator: `src/processor/operator/persistent/node_batch_insert.cpp`, `src/processor/operator/persistent/rel_batch_insert.cpp` — use `addOptimisticAllocator()`.
- Checkpoint: `src/storage/checkpointer.cpp` (writeCheckpoint, finalizeCheckpoint after serialize), `src/main/database.cpp` (Database destructor, checkpoint on close).
- RELOAD in test: `test/graph_test/private_graph_test.cpp` — `reloadDBFlag` → `createDB(checkpointWaitTimeout)` (database.reset() then new Database).
