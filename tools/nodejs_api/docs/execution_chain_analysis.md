# LOAD FROM stream: Execution Chain (reference & recommendations)

## Execution chain diagram

```mermaid
%%{init: {'flowchart': {'defaultRenderer': 'elk', 'elk': {'direction': 'DOWN'}}}}%%
flowchart TB
  subgraph JS["JS (main thread)"]
    direction TB
    A["query('LOAD FROM name RETURN *')"]
    B["registerStream: getChunk(requestId) → pending.push; runConsumer via setImmediate"]
    C["runConsumer: sort pending, for each id take it.next(), returnChunk(id, rows, done)"]
    D["AsyncIterator: it.next() → yield rows"]
    A --> B
    B --> C
    C --> D
  end

  subgraph CppAddon["C++ addon (Node worker thread)"]
    direction TB
    E["tableFunc: mutex, nextRequestId(), setChunkRequest, BlockingCall(getChunk)"]
    F["wait reqPtr->cv until filled"]
    G["returnChunkFromJS: req->rows, filled=true, cv.notify_one"]
    H["Copy rows to output.dataChunk, return cap"]
    E --> F
    G --> F
    F --> H
  end

  subgraph Engine["Engine (single task thread, canParallelFunc=false)"]
    direction TB
    I["getNextTuple → getNextTuplesInternal"]
    J["tableFunc(input, output) → numTuplesScanned"]
    K["FactorizedTable accumulates chunks"]
    L["MaterializedQueryResult + FactorizedTableIterator"]
    I --> J
    J --> K
    K --> L
  end

  J --> E
  E --> B
  C --> G
  H --> J
  L --> M["JS hasNext / getNext"]
  M --> A
```

---

## Useful observations

- **Order**: With `canParallelFunc = false`, one engine thread calls `tableFunc` sequentially. Request IDs are assigned under mutex; JS `runConsumer` sorts `pending` and serves chunks by `requestId`, so iterator order is preserved.
- **End of stream**: Engine calls `tableFunc` until it returns 0. JS sends `returnChunk(id, [], true)` when the iterator is done; C++ returns 0 and the engine stops. No extra call after 0.
- **getNext contract**: Core `getNext()` throws if `!hasNext()`. Addon always checks `hasNext()` before `getNext()` and returns `null` when exhausted so that JS API matches `getNext(): Promise<Record | null>`.

---

## Recommendations for the future

1. **Keep `canParallelFunc = false`** for the node stream table function. Enabling parallelism would require a deterministic merge of chunks by requestId on the engine side; until then, single-thread keeps order and avoids subtle bugs.
2. **Any new code path that reads rows** (e.g. another language binding or helper) must guard with `hasNext()` before `getNext()`; core will throw otherwise.
3. **Mutex in `tableFunc`**: Currently redundant with single-thread execution but harmless. If parallelism is ever introduced, either remove the mutex and solve ordering in the engine or keep it and document that the stream source is intentionally serialized.
4. **Tests**: Prefer iterating with `hasNext()` + `getNext()` and asserting `getNext()` returns `null` exactly when `hasNext()` becomes false, to lock the contract (see `test_query_result.js`).
5. **Rebuild and full test run** (e.g. `register_stream` + `query_result`) after any change in the addon or engine table function path.
