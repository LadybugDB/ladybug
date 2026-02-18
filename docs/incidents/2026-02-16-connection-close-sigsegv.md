# Incident: SIGSEGV when Connection is destroyed while query workers are still running

**Date:** 2026-02-16
**Status:** Resolved
**Severity:** High (crash; observed in Node.js addon and during COPY)
**Component:** Main — Connection / ClientContext lifecycle, Processor task scheduler
**Author:** [@vkozio](https://github.com/vkozio)

---

## Summary

Closing a Connection (or Database) while a query was still executing (or while worker threads were cleaning up after an exception) could lead to **SIGSEGV (signal 11)**. The destructor of `Connection` destroyed `ClientContext` immediately; worker threads in `TaskScheduler` could still hold references to that context (e.g. in `ProcessorTask::finalize()` or when handling exceptions). Those workers then touched already-freed memory. A related symptom was **TransactionManagerException** ("Only one write transaction at a time") when a second write was attempted while the first was still in progress; if the application closed the connection in response, the ensuing teardown could trigger the crash.

---

## Impact

- **Affected:** Any embedding that closes a Connection (or Database) while a query is in flight or immediately after an error — notably the **Node.js addon** (e.g. `connection.close()` or `db.close()` while a query or result iteration is still running).
- **Observed:** SIGSEGV during or shortly after Connection/DB close; in one scenario, TransactionManagerException during COPY followed by Fatal signal 11.
- **User impact:** Process crash; data loss only if the process was in the middle of a write (the DB itself is not corrupted by this bug).

---

## Root cause

### Lifecycle before the fix

1. **Connection** owns a `std::unique_ptr<ClientContext> clientContext`.
2. **Query execution:** `ClientContext::query()` → `QueryProcessor::execute()` → `TaskScheduler::scheduleTaskAndWaitOrError(task, context)`. The `ExecutionContext` passed to the task holds a raw pointer `clientContext`. Worker threads run `ProcessorTask::run()` and, on exit or exception, may call `ProcessorTask::finalize()`, which can access `executionContext->clientContext`.
3. **Connection destructor** (before fix): Set `clientContext->preventTransactionRollbackOnDestruction = true` and then destroyed `clientContext` (via `unique_ptr` reset). There was **no wait** for in-flight queries to finish.
4. If the application closed the Connection while a query was still running (or while a worker was in `finalize()` after an exception), a worker could dereference `clientContext` after it had been destroyed → use-after-free → SIGSEGV.

### Why TransactionManagerException could precede SIGSEGV

With a single write transaction allowed, a second write (e.g. concurrent COPY) throws TransactionManagerException. The application or test might then close the connection. That close ran the destructor while the first write’s workers were still shutting down or finalizing, leading to the same use-after-free and SIGSEGV.

### Existing partial mitigation

`Connection::~Connection()` already set `preventTransactionRollbackOnDestruction = true` so that `ClientContext`’s destructor does not attempt a transaction rollback (which would touch the database and could also crash if the database or connection was already torn down). That avoided one class of crash but did not address workers still holding references to `ClientContext`.

---

## Resolution

### Change: wait for no active query before destroying ClientContext

1. **ClientContext** (`src/include/main/client_context.h`, `src/main/client_context.cpp`):
   - Added `std::atomic<uint32_t> activeQueryCount`, `std::mutex mtxForClose`, `std::condition_variable cvForClose`.
   - Added `registerQueryStart()`, `registerQueryEnd()`, `waitForNoActiveQuery()`.
   - `registerQueryEnd()` decrements the count and notifies the condition variable when it reaches zero.

2. **QueryProcessor::execute()** (`src/processor/processor.cpp`):
   - At entry: `context->clientContext->registerQueryStart()`.
   - RAII guard (stack object) calls `registerQueryEnd()` in its destructor so it is always called on normal return or on exception.
   - Thus every path that runs `scheduleTaskAndWaitOrError` is bracketed by start/end.

3. **Connection::~Connection()** (`src/main/connection.cpp`):
   - Before setting `preventTransactionRollbackOnDestruction` and before `clientContext` is destroyed, call `clientContext->waitForNoActiveQuery()`.
   - The destructor therefore blocks until all queries that had been started on this connection have completed (and their workers have finished), so no worker can touch `ClientContext` after it is destroyed.

### Files changed

- `src/include/main/client_context.h` — declarations and members.
- `src/main/client_context.cpp` — implementations of register/wait.
- `src/main/connection.cpp` — `waitForNoActiveQuery()` in destructor.
- `src/processor/processor.cpp` — include `client_context.h`, start/end guard in `execute()`.

---

## Verification

- **C API:** `build/relwithdebinfo/test/c_api/c_api_test --gtest_filter="*Connection*"` — all 11 tests passed (CreationAndDestroy, Query, etc.).
- **Node addon:** `make nodejstest` — 163 passed, 0 failed; suite "Resilience (close during/after use)" passed (close while iterating, hasNext/getNext after DB closed, etc.).
- **Copy tests:** `build/relwithdebinfo/test/copy/copy_tests` — 19 tests passed.

---

## Lessons learned

1. **Ownership and worker lifetime:** If worker threads hold raw pointers to an object, the owner must ensure those workers have finished (or have been told to stop) before destroying the object. A simple ref-count or “active operation” count plus condition variable is one way to do that.
2. **Destructors and async work:** Connection/ClientContext teardown is a critical path; document and enforce the rule that no query may be in flight when the destructor runs. The RAII guard in `execute()` keeps the count accurate even when exceptions occur.
3. **Embeddings (Node, etc.):** Applications may close connections on error or on user action without draining results. The core must tolerate “close while busy” by waiting for in-flight work instead of destroying state immediately.

---

## References

- Fix commit: Connection wait for in-flight queries before destroying ClientContext (activeQueryCount, registerQueryStart/End, waitForNoActiveQuery).
- Code: `src/include/main/client_context.h`, `src/main/client_context.cpp`, `src/main/connection.cpp`, `src/processor/processor.cpp`.
- Related: `preventTransactionRollbackOnDestruction` in `Connection::~Connection()` and `ClientContext` destructor (rollback skipped on teardown).
- Node addon tests: `tools/nodejs_api/test/test_resilience.js`, `test/test_connection.js`.
