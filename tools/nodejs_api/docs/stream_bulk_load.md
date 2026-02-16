# Stream bulk load (registerStream / LOAD FROM)

## Current state

- **API**: `conn.registerStream(name, asyncIterable, { columns })` registers a stream for `LOAD FROM name RETURN ...`. Source must be AsyncIterable; each yielded value is one or more rows (array of column values in schema order, or object keyed by column name). `unregisterStream(name)` removes it.
- **Flow**: When the plan scans the stream, the table function `NODE_STREAM_SCAN` runs. Each call:
  1. Creates a chunk request and invokes the JS callback (ThreadSafeFunction) with a `requestId`.
  2. JS consumer runs: one `it.next()` from the async iterable; `toRows(value)` normalizes to an array of rows; `conn.returnChunk(requestId, rows, done)`.
  3. C++ blocks until the request is filled, then copies **at most `DEFAULT_VECTOR_CAPACITY`** rows (e.g. 2048, from `system_config.h`) into the output `DataChunk` and returns that count.
- **Limitation**: Any rows returned by JS beyond the first `DEFAULT_VECTOR_CAPACITY` in a single chunk are **discarded**. There is no local state to buffer the remainder for the next call. So if the user yields 10 000 rows at once, only 2048 are used and 7952 are lost. Correct usage today is to yield batches of at most 2048 rows per `next()`; the engine then requests more chunks until the stream is done.
- **Other details**: Single-threaded (`canParallelFunc = false`). One global mutex around the getChunk call. Backpressure is implicit (engine blocks until JS fills the chunk).

## Goal

Improve correctness and efficiency of stream bulk load: no data loss, fewer round-trips, and clearer contract for batch size.

---

## Options and analysis

| Option | Description | Feasibility | Effort | Risk |
|--------|-------------|-------------|--------|------|
| **A. Buffer remainder in C++** | Keep leftover rows (when JS returns more than `DEFAULT_VECTOR_CAPACITY` in one chunk) in table-function local state; serve them on the next call before requesting a new chunk from JS. | High. Table functions support local state; copy pattern is already in place. | Low. One local state struct, drain remainder before calling getChunk again. | Low. Must not request a new chunk while remainder is non-empty. |
| **B. Batch size hint to JS** | When requesting a chunk, pass a desired row count (e.g. `DEFAULT_VECTOR_CAPACITY`) so the JS consumer can batch up to that many rows from the iterable before calling `returnChunk`. | High. getChunk callback already receives `requestId`; adding a second argument (maxRows) is a small API change. | Low. C++ passes hint; JS side batches in a loop until hint or done. | Low. Backward compat: old JS can ignore the hint. |
| **C. Accept Arrow/columnar input** | Allow registering a stream that yields Arrow RecordBatches (e.g. IPC buffers or C Data) and feed them into the engine without row-by-row conversion. | Medium. Would require a separate code path (e.g. Arrow scan from buffer) and possibly a second register API. | High. New binding, type mapping, and lifecycle. | Medium. More code paths and dependency on Arrow format. |
| **D. Configurable batch size** | Let `options.batchSize` override the effective chunk size. | Medium. Output chunk size is fixed by `DEFAULT_VECTOR_CAPACITY` elsewhere; we could only use it as a hint to JS (same as B) or cap reads at min(batchSize, DEFAULT_VECTOR_CAPACITY). | Low if only hint; higher if we try to expose variable-sized engine chunks. | Low for hint-only. |
| **E. Document only** | Document that each yield must be ≤ DEFAULT_VECTOR_CAPACITY and that larger yields are currently truncated. | High. | Trivial. | Does not fix data loss; only warns. |

**Conclusion from analysis:** A is necessary for correctness (no discard). B improves efficiency (fewer round-trips) and aligns JS batching with engine expectations. C is a larger feature. D (as a hint) overlaps with B. E alone is insufficient.

---

## Validation

- **Remainder buffering (A):** The table function has `initLocalState` and `TableFuncLocalState`; it is called once per thread. The same thread repeatedly calls the table func until the pipeline has enough rows. Storing a `std::vector<std::vector<Value>> remainder` and draining it before invoking getChunk is consistent with the existing pull model. No change to the JS API required.
- **Batch hint (B):** The getChunk callback is invoked from C++ with `requestId` only. Adding a second parameter (e.g. `maxRows`) is a small extension. On the JS side, `runConsumer` can loop `it.next()` until `rows.length >= maxRows || done` (with a cap to avoid infinite loop on slow iterables). Existing callers that ignore the second parameter continue to work.
- **Data loss (current):** Reproduced by yielding one large array (e.g. 10k rows) in a single `next()`; only the first DEFAULT_VECTOR_CAPACITY rows are used. A fixes this; E does not.

---

## Consensus and recommendation

**Primary: fix correctness, then improve batching.**

1. **Implement A (buffer remainder in C++)**  
   - In `NodeStreamScanFunction`, add local state that holds leftover rows (e.g. `std::vector<std::vector<Value>> remainder`).
   - In `tableFunc`, first drain from `remainder` into the output chunk (up to `DEFAULT_VECTOR_CAPACITY`). If remainder is non-empty after that, return without calling getChunk.
   - Only when remainder is empty, request a new chunk from JS. When the response has more than `DEFAULT_VECTOR_CAPACITY` rows, push the excess into `remainder` and use the first `DEFAULT_VECTOR_CAPACITY` for the current output.
   - Ensures no rows are dropped regardless of how the user batches yields.

2. **Implement B (batch size hint to JS)**  
   - Extend the getChunk callback to pass a desired row count: e.g. `getChunk(requestId, maxRows)` with `maxRows = DEFAULT_VECTOR_CAPACITY` (or a constant exposed for JS).
   - In the JS consumer loop, collect rows until `rows.length >= maxRows` or the iterable is done, then call `returnChunk(requestId, rows, done)`. This reduces the number of round-trips when the source can produce many rows per yield.
   - Document that yielding batches of around `maxRows` (e.g. 2048) is optimal; A ensures correctness if larger batches are yielded.

3. **Documentation**  
   - Document that the stream yields "chunks" of rows (array or single row); the engine processes up to DEFAULT_VECTOR_CAPACITY rows per internal chunk; recommending batch sizes of that order avoids unnecessary round-trips.
   - After A, document that larger batches are supported and no longer truncated.

**Optional later**

- **C (Arrow input):** If demand exists for zero-copy or Arrow-native streams, add a separate path (e.g. `registerArrowStream` or an option that accepts IPC buffers / C Data) and design it alongside the existing row-based stream.
- **D:** If we expose a recommended batch size constant to JS (e.g. in the addon or docs), that suffices; a full configurable engine chunk size is not required for the addon.

---

## Implementation sketch

### A. Buffer remainder (C++)

- Add a `TableFuncLocalState` that holds:
  - `std::vector<std::vector<Value>> remainder`
  - (optional) index of next row to consume in remainder for simpler drain logic.
- In `tableFunc`:
  1. If `remainder` is non-empty: copy `min(remainder.size(), DEFAULT_VECTOR_CAPACITY)` rows into `output.dataChunk`, remove those from `remainder`, set `output.dataChunk.state->getSelVectorUnsafe().setSelSize(...)`, return that count.
  2. If `remainder` is empty: call getChunk as today. When the response arrives, if `rowsCopy.size() > DEFAULT_VECTOR_CAPACITY`, push rows from `DEFAULT_VECTOR_CAPACITY` onward into `remainder`, and use rows `0..DEFAULT_VECTOR_CAPACITY-1` for the current output. Else use all rows for the current output.
- Ensure `remainder` is cleared or not reused across streams if the same state were ever reused (current design: one stream per query, so no reuse).

### B. Batch size hint (C++ and JS)

- **C++**: When calling the TSF, pass `requestId` and `maxRows` (e.g. `DEFAULT_VECTOR_CAPACITY`). Update `NodeStreamChunkRequest` if the JS side needs to know the hint (e.g. for logging); the main use is for JS to receive it in the callback.
- **JS**: Change the callback signature to `(requestId, maxRows)`. In `runConsumer`, loop: `while (rows.length < maxRows) { const n = await it.next(); ... rows.push(...); if (done) break; }` then `returnChunk(requestId, rows, done)`.
- **Backward compat**: If `maxRows` is undefined or missing, JS behaves as today (one `it.next()` per request).

### Tests

- Add a test that yields a single array of 5000 rows and asserts all 5000 are returned by `LOAD FROM stream RETURN *` (validates A).
- Add a test that yields row-by-row (5000 times) and asserts all 5000 are returned (validates no regression).
- Optionally: test that with B, a stream that yields in batches of 2048 results in fewer getChunk calls than yielding one row at a time.

---

## References

- Node stream scan: `src_cpp/node_stream_scan.cpp`, `node_scan_replacement.cpp`, `include/node_stream_scan.h`, `include/node_scan_replacement.h` (paths relative to `tools/nodejs_api`).
- JS: `src_js/connection.js` (`registerStream`, consumer loop, `returnChunk`).
- Table function contract: repo `src/include/function/table/table_function.h`, `simple_table_function.h` (maxMorselSize, local state).
- Constant: `DEFAULT_VECTOR_CAPACITY` from repo `common/system_config.h` (generated from `cmake/templates/system_config.h.in`).
