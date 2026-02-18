# Incident: CopyRelSegmentTest failure — "Try to partition multiple factorization group"

**Date:** 2026-02-16
**Status:** Resolved
**Severity:** Medium (E2E test failure; no production impact)
**Component:** Planner — COPY FROM for relationship tables
**Author:** [@vkozio](https://github.com/vkozio)

---

## Summary

The E2E test `copy~segmentation.CopyRelSegmentTest` failed with a runtime exception during planning:

```text
Runtime exception: Try to partition multiple factorization group. This should not happen.
```

The failure occurred in `LogicalPartitioner::computeFactorizedSchema()` when planning `COPY <rel_table> FROM (subquery)` for a query source. The root cause was an inconsistent use of schema group checks in `planCopyRelFrom`: the code used `getGroupsPosInScope().size() == 1` to decide whether to add an Accumulate operator, while `LogicalPartitioner` later enforces a single group with `getNumGroups() != 1`. In cases where the plan had more than one factorization group but only one group had expressions in scope, no Accumulate was added and the Partitioner received a multi-group schema and threw.

---

## Impact

- **Affected:** E2E test suite; any use of `COPY <rel_table> FROM (subquery)` when the subquery plan had multiple factorization groups and only one group in scope.
- **Observed:** Test `copy~segmentation.CopyRelSegmentTest` (e.g. Test #2336) failed; subsequent STORAGE_INFO checks in the same test also failed because the first COPY never ran successfully.
- **User impact:** None reported; failure was caught by tests.

---

## Root cause

### Schema APIs involved

In `src/include/planner/operator/schema.h`:

- **`getNumGroups()`** (public) returns `groups.size()` — the total number of factorization groups in the schema.
- **`getGroupsPosInScope()`** returns the set of group positions that have at least one expression in the current scope (projected columns).

So the schema can have `getNumGroups() > 1` while `getGroupsPosInScope().size() == 1` if only one of the groups has expressions in scope.

### Logic before the fix

In `src/planner/plan/plan_copy.cpp`, `planCopyRelFrom()` for `ScanSourceType::QUERY`:

1. Builds a plan with `planQuery(...)`.
2. Decides whether to add an Accumulate to “flatten” to a single data chunk:
   - **Condition used:** `if (schema->getGroupsPosInScope().size() == 1) break;`
   - So Accumulate was **skipped** when exactly one group had expressions in scope, even if the schema had multiple groups.
3. Then appends IndexScan (primary key lookup) and Partitioner.

`LogicalPartitioner::computeFactorizedSchema()` (in `src/planner/operator/logical_partitioner.cpp`) does:

- `copyChildSchema(0)` then `validateSingleGroup(*schema)`.
- `validateSingleGroup()` throws if `schema.getNumGroups() != 1`.

So the Partitioner requires a **single group** (`getNumGroups() == 1`), not “single group in scope”.

### Failure scenario

1. For some COPY rel-from-query plans, the query plan had **two** factorization groups but only **one** group had expressions in scope.
2. `getGroupsPosInScope().size() == 1` → Accumulate was not added.
3. IndexScan copied the child schema (still two groups) and added its expressions into one group; the schema still had two groups.
4. Partitioner copied that schema and called `validateSingleGroup()` → `getNumGroups() != 1` → exception.

### Inconsistency with node COPY

`planCopyNodeFrom()` already used the stricter check:

```cpp
if (plan.getSchema()->getNumGroups() > 1) {
    appendAccumulate(...);
}
```

So node COPY and rel COPY were using different criteria for when to accumulate, and only the node path was aligned with the downstream single-group assumption.

---

## Timeline

| When        | What |
|------------|------|
| Test run   | `copy~segmentation.CopyRelSegmentTest` failed on first `COPY edges from (unwind range(...) return num, num+1)` with "Try to partition multiple factorization group". |
| Diagnosis  | Stack trace pointed to `validateSingleGroup` in `logical_partitioner.cpp`; comparison with `planCopyNodeFrom` and Schema API usage revealed the wrong condition in `planCopyRelFrom`. |
| Fix        | Condition in `planCopyRelFrom` changed from `getGroupsPosInScope().size() == 1` to `getNumGroups() <= 1`, and Accumulate comment kept in sync with node path. |

---

## Resolution

**Change (single file):** `src/planner/plan/plan_copy.cpp`

- **Before:**
  `if (schema->getGroupsPosInScope().size() == 1) break;`
- **After:**
  `if (schema->getNumGroups() <= 1) break;`

So we only skip adding Accumulate when the query plan has at most one factorization group. When it has more than one, we always add Accumulate so that the plan reaching the Partitioner has a single group, satisfying `validateSingleGroup()`.

**Verification:** Re-run the failing test (e.g. with RelWithDebInfo build):

```bash
E2E_TEST_FILES_DIRECTORY=test/test_files build/relwithdebinfo/test/runner/e2e_test --gtest_filter="copy~segmentation.CopyRelSegmentTest"
```

---

## Lessons learned

1. **Use a single, clear contract for “single group”:** Downstream operators (here, Partitioner) that require one group should be matched by one consistent check (e.g. `getNumGroups() <= 1`) at the planning site. Mixing “groups in scope” and “number of groups” led to a mismatch.
2. **Keep similar code paths aligned:** Node and rel COPY-from-query had the same goal (one chunk for Copy operator) but used different schema checks; aligning rel COPY with node COPY (and with Partitioner) would have avoided the bug.
3. **Tests are the canary:** The E2E test exposed a planner path that only fails when the query plan has multiple groups with only one in scope; unit tests that only use single-group plans would not have caught this.

---

## Action items

| Item | Owner | Status |
|------|--------|--------|
| Fix applied and documented | — | Done |
| Consider adding a planner unit test that builds a COPY rel-from-query with a multi-group subquery and asserts no exception and single group before Partitioner | Optional | Open |
| Consider a short comment in `plan_copy.cpp` next to the condition that the Partitioner requires exactly one group (reference to `validateSingleGroup`) | Optional | Open |

---

## References

- Fix: `src/planner/plan/plan_copy.cpp` (condition in `planCopyRelFrom` for `ScanSourceType::QUERY`).
- Failing test: `test/test_files/copy/segmentation.test` — case `CopyRelSegmentTest`.
- Downstream check: `src/planner/operator/logical_partitioner.cpp` — `validateSingleGroup`.
- Schema API: `src/include/planner/operator/schema.h` — `getNumGroups()`, `getGroupsPosInScope()`.
