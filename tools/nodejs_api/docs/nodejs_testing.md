# Node.js API — Testing Guide

Guidelines for writing and reviewing tests for the Node.js API (`tools/nodejs_api/`). Use this when adding or changing tests to keep the suite correct, isolated, and maintainable.

---

## 1. Assertions and Oracles

- **Strict equality:** The test shim’s `assert.equal(a, b)` is `strictEqual`. A number and a string (e.g. `1234` vs `'1234'`) are not equal. When the API or DB returns strings (e.g. `current_setting()`), coerce before comparing: `Number(tuple["checkpoint_threshold"]) === 1234`, or compare to the expected string.
- **Floating point:** Use `assert.approximately(actual, expected, delta)` for FLOAT/DOUBLE. Choose a small delta (e.g. `1e-6`). NaN is not considered approximately equal to anything; that is intentional.
- **Arrays in assertions:** In JavaScript, `(a, b)` is the comma operator and evaluates to `b`. Never write `assert.deepEqual(x, [(10, 8)])` — that compares `x` to `[8]`. Use `assert.deepEqual(x, [10, 8])`.
- **API return types:** If the API can return either a value or a list (e.g. single value vs array), assert the actual shape (e.g. `assert.deepEqual(result["usedNames"], ["Aida"])` if the API returns an array).
- **Naming:** Use clear variable names (e.g. `expectedResultArr` instead of typo-prone names) so assertions stay readable.

---

## 2. Test Isolation and Shared State

- **Assert the right object:** If the test creates its own database or connection (e.g. `testDb`, `testConn`), run config or data checks **against that instance**, not the global `db`/`conn`. Using global `conn` in a test that built `testDb` checks the wrong database and is a logic bug.
- **Prefer a dedicated connection for local DBs:** When testing options of a newly created database, create a connection to that database, run the query, then close both the connection and the database.
- **Close what you open:** If a test creates a connection or database, close it in the same test (or in a reliable `finally`/hook). Leaving connections or databases open can leak handles and affect other tests or the process.
- **Shared fixtures:** Tests that use the global `db`/`conn` from `before()` are fine for read-only or shared-scenario tests; just don’t use them to verify state of a different, locally created DB/conn.

---

## 3. Data Types and Boundaries

- **Settings as strings:** `current_setting()` returns strings (e.g. `'1234'`, `'False'`). For numeric checks use `Number(...)`; for booleans compare to the string the backend returns (e.g. `"False"`).
- **Large integers:** Values above `Number.MAX_SAFE_INTEGER` (2^53) can lose precision in JavaScript. For UINT64/INT64 round-trip tests with very large values, a short comment is helpful (e.g. that values > 2^53 may be lossy in JS).
- **Column names:** Tests that depend on exact column names (e.g. from `RETURN CAST($1, 'UINT64')`) will break if the backend changes display names. Prefer stable API contracts when possible; otherwise document the dependency.

---

## 4. Concurrency and Timing

- **Time-based races:** Tests that close a connection or DB after a short delay (e.g. 80 ms) then assert “query rejects” can be flaky on slow CI. Use a timeout (e.g. 2 s) so the test fails fast if the query never rejects, and consider slightly longer delays on CI if needed.
- **Node.js test runner timeout:** For long-running tests (e.g. interrupt), set timeout via the test option: `it("...", { timeout: 5000 }, async function () { ... })`. The test context in `node:test` does not provide `this.timeout()`.
- **Concurrent queries:** When running multiple queries in parallel on the same connection, assert results against known stable data (e.g. fixed IDs) and avoid shared mutable state.

---

## 5. Error Messages and API Contracts

- **Exact vs partial match:** `assert.equal(e.message, "exact string")` is brittle if the backend changes wording. For stability, prefer `assert.include(e.message, keyPhrase)` or similar when the exact text is not part of the public API contract.
- **Resilience tests:** Checking that the error message contains “closed” or “not allowed” is a good balance between stability and coverage.

---

## 6. Resource Lifecycle and Cleanup

- **Databases and connections:** Every database or connection created in a test should be closed in that test (or in a `finally`/hook that always runs). This includes “positive” tests (e.g. “should create a database with valid path and no buffer size”).
- **Query results:** Prefer calling `res.close()` when a test opens many results (e.g. concurrency) or when the test is long-lived. Relying on GC alone can hide leaks.
- **Temp directories:** Use a helper (e.g. `withTempDb`) that creates a temp DB/conn, runs the test, and in `finally` closes them and removes the temp path. Avoid leaving temp dirs or DBs open.
- **process.exit(0):** If the test runner uses `process.exit(0)` in `after()` to avoid the event loop hanging (e.g. due to the native addon), document it; it can mask unclosed resources, so use only when necessary.

---

## 7. Validation Checklist

Before submitting test changes:

- [ ] Run the full suite: `npm test` (from `tools/nodejs_api/`).
- [ ] If you test against an installed package, run with `TEST_INSTALLED=1` as applicable.
- [ ] For tests that create a local DB or connection, ensure config/data assertions use that instance, not the global `db`/`conn`.
- [ ] Ensure no comma-operator traps in assertions: no `(a, b)` used as an array element in `deepEqual`/`equal`.
- [ ] All resources (DB, connection) created in the test are closed in the same test or a guaranteed cleanup path.
