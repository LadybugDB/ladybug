# Ladybug Node.js API Reference

Detailed API documentation for the `lbug` package. For installation, quick start, and usage patterns see [README.md](../README.md).

---

## Module exports

**CommonJS:**

```js
const lbug = require("lbug");
// or
const { Database, Connection, PreparedStatement, QueryResult, createPool, Pool, LBUG_DATABASE_LOCKED, VERSION, STORAGE_VERSION } = require("lbug");
```

**ES Modules:**

```js
import lbug from "lbug";
// or
import { Database, Connection, PreparedStatement, QueryResult, createPool, Pool, LBUG_DATABASE_LOCKED, VERSION, STORAGE_VERSION } from "lbug";
```

| Export | Description |
|--------|-------------|
| `Database` | Database instance (path, options). |
| `Connection` | Connection to a database; runs Cypher and manages streams. |
| `PreparedStatement` | Prepared Cypher statement (from `Connection.prepare`). |
| `QueryResult` | Result of `query()` / `execute()`; async iterable, stream, getAll, etc. |
| `createPool` | Factory: `createPool(options)` → `Pool`. |
| `Pool` | Connection pool (use `createPool`, not `new Pool`). |
| `LBUG_DATABASE_LOCKED` | Error code string when DB file is locked. |
| `VERSION` | Library version string. |
| `STORAGE_VERSION` | Storage version (bigint). |

---

## Types (TypeScript / JSDoc)

### Value types

| Type | Description |
|------|-------------|
| `Nullable<T>` | `T \| null` |
| `Callback<T>` | `(error: Error \| null, result?: T) => void` |
| `ProgressCallback` | `(pipelineProgress, numPipelinesFinished, numPipelines) => void` |
| `QueryOptions` | `{ signal?: AbortSignal; progressCallback?: ProgressCallback }` |
| `NodeID` | `{ offset: number; table: number }` |
| `NodeValue` | `{ _label: string \| null; _id: NodeID \| null; [key: string]: any }` |
| `RelValue` | `{ _src, _dst, _label, _id; [key: string]: any }` |
| `RecursiveRelValue` | `{ _nodes: any[]; _rels: any[] }` |
| `LbugValue` | `null \| boolean \| number \| bigint \| string \| Date \| NodeValue \| RelValue \| RecursiveRelValue \| LbugValue[] \| { [key: string]: LbugValue }` |

### Config types

| Type | Description |
|------|-------------|
| `SystemConfig` | Database options (bufferPoolSize, enableCompression, readOnly, maxDBSize, autoCheckpoint, checkpointThreshold). |
| `PoolDatabaseOptions` | Same shape as Database constructor options (no path): bufferManagerSize, enableCompression, readOnly, maxDBSize, autoCheckpoint, checkpointThreshold, throwOnWalReplayFailure, enableChecksums, openLockRetryMs. |
| `PoolOptions` | databasePath?, databaseOptions?, minSize?, **maxSize**, acquireTimeoutMillis?, validateOnAcquire?. |
| `QuerySummary` | `{ compilingTime: number; executionTime: number }` (milliseconds). |

---

## Database

In-process database instance. One database can be shared by multiple `Connection` instances (e.g. in a pool).

### Constructor

```ts
new Database(
  databasePath?: string,           // default ":memory:"
  bufferManagerSize?: number,      // default 0
  enableCompression?: boolean,     // default true
  readOnly?: boolean,              // default false
  maxDBSize?: number,              // default 0
  autoCheckpoint?: boolean,       // default true
  checkpointThreshold?: number,   // default -1
  throwOnWalReplayFailure?: boolean, // default true
  enableChecksums?: boolean,       // default true
  openLockRetryMs?: number        // default 5000; 0 = fail immediately on lock
)
```

- **databasePath**: `":memory:"` or path to directory. Empty/undefined → `":memory:"`.
- **openLockRetryMs**: Only for async `init()`. Retry opening for up to this many ms when file is locked. Ignored for `:memory:`.

### Instance methods

| Method | Returns | Description |
|--------|---------|-------------|
| `init()` | `Promise<void>` | Initialize DB (optional; done on first use). Retries on lock for up to `openLockRetryMs`. |
| `initSync()` | `void` | Initialize synchronously; blocks. No retry on lock. |
| `close()` | `Promise<void>` | Close and release resources. |
| `closeSync()` | `void` | Close synchronously. |

### Static methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Database.getVersion()` | `string` | Library version. |
| `Database.getStorageVersion()` | `number` | Storage version. |

### Errors

- Lock errors on init are normalized to `Error` with `code === LBUG_DATABASE_LOCKED`. See [database_locked.md](database_locked.md).

---

## Connection

Connection to a `Database`. Use for queries, prepared statements, transactions, streams, and metadata.

### Constructor

```ts
new Connection(database: Database, numThreads?: number)
```

- **numThreads**: Max threads for query execution. Can be set later with `setMaxNumThreadForExec(numThreads)`.

### Initialization

| Method | Returns | Description |
|--------|---------|-------------|
| `init()` | `Promise<void>` | Initialize connection (optional; done on first query). |
| `initSync()` | `void` | Initialize synchronously; may block. |

### Query execution

| Method | Returns | Description |
|--------|---------|-------------|
| `query(statement, optionsOrProgressCallback?)` | `Promise<QueryResult \| QueryResult[]>` | Execute Cypher. Options: `{ signal?, progressCallback? }`. Rejects with `AbortError` if `signal` aborted. |
| `querySync(statement)` | `QueryResult \| QueryResult[]` | Execute synchronously; blocks. |
| `prepare(statement)` | `Promise<PreparedStatement>` | Prepare a statement. |
| `prepareSync(statement)` | `PreparedStatement` | Prepare synchronously. |
| `execute(preparedStatement, params?, optionsOrProgressCallback?)` | `Promise<QueryResult \| QueryResult[]>` | Execute prepared statement with `params` object. Same options as `query`. |
| `executeSync(preparedStatement, params?)` | `QueryResult \| QueryResult[]` | Execute prepared statement synchronously. |

**params**: Plain object, e.g. `{ name: "Alice", age: 30 }`. Keys must match parameter names in the prepared Cypher.

### Transaction

| Method | Returns | Description |
|--------|---------|-------------|
| `transaction(fn)` | `Promise<T>` | Run `fn()` in a single write transaction. `BEGIN TRANSACTION` → fn() → `COMMIT` on success, `ROLLBACK` on throw. |

### Configuration and control

| Method | Returns | Description |
|--------|---------|-------------|
| `setMaxNumThreadForExec(numThreads)` | `void` | Max threads for execution. |
| `setQueryTimeout(timeoutInMs)` | `void` | Query timeout in ms; queries aborted after this. |
| `interrupt()` | `void` | Interrupt current query on this connection. No-op if none running. |

### Metadata and health

| Method | Returns | Description |
|--------|---------|-------------|
| `ping()` | `Promise<boolean>` | Liveness check; rejects if connection broken. |
| `explain(statement)` | `Promise<string>` | Run EXPLAIN on Cypher; returns plan string (one row per line). |
| `getNumNodes(nodeName)` | `number` | Count of nodes in node table. Connection must be initialized. |
| `getNumRels(relName)` | `number` | Count of relationships in rel table. |

### Stream source (LOAD FROM)

| Method | Returns | Description |
|--------|---------|-------------|
| `registerStream(name, source, options)` | `Promise<void>` | Register AsyncIterable as `LOAD FROM name`. **options.columns** required: `[{ name, type }]`. Types: INT64, INT32, INT16, INT8, UINT64, UINT32, DOUBLE, FLOAT, STRING, BOOL, DATE, TIMESTAMP. |
| `unregisterStream(name)` | `void` | Unregister stream by name. |

**source**: AsyncIterable of rows; each row is an array (column order) or object (column names).

### Lifecycle

| Method | Returns | Description |
|--------|---------|-------------|
| `close()` | `Promise<void>` | Close connection. |
| `closeSync()` | `void` | Close synchronously. |

---

## PreparedStatement

Created by `Connection.prepare()` / `Connection.prepareSync()`. Do not construct directly.

### Instance methods

| Method | Returns | Description |
|--------|---------|-------------|
| `isSuccess()` | `boolean` | Whether preparation succeeded. |
| `getErrorMessage()` | `string` | Error message if preparation failed. |

Execution is via `conn.execute(preparedStatement, params)` or `conn.executeSync(preparedStatement, params)`. If `!isSuccess()`, `execute` rejects with `getErrorMessage()`.

---

## QueryResult

Returned by `Connection.query()`, `Connection.querySync()`, `Connection.execute()`, `Connection.executeSync()`. Implements `AsyncIterable<Record<string, LbugValue> | null>`.

### Consumption (pick one style)

| Method / usage | Returns | Description |
|----------------|---------|-------------|
| `getAll()` | `Promise<Record[]>` | All rows (loads into memory). |
| `getAllSync()` | `Record[]` | All rows synchronously. |
| `getNext()` | `Promise<Record \| null>` | Next row; null when exhausted. |
| `getNextSync()` | `Record \| null` | Next row synchronously. |
| `hasNext()` | `boolean` | Whether more rows exist. |
| `for await (const row of result)` | — | Async iteration; no full materialization. |
| `toStream()` | `stream.Readable` | Node.js Readable (object mode), one row per chunk. |
| `each(resultCb, doneCb, errorCb)` | `void` | Callback-based iteration. |
| `all(resultCb, errorCb)` | `void` | Callback with all rows. |
| `toString()` | `string` | Header + rows (or error message for failed query). |

### Metadata

| Method | Returns | Description |
|--------|---------|-------------|
| `getNumTuples()` | `number` | Number of rows. |
| `getColumnNames()` | `Promise<string[]>` | Column names. |
| `getColumnNamesSync()` | `string[]` | Column names synchronously. |
| `getColumnDataTypes()` | `Promise<string[]>` | Column data types. |
| `getColumnDataTypesSync()` | `string[]` | Column types synchronously. |
| `getQuerySummary()` | `Promise<QuerySummary>` | `{ compilingTime, executionTime }` in ms. |
| `getQuerySummarySync()` | `QuerySummary` | Same, synchronously. |

### Other

| Method | Returns | Description |
|--------|---------|-------------|
| `resetIterator()` | `void` | Reset cursor to start (for re-iteration). |
| `close()` | `void` | Release resources. Optional if fully consumed. |

**Multiple results**: A batch of statements can return `QueryResult[]`. Single statement returns one `QueryResult`.

---

## Pool and createPool

Connection pool: one shared `Database`, up to `maxSize` `Connection` instances.

### createPool(options)

```ts
function createPool(options: PoolOptions): Pool
```

**PoolOptions:**

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `databasePath` | string | `":memory:"` | DB path. |
| `databaseOptions` | PoolDatabaseOptions | — | Same shape as Database constructor (no path). |
| `minSize` | number | 0 | Minimum connections to keep. |
| `maxSize` | number | **required** | Maximum connections. |
| `acquireTimeoutMillis` | number | 0 | Max wait for acquire (0 = wait forever). |
| `validateOnAcquire` | boolean | false | If true, call `conn.ping()` before handing out. |

### Pool methods

| Method | Returns | Description |
|--------|---------|-------------|
| `acquire()` | `Promise<Connection>` | Get a connection; **must** call `release(conn)` when done. |
| `release(conn)` | `void` | Return connection to pool. |
| `run(fn)` | `Promise<T>` | Acquire, run `fn(conn)`, release in `finally`. Preferred over manual acquire/release. |
| `close()` | `Promise<void>` | Reject new/pending acquire; close all connections and database. |

**Example:**

```js
const pool = createPool({ databasePath: "./mydb", maxSize: 10 });
const rows = await pool.run(async (conn) => {
  const result = await conn.query("MATCH (u:User) RETURN u.name LIMIT 5");
  const rows = await result.getAll();
  result.close();
  return rows;
});
await pool.close();
```

---

## Constants

| Name | Type | Description |
|------|------|-------------|
| `LBUG_DATABASE_LOCKED` | `"LBUG_DATABASE_LOCKED"` | Error code when DB file is locked. Use with `err.code === LBUG_DATABASE_LOCKED`. |
| `VERSION` | string | Library version (same as `Database.getVersion()`). |
| `STORAGE_VERSION` | bigint | Storage version (same as `Database.getStorageVersion()`). |

---

## Query options and cancellation

- **signal**: Pass `AbortSignal` (e.g. from `AbortController`) in options to cancel `query()` or `execute()`. On abort, the promise rejects with `DOMException` "AbortError".
- **progressCallback**: `(pipelineProgress, numPipelinesFinished, numPipelines) => void`. Optional progress updates during execution.

Legacy: you can pass a single function as the second argument to `query(statement, progressCallback)` or `execute(ps, params, progressCallback)` instead of an options object.

---

## Error handling

- **Database lock**: Async `init()` retries for `openLockRetryMs` (default 5s). Then throws with `code === LBUG_DATABASE_LOCKED`. See [database_locked.md](database_locked.md).
- **Abort**: When `options.signal` is aborted, `query`/`execute` reject with `DOMException` "AbortError".
- **Prepared statement**: If `!preparedStatement.isSuccess()`, `execute` rejects with `preparedStatement.getErrorMessage()`.
- **Validation**: Invalid arguments (e.g. non-object params, wrong types) throw `Error` with descriptive messages.

---

## Related docs

- [README.md](../README.md) — Installation, quick start, transactions, stream loading, pool usage, prebuilt binaries.
- [database_locked.md](database_locked.md) — Lock behavior, retry, read-only, best practices.
- [execution_chain_analysis.md](execution_chain_analysis.md) — LOAD FROM stream execution chain (for implementers).
