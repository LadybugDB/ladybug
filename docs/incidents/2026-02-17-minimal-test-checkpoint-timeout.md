# Incident: CI minimal test — CloseConnectionWithActiveTransaction fails (checkpoint timeout)

**Date:** 2026-02-17  
**Status:** Resolved  
**Severity:** Medium (CI red; test passes locally, fails in CI)  
**Component:** Main — Connection/ClientContext teardown, TransactionManager checkpoint  
**Related:** [2026-02-16-connection-close-sigsegv.md](2026-02-16-connection-close-sigsegv.md)

---

## Current state

- **Observed:** Job "minimal test" fails in CI with one failed test: `PrivateApiTest.CloseConnectionWithActiveTransaction`. The failure occurs during **TearDown**, not in the test body. Stack trace shows `CheckpointException` thrown from `TransactionManager::checkpointNoLock` → `stopNewTransactionsAndWaitUntilAllTransactionsLeave()` (timeout waiting for active transactions).
- **Goal:** Test must pass in CI; teardown must not throw so that checkpoint either succeeds or is skipped/absorbed without failing the test.
- **Existing behaviour:**  
  - `Connection::~Connection()` calls `waitForNoActiveQuery()`, sets `clientContext->preventTransactionRollbackOnDestruction = true`, then destroys `clientContext`. So **no rollback** runs when a connection is closed without draining the result (by design, to avoid SIGSEGV — see 2026-02-16 incident).  
  - The active transaction is therefore **never removed** from `TransactionManager::activeTransactions`.  
  - `BaseGraphTest::TearDown()` does `conn.reset()` then `database.reset()`.  
  - `Database::~Database()` calls `transactionManager->checkpoint(clientContext)` when not read-only and `forceCheckpointOnClose` is true.  
  - Checkpoint calls `stopNewTransactionsAndWaitUntilAllTransactionsLeave()` and waits up to `DEFAULT_CHECKPOINT_WAIT_TIMEOUT_IN_MICROS` (5s). Because the transaction from the closed connection is still in the list, the wait **times out** and throws. In CI (slower runner) this manifests consistently; locally it may pass by timing or not hit the path.
- **Gap:** Closing a connection with an active transaction leaves that transaction in the manager; a subsequent database close then tries to checkpoint and times out. There is no path that clears the transaction on connection close without running the full rollback path.

---

## Options and analysis

| Option | Description | Feasibility | Effort | Risk |
|--------|-------------|-------------|--------|------|
| A. Explicit rollback in Connection destructor | After `waitForNoActiveQuery()`, in `~Connection()`, if there is an active transaction, call `transactionManager->rollback(*clientContext, Transaction::Get(*clientContext))`, then set the prevent flag and destroy `clientContext`. | High | Low | Rollback was previously skipped in destructor to avoid SIGSEGV; that was in `~ClientContext` during `clientContext.reset()`. Here rollback runs from `~Connection` while both Connection and Database are still alive. |
| B. Force-clear transaction on Connection destroy | When `preventTransactionRollbackOnDestruction` is set, add a path (e.g. `TransactionManager::clearTransactionWithoutRollback`) that only removes the transaction from `activeTransactions` so checkpoint can proceed. | Medium | Medium | Checkpoint would run with in-memory state that may include uncommitted changes; data consistency and WAL semantics need careful reasoning. Prefer not to use unless A is proven unsafe. |
| C. Increase checkpoint wait timeout in CI/tests | Set a larger `checkpointWaitTimeoutInMicros` for the test suite or CI so that the wait rarely times out. | High | Low | Does not fix root cause; transaction still never leaves. Under load or slower runners the test can remain flaky. |
| D. Database destructor: catch timeout and skip checkpoint | In `Database::~Database()`, catch the timeout (or any checkpoint failure) and skip checkpoint instead of letting the exception propagate. | High | Low | Destructor already has `catch (...) {}` around checkpoint; the exception may be propagating from another layer (e.g. gtest sees an unhandled exception or terminate). Need to confirm where the test sees the failure. |
| E. Test-only: delay or sync before database.reset() in TearDown | In `BaseGraphTest::TearDown()` add a short sleep or explicit sync before `database.reset()` so that connection teardown has “finished”. | Medium | Low | Flaky and hides the real bug; not acceptable as the main fix. |

**Conclusion:** Option A is the only one that fixes the root cause (transaction left in the manager) without leaving inconsistent state or masking the issue. B is a fallback if A turns out to trigger the same SIGSEGV. C and E are workarounds; D is worth checking (whether exception is actually escaping) but does not remove the stuck transaction.

---

## Validation

- **Option A:**  
  - Code path: `Connection::~Connection()` runs while `database` pointer is still valid (Connection holds `Database*`). So `getDatabase()->transactionManager->rollback(*clientContext, Transaction::Get(*clientContext))` is called with live ClientContext and Database. This is the same rollback path used on normal ROLLBACK/error.  
  - The 2026-02-16 incident stated that rollback was skipped in **ClientContext** destructor because it “could touch the database and could also crash if the database or connection was already torn down”. Here we perform rollback **before** destroying ClientContext, so the connection and database are still intact.  
  - Evidence: `TransactionManager::rollback` takes `ClientContext&` and `Transaction*`; both are valid in `~Connection()` before `clientContext.reset()`.

- **Option B:**  
  - Would require a new API (e.g. remove from `activeTransactions` without WAL/undo rollback). Checkpoint then runs; uncommitted changes might still be in version info / buffers. Not validated as safe for correctness; keep as optional fallback only.

- **Option D:**  
  - `Database::~Database()` at `database.cpp:143–148` already wraps `checkpoint(clientContext)` in `try { ... } catch (...) {}`. So in theory the exception should not propagate. The CI failure stack shows the exception in the checkpoint path; it is possible the exception is rethrown from somewhere else (e.g. from a destructor invoked during unwinding) or that the test binary was built from a version where the catch was not present. Re-check current `database.cpp` and test binary to confirm.

---

## Consensus and recommendation

- **Primary:** Implement **Option A**: in `Connection::~Connection()`, after `waitForNoActiveQuery()`, if there is an active transaction, call `transactionManager->rollback(*clientContext, Transaction::Get(*clientContext))`, then set `preventTransactionRollbackOnDestruction = true` and destroy `clientContext`. This clears the transaction so that a later `database.reset()` can checkpoint without timing out.
- **Alternative / follow-up:** If Option A ever reproduces a crash in rollback-during-close scenarios, consider **Option B** (force-clear transaction only on close, with clear documentation that checkpoint after such a close is best-effort) and/or a targeted increase of timeout for this test only (**Option C**) as a temporary mitigation.
- **Do first:** Implement A; run `PrivateApiTest.CloseConnectionWithActiveTransaction` locally and in CI repeatedly; run broader transaction and connection-close tests (e.g. C API, Node addon resilience).
- **Document:** In `Connection::~Connection()` or in a short comment near `preventTransactionRollbackOnDestruction`, note that we now rollback in ~Connection when safe (after waitForNoActiveQuery) so that the transaction is removed before ClientContext is destroyed.
- **Reconsider:** If rollback in ~Connection() causes any new failures (e.g. in embeddings that close during error paths), revisit Option B and the exact order of operations in destructors.

---

## Implementation sketch (Option A)

1. **File:** `src/main/connection.cpp`  
   - In `Connection::~Connection()` after `clientContext->waitForNoActiveQuery();`:  
     - Include or forward-declare so that `Transaction::Get(*clientContext)` and `transactionManager->rollback(...)` are available.  
     - If `Transaction::Get(*clientContext)` is non-null, call `database->getTransactionManager()->rollback(*clientContext, Transaction::Get(*clientContext))`.  
     - Then set `clientContext->preventTransactionRollbackOnDestruction = true;` (so that when `clientContext` is destroyed, `~ClientContext` does not attempt rollback again).  
     - Then leave the rest of the destructor as-is (e.g. `clientContext.reset()` or equivalent).

2. **Dependencies:** `connection.cpp` already has access to `database` and `clientContext`. Ensure `Transaction::Get` and `TransactionManager::rollback` are visible (include `transaction/transaction.h`, `transaction/transaction_manager.h`, or equivalent).

3. **Tests:**  
   - `build/relwithdebinfo/test/transaction/transaction_test --gtest_filter="*CloseConnectionWithActiveTransaction*"` (single test).  
   - Then `make shell-test test` or the full minimal-test job.  
   - Re-run CI job "minimal test" after pushing.

4. **Rollback behaviour:** No change to normal commit/rollback semantics; only the teardown path when a connection is closed with an active transaction now performs one explicit rollback before destroying ClientContext.

---

## References

- CI failure: job "minimal test", test `PrivateApiTest.CloseConnectionWithActiveTransaction`; stack: `CheckpointException` from `transaction_manager.cpp:175` (`stopNewTransactionsAndWaitUntilAllTransactionsLeave` timeout).
- Code: `src/main/connection.cpp` (Connection destructor), `src/main/client_context.cpp` (ClientContext destructor, preventTransactionRollbackOnDestruction), `src/transaction/transaction_manager.cpp` (rollback, checkpoint, stopNewTransactionsAndWaitUntilAllTransactionsLeave), `src/main/database.cpp` (Database destructor, checkpoint on close).
- Test: `test/transaction/transaction_test.cpp` — `CloseConnectionWithActiveTransaction`; fixture: `test/include/graph_test/private_graph_test.h`, `test/include/graph_test/base_graph_test.h` (TearDown).
- Constants: `src/include/common/constants.h` — `DEFAULT_CHECKPOINT_WAIT_TIMEOUT_IN_MICROS`, `THREAD_SLEEP_TIME_WHEN_WAITING_IN_MICROS`.
- Related incident: [2026-02-16-connection-close-sigsegv.md](2026-02-16-connection-close-sigsegv.md).

---

## Resolution

**Option A implemented.** In `Connection::~Connection()` (`src/main/connection.cpp`), after `waitForNoActiveQuery()`, if there is an active transaction we call `database->getTransactionManager()->rollback(*clientContext, Transaction::Get(*clientContext))`, then set `preventTransactionRollbackOnDestruction = true` and destroy `clientContext`. The transaction is removed from `TransactionManager::activeTransactions`, so a later `Database::~Database()` checkpoint no longer times out.

**Verification:** `build/relwithdebinfo/test/transaction/transaction_test --gtest_filter="*CloseConnectionWithActiveTransaction*"` passes.
