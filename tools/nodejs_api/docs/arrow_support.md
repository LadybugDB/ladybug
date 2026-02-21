# Arrow support in the Node.js addon

## Current state

- **Node addon** (`tools/nodejs_api`): no Arrow. `QueryResult` exposes only row-by-row (`getNext`, `getAll`) and metadata (column names, types, summary). No `getAsArrow()` or similar.
- **Python API**: has `get_as_arrow(chunk_size, fallbackExtensionTypes)` which uses C++ `ArrowRowBatch` + `ArrowConverter` and PyArrow's C Data Interface (`RecordBatch._import_from_c`, `Schema._import_from_c`).
- **C++**: `MaterializedQueryResult` already has `hasNextArrowChunk()` and `getNextArrowChunk(chunkSize)` returning `std::unique_ptr<ArrowArray>`. Schema is built via `ArrowConverter::toArrowSchema()`. Release callbacks are set in `arrow_row_batch.cpp` and `arrow_converter.cpp`. The core Arrow export path exists and is used by Python.

## Goal

Expose the same Arrow export from the Node addon so that JS can get query results as Apache Arrow (e.g. `Table` from `apache-arrow` npm).

---

## Options and analysis

| Option | Description | Feasibility | Effort | Risk |
|--------|-------------|-------------|--------|------|
| **A. IPC (Buffer)** | C++ produces Arrow IPC stream bytes; JS uses `tableFromIPC(buffer)` from `apache-arrow`. | High. IPC format is fixed; `tableFromIPC` is standard. | Medium if implementing a minimal IPC writer in-tree; low if linking Arrow C++ (adds dependency). | Custom writer: bugs on nested types, alignment. Arrow C++: build size and version coupling. |
| **B. C Data Interface** | C++ exposes `ArrowArray`/`ArrowSchema` pointers; JS imports them zero-copy. | Medium. Arrow JS does not ship C→JS import; would need a small native helper or copy path. | High (native bridge that reads C structs and builds Arrow JS `Table`/`RecordBatch`), unless a maintained helper exists. | Lifetime and release callbacks must be correct; otherwise leaks or use-after-free. |
| **C. Chunked IPC** | Same as A but return multiple IPC buffers (one per chunk) for streaming. | High, same as A. | Same as A plus JS loop to combine or stream. | Same as A. |

**Conclusion from analysis:** A (or C) is the most predictable: no new JS native layer, standard format, and `tableFromIPC` is the supported entry point. B is attractive for zero-copy but depends on a C→JS import path that is not currently provided by `apache-arrow` npm.

---

## Validation

- **IPC path:** Confirmed that `apache-arrow` npm provides `tableFromIPC(buffer)` and accepts `Uint8Array`/Node `Buffer`. Single-buffer (file-style) IPC is sufficient for a first version; streaming IPC can be added later if needed.
- **C Data path:** The main `apache-arrow` JS package does not expose a C→JS import API. Projects like `arrow-js-ffi` target WASM, not Node addons. So B would require custom native code to interpret `ArrowArray`/`ArrowSchema` and construct JS Arrow structures, which duplicates logic and increases maintenance.
- **Reuse of C++:** The existing `getNextArrowChunk` and `ArrowConverter::toArrowSchema` produce valid C Data structures with correct release callbacks. Any IPC writer would consume these same structures; no change to the core export logic.

---

## Consensus and recommendation

**Primary approach: IPC (Option A).**

1. Add a way in C++ to serialize the result to Arrow IPC format and return it as a single buffer to the Node addon. Two sub-variants:
   - **A1.** Implement a minimal IPC writer in-tree (schema + record batch messages, no dependency on Arrow C++). Prefer if the project avoids adding the Arrow C++ dependency.
   - **A2.** Link Arrow C++ and use its IPC writer. Prefer if the dependency is acceptable and we want to avoid maintaining IPC format code.
2. In the Node addon, expose a method that returns this buffer (e.g. `getAsArrowBuffer(chunkSize?)`), and in JS a wrapper that calls `tableFromIPC(buffer)` so the addon API can offer `getAsArrow(chunkSize?)` returning an Arrow `Table`.
3. Optional later: chunked/streaming IPC (Option C) if we need to avoid materializing the full result in one buffer.

**When to reconsider C Data (Option B):** If a maintained, Node-oriented C Data import (C→JS) appears (e.g. in `apache-arrow` or a small official helper), then B becomes viable for zero-copy and we can evaluate it as an alternative or addition.

---

## Implementation sketch (IPC path)

1. **C++ (node addon)**
   - In `NodeQueryResult`:
     - Include `main/query_result/materialized_query_result.h`, `common/arrow/arrow_converter.h`, `common/arrow/arrow_row_batch.h`.
     - Cast `queryResult` to `MaterializedQueryResult*` (or use existing virtuals on `QueryResult` where available).
     - New method: get column types/names; build `ArrowSchema` once via `ArrowConverter::toArrowSchema`; loop with `getNextArrowChunk(chunkSize)`; serialize each batch (and schema) to IPC bytes; return a single N-API buffer (or array of buffers).
   - IPC: either implement minimal writer (schema + record batch layout) or call Arrow C++ IPC API. Ensure `ArrowArray`/`ArrowSchema` release callbacks are invoked after serialization so memory is not leaked.

2. **JS (query_result.js)**
   - Add `getAsArrow(chunkSize?)`: call the new native method to get IPC buffer(s), then `tableFromIPC(buffer)` (or concatenate buffers if multiple). Return the Arrow `Table`.
   - Add optional (or peer) dependency on `apache-arrow` and document it for users who need Arrow.

3. **Build**
   - Addon already links `lbug`; Arrow export lives inside `lbug`. Only if using A2: add Arrow C++ to the node addon's link step.

4. **Tests**
   - Assert `getAsArrow()` returns a Table with expected column names, types, and row count for a few queries and chunk sizes.

---

## References

- Python: `tools/python_api/src_cpp/py_query_result.cpp` (getAsArrow, getNextArrowChunk, ArrowConverter::toArrowSchema).
- C++ result: `src/main/query_result/materialized_query_result.cpp` (getNextArrowChunk), `src/common/arrow/arrow_row_batch.cpp`, `arrow_converter.cpp`.
- Node addon: `src_cpp/node_query_result.cpp`, `include/node_query_result.h` (paths relative to `tools/nodejs_api`).
- Arrow JS: `tableFromIPC`, [Arrow JS docs](https://arrow.apache.org/docs/js/).
- Arrow IPC format: [IPC format](https://arrow.apache.org/docs/format/IPC.html).
