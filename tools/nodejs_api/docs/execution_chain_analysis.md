# LOAD FROM stream: Execution Chain Analysis

## 1. End-to-end execution chain

```mermaid
flowchart TB
  subgraph JS["JS (main thread)"]
    A[conn.query("LOAD FROM name RETURN *")]
    B[registerStream: getChunk(requestId) → pending.push; runConsumer via setImmediate]
    C[runConsumer: sort pending, for each requestId take chunks[index], returnChunk(requestId, rows, done)]
    D[AsyncIterator: it.next() → yield rows]
  end

  subgraph CppAddon["C++ addon (Node worker)"]
    E[tableFunc: mutex, nextRequestId(), setChunkRequest, BlockingCall getChunk]
    F[wait reqPtr->cv until filled]
    G[returnChunkFromJS: req->rows, req->filled=true, cv.notify_one]
    H[Copy rows to output.dataChunk, return cap]
  end

  subgraph Engine["Engine (task thread, single if canParallelFunc=false)"]
    I[getNextTuple → getNextTuplesInternal]
    J[tableFunc(input, output) → numTuplesScanned]
    K[FactorizedTable accumulates chunks]
    L[Result: MaterializedQueryResult + FactorizedTableIterator]
  end

  A --> I
  I --> J
  J --> E
  E --> B
  B --> C
  C --> D
  C --> G
  G --> F
  F --> H
  H --> J
  J --> K
  K --> L
  L --> M[hasNext / getNext in JS]
  M --> A
```

### Sequence (single-threaded pipeline)

| Step | Where | What |
|------|--------|------|
| 1 | JS | `query("LOAD FROM mystream RETURN *")` |
| 2 | Engine | Parse, plan TableFunctionCall (node stream), execute task |
| 3 | Engine | Source: `getNextTuple()` → `getNextTuplesInternal()` → `tableFunc()` |
| 4 | C++ | `tableFunc`: lock mutex, requestId=1, setChunkRequest(1), BlockingCall(getChunk, 1) |
| 5 | JS | getChunk(1) called on main thread; pending=[1], runConsumer scheduled |
| 6 | C++ | BlockingCall returns only after JS calls returnChunk; C++ waits on req->cv |
| 7 | JS | runConsumer: requestId=1, index=0, it.next() → { value: [1,"a"], done: false }, returnChunk(1, [[1,"a"]], false) |
| 8 | C++ | returnChunkFromJS: req->rows, filled=true, cv.notify_one; tableFunc wakes, copies 1 row, returns 1 |
| 9 | Engine | TableFunctionCall outputs chunk to ResultSet; getNextTuple called again |
| 10 | C++ | tableFunc again: requestId=2, BlockingCall(getChunk, 2), wait |
| 11 | JS | getChunk(2), pending=[2], runConsumer: it.next() → [2,"b"], returnChunk(2, ...) |
| … | … | Repeat until tableFunc returns 0 (empty chunk / done) |
| N | Engine | No more tuples from source → pipeline finishes, MaterializedQueryResult built |
| N+1 | JS | result.hasNext() / result.getNext() over FactorizedTableIterator |
| N+2 | C++ addon | GetNextAsync: if !hasNext() return null; else getNext() (core throws if !hasNext()) |
| N+3 | JS | getNext() resolves to row or null |
```

---

## 2. Expert views and failure points

### Expert 1: Engine / pipeline

- **Order**: With `canParallelFunc = false`, one thread runs the pipeline; tableFunc is called sequentially (1, 2, 3, …). Chunks are appended to FactorizedTable in call order → row order is preserved.
- **End of stream**: Engine keeps calling tableFunc until it returns 0. JS sends `returnChunk(id, [], true)` when iterator is done; C++ returns 0 → engine stops. No extra tableFunc call after “done”.
- **Risk**: If the engine ever called tableFunc again after a 0 return, the next requestId would be issued and JS would call it.next() again (possibly past end). Current code path: return 0 → getNextTuplesInternal returns false → parent stops pulling; no evidence of extra call.

### Expert 2: Addon (C++ / JS bridge)

- **requestId ordering**: C++ assigns requestId sequentially under mutex; JS runConsumer sorts `pending` and serves `chunks[requestId - 1]`. So request 1 gets first it.next(), request 2 gets second, etc. Correct.
- **getNext() contract**: Core MaterializedQueryResult::getNext() throws if !hasNext(). Addon GetNextSync checks hasNext() and returns env.Null() without calling getNext(). GetNextAsync previously called getNext() even when !hasNext() → throw. Fix: in Execute(), if !hasNext() set cppTuple=null and return; in OnOK(), pass env.Null() as value when cppTuple is null.
- **Risk**: Any other code path that calls getNext() without checking hasNext() will still throw (by design in core); addon must always guard.

### Expert 3: API / tests

- **Contract**: d.ts says `getNext(): Promise<Record<string, LbugValue> | null>`. So “no more rows” must be expressed as resolving with `null`, not throwing.
- **Tests**: register_stream expects getNext() after last row to return null; test_query_result was updated to expect null when exhausted instead of throw.
- **Risk**: Inconsistent use of hasNext() before getNext() in other tests or user code can still hit the core throw if the addon path is bypassed or a different API is used.

---

## 3. Hypotheses (why “No more tuples” or wrong order happened)

| # | Hypothesis | Likelihood | Evidence |
|---|------------|------------|----------|
| H1 | Multiple threads called tableFunc; results merged by completion order, so first row could be id=3; then one thread called getNext() after exhaustion | High (before fix) | canParallelFunc default true; TaskScheduler runs task with N threads; each copy of operator calls getNextTuplesInternal. |
| H2 | GetNextAsync did not check hasNext() before getNext(); when test called getNext() the 4th time, C++ threw “No more tuples” | High | Execute() had “if (!hasNext()) cppTuple.reset();” then unconditionally “cppTuple = getNext();” → always threw when exhausted. |
| H3 | JS runConsumer delivered chunks out of order (e.g. request 2 before 1) | Low | pending is sorted by requestId; chunks[index] with index = requestId - 1; iterator consumed in order. |
| H4 | Engine calls tableFunc one extra time after 0 return | Low | No code path found that would call tableFunc again after return 0. |
| H5 | Empty chunk not handled (e.g. returnChunk(id, [], false) and engine expects more) | Low | C++ returns 0 for numRows==0; engine treats 0 as “no tuples this call”, continues until next tableFunc returns 0; JS can send [] and done=false for “no data this chunk” and later send done=true. |

---

## 4. Brainstorm: design options

| Option | Description | Pros | Cons |
|--------|-------------|------|------|
| A. Single-thread table func (current) | canParallelFunc = false for node stream | Simple, deterministic order, no cross-thread ordering bugs | Slightly less engine parallelism for this source only |
| B. Return null when exhausted (current) | Addon GetNextAsync: if !hasNext() return null, do not call getNext() | Matches d.ts and user expectation; tests can rely on getNext() → null | Core still throws if getNext() called without hasNext(); only addon is defensive |
| C. Relax tests: order-independent | Tests collect all rows, sort by id, assert set | Resilient to any engine reorder | Hides real ordering bugs; less strict |
| D. Keep mutex in tableFunc | Already in place | Serializes tableFunc calls even if canParallelFunc were true | Redundant with A; adds lock contention if we ever parallelize |
| E. Core API: getNext() returns null | Change MaterializedQueryResult::getNext() to return null instead of throw when !hasNext() | Single contract everywhere | Breaking change for C++ API users who rely on throw |
| F. Document “always use hasNext() before getNext()” | Keep throw in core, document in JS | Clear contract | Poor DX; register_stream tests expect null |

**Best combination (current state):**

- **A + B**: Single-thread table function + addon returns null when exhausted. No test relaxation (C). Mutex (D) is optional extra safety; can keep. No core API change (E) to avoid breaking C++ users.

---

## 5. Checklist (what was fixed / what to verify)

- [x] **canParallelFunc = false** for NodeStreamScanFunction → single-thread pipeline, order preserved.
- [x] **GetNextAsync**: if !hasNext(), do not call getNext(); pass env.Null() to callback so JS gets null.
- [x] **test_query_result**: expect null when exhausted; test name updated.
- [ ] **Rebuild addon** and run full test suite (register_stream + query_result) to confirm no regressions.
- [x] **Optional**: Add a single test that iterates with hasNext() and asserts getNext() returns null exactly when hasNext() becomes false, to lock the contract (test_query_result.js: "getNext() returns null exactly when hasNext() is false").

---

## 6. One-page summary

**Chain:** JS `query("LOAD FROM name")` → Engine plans TableFunctionCall → one task thread (canParallelFunc=false) calls tableFunc repeatedly → C++ tableFunc uses mutex, nextRequestId(), BlockingCall(getChunk), waits on cv → JS getChunk pushes requestId, runConsumer feeds iterator in request order and returnChunk → C++ wakes, copies rows, returns count → Engine accumulates into FactorizedTable → MaterializedQueryResult → JS hasNext/getNext.

**Root causes of past failures:** (1) Parallel table function → chunks completed out of order → first row could be 3 instead of 1. (2) GetNextAsync called getNext() even when !hasNext() → core threw “No more tuples”.

**Fixes applied:** (1) canParallelFunc = false. (2) GetNextAsync: if !hasNext() return null and skip getNext(); callback second arg = env.Null(). (3) Test expects null when exhausted.

**Recommended:** Keep current design (A+B). Verify with full rebuild and tests.
