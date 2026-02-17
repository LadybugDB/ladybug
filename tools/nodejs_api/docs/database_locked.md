# Database locked

## When it happens

The database file is locked when:

- Another process has already opened the same path for read-write (e.g. another Node app, the Ladybug shell, or a backup).
- You open the same path twice in one process (e.g. two `Database` instances to the same path) and both try to write.

Opening is done at the first use: `db.init()`, `db.initSync()`, or the first `conn.query()` on that database. If the OS file lock cannot be acquired, the native layer throws and the Node API surfaces it as an **Error with `code === 'LBUG_DATABASE_LOCKED'`**.

## How other systems handle it

| System   | Approach |
|----------|----------|
| **SQLite** | `busy_timeout` (e.g. 5 seconds): block until lock is released or timeout, then return `SQLITE_BUSY`. Apps often retry with exponential backoff. |
| **DuckDB** | Open fails immediately if locked; application retries with backoff. |
| **LMDB** | Single writer; readers use `MDB_NOLOCK` or shared lock. Writers get exclusive lock. |
| **RocksDB** | Options for concurrent access; single process or client–server. |

Common patterns:

1. **Fail fast** — return a clear error (e.g. “database locked”) so the app can show a message or retry.
2. **Retry with backoff** — in application code: catch the error, wait (e.g. 50 ms, 100 ms, 200 ms), try again, then give up.
3. **Block with timeout** — wait up to N ms for the lock (requires support in the engine; Ladybug currently uses “fail immediately”).
4. **Read-only for readers** — open in read-only mode so multiple processes can read; only one writer.

## What the Node API does

- **Grace period (async init only)**: When you open a database with async `init()` (or the first `query()`), the driver **retries for up to 5 seconds** by default if the file is locked. So short-lived contention (e.g. MCP server or another tool briefly holding the lock) often succeeds without you doing anything. Configure with the last constructor argument `openLockRetryMs` (default `5000`; set `0` to fail immediately).
- **Clear error**: After the grace period or when retry is disabled, you get an Error whose message includes “Could not set lock on file” and a link to the concurrency docs.
- **Error code**: The error is normalized so `err.code === 'LBUG_DATABASE_LOCKED'`. You can import `LBUG_DATABASE_LOCKED` from `lbug` and catch it if you need custom retry or messaging.
- **Sync init**: `initSync()` does not retry; it fails immediately on lock (no blocking wait in the driver).

## Best practices

1. **One writer per path** — avoid opening the same on-disk database for write from more than one process at a time.
2. **Concurrent readers** — use `new Database(path, undefined, undefined, true)` (read-only) so multiple processes can read the same DB.
3. **Retry with backoff** — if you expect short-lived contention (e.g. restart or another tool), catch `LBUG_DATABASE_LOCKED`, wait, and retry a few times.
4. **Close when done** — call `db.close()` so the lock is released for other processes.
