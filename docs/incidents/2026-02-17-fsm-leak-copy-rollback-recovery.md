# Incident: FSM leak after COPY + ROLLBACK + RELOAD DB

**Date:** 2026-02-17  
**Status:** Open; root cause not fully fixed  
**Severity:** Medium (two e2e tests failed)

---

## Summary

After COPY in a transaction, ROLLBACK, and RELOAD DB, the FSM leak checker expected 4 used pages but got 95. Adding evict loop in `mergeFreePages()` was necessary but not sufficient; the leak persists (likely FSM serialization/deserialization or tail truncation).

## Impact

- **Affected tests:** `CreateNodeAndCopyRelRollbackRecovery`, `CopyNodeAndRelRollbackRecovery` (copy_rel.test).
- **Observed:** FSMLeakChecker expected 4 used pages (header + catalog + metadata), got 95 (91 pages not reflected as free after RELOAD).

## Root cause

`LocalStorage::rollback()` calls `PageManager::mergeFreePages()` → `FreeSpaceManager::mergeFreePages()`. That function only called `mergePageRanges()` and did not evict pages from the buffer manager. `finalizeCheckpoint()` correctly calls `evictPages()` before `mergePageRanges()`. Without eviction, pages remained pinned in buffer manager frames; FSM state and file/reload were inconsistent.

## Resolution

- **Evict fix (in code):** In `FreeSpaceManager::mergeFreePages()`, evict loop was added before `mergePageRanges()` to match `finalizeCheckpoint()`. This alone does not fix the leak: after RELOAD, numUsedPages is still 95 instead of 4. Root cause (FSM state across checkpoint/reload or truncation) remains to be fixed.

## References

- `src/storage/free_space_manager.cpp` — mergeFreePages, finalizeCheckpoint, evictPages
- `src/storage/local_storage/local_storage.cpp` — rollback
- Tests: `test/test_files/transaction/copy/copy_rel.test`; FSM leak checker: `test/test_runner/fsm_leak_checker.cpp`
