
# Ladybug Node.js API

A high-performance graph database for knowledge-intensive applications. This Node.js wrapper enables interaction with the Ladybug database via JavaScript or TypeScript using either **CommonJS** or **ES Modules**.

---

## 📦 Installation

**Node.js version requirement**

This package **requires Node.js 20 or later**. Older Node.js versions are not supported and installation may fail due to the enforced `engines.node` constraint and native build tooling (`cmake-js` 8.x, `node-addon-api` 8.x).

**From npm (if published):**

```bash
npm install lbug
```

**From GitHub** (monorepo; the Node package lives in `tools/nodejs_api`):

- **pnpm** (v9+), subdirectory is supported:

  ```bash
  pnpm add lbug@github:LadybugDB/ladybug#path:tools/nodejs_api
  ```

  On install, the package will build the native addon from source (needs CMake and a C++20 compiler).

- **npm**: no built-in subdirectory install. Either use a **local path** after cloning and building (see [Build and use in other projects](#-build-and-use-in-other-projects-local)), or a tarball from [GitPkg](https://gitpkg.vercel.app/) (e.g. `https://gitpkg.vercel.app/LadybugDB/ladybug/tools/nodejs_api?main`).

---

## 🚀 Quick Start

### Example (ES Modules)

```js
// Import the Ladybug module (ESM)
import { Database, Connection } from "lbug";

const main = async () => {
  // Initialize database and connection
  const db = new Database("./test");
  const conn = new Connection(db);

  // Define schema
  await conn.query(`
    CREATE NODE TABLE User(name STRING, age INT64, PRIMARY KEY (name));
  `);
  await conn.query(`
    CREATE NODE TABLE City(name STRING, population INT64, PRIMARY KEY (name));
  `);
  await conn.query(`
    CREATE REL TABLE Follows(FROM User TO User, since INT64);
  `);
  await conn.query(`
    CREATE REL TABLE LivesIn(FROM User TO City);
  `);

  // Load data from CSV files
  await conn.query(`COPY User FROM "user.csv"`);
  await conn.query(`COPY City FROM "city.csv"`);
  await conn.query(`COPY Follows FROM "follows.csv"`);
  await conn.query(`COPY LivesIn FROM "lives-in.csv"`);

  // Run a query
  const result = await conn.query("MATCH (u:User) RETURN u.name, u.age;");

  // Consume results (choose one style)
  const rows = await result.getAll();
  for (const row of rows) {
    console.log(row);
  }
};

main().catch(console.error);
```
 ✅ The dataset used in this example can be found in the [official Ladybug repository](https://github.com/LadybugDB/ladybug/tree/master/dataset/demo-db/csv).

---

## 📚 API Overview

**Full API reference:** [docs/API.md](docs/API.md) — types, methods, options, errors, and constants.

The `lbug` package exposes the following primary classes:

* **Database** – `new Database(path, bufferPoolSize?, ...)`. Initialize with `init()` / `initSync()` (optional; done on first use). When the file is locked, **async init() retries for up to 5s** (configurable: last ctor arg `openLockRetryMs`; set `0` to fail immediately). Close with `close()`.
* **Connection** – `new Connection(database, numThreads?)`. Run Cypher with `query(statement)` or `prepare(statement)` then `execute(preparedStatement, params)`. Use `transaction(fn)` for a single write transaction, `ping()` for liveness checks. **`getNumNodes(nodeName)`** and **`getNumRels(relName)`** return row counts for node/rel tables. Use `registerStream(name, source, { columns })` to load data from an AsyncIterable via `LOAD FROM name`; `unregisterStream(name)` when done. Configure with `setQueryTimeout(ms)`, `setMaxNumThreadForExec(n)`.
* **QueryResult** – Returned by `query()` / `execute()`. Consume with `getAll()`, `getNext()` / `hasNext()`, **async iteration** (`for await...of`), or **`toStream()`** (Node.js `Readable`). Use **`toString()`** for a string representation (header + rows; useful for debugging). Metadata: `getColumnNames()`, `getColumnDataTypes()`, `getQuerySummary()`. Call `close()` when done (optional if fully consumed).
* **PreparedStatement** – Created by `conn.prepare(statement)`. Execute with `conn.execute(preparedStatement, params)`. Reuse for parameterized queries.
* **Pool** – `createPool({ databasePath, maxSize, ... })` returns a connection pool. Use **`pool.run(conn => ...)`** (recommended) or `acquire()` / `release(conn)`; call **`pool.close()`** when done.

Both CommonJS (`require`) and ES Modules (`import`) are fully supported.

### Consuming query results

```js
const result = await conn.query("MATCH (n:User) RETURN n.name LIMIT 1000");

// Option 1: get all rows (loads into memory)
const rows = await result.getAll();

// Option 2: row by row (async)
while (result.hasNext()) {
  const row = await result.getNext();
  console.log(row);
}

// Option 3: async iterator (streaming, no full materialization)
for await (const row of result) {
  console.log(row);
}

// Option 4: Node.js Readable stream (e.g. for .pipe())
const stream = result.toStream();
stream.on("data", (row) => console.log(row));

// Option 5: string representation (e.g. for debugging)
console.log(result.toString());
```

### Table counts

After creating node/rel tables and loading data, you can get row counts:

```js
conn.initSync(); // or await conn.init()
const numUsers = conn.getNumNodes("User");
const numFollows = conn.getNumRels("Follows");
```

### Connection pool

Use **`createPool(options)`** to get a pool of connections (one shared `Database`, up to `maxSize` connections). Prefer **`pool.run(fn)`**: it acquires a connection, runs `fn(conn)`, and releases in `finally` (on success or throw), so you never leak a connection.

**Options:** `maxSize` (required), `databasePath`, `databaseOptions` (same shape as `Database` constructor), `minSize` (default 0), `acquireTimeoutMillis` (default 0 = wait forever), `validateOnAcquire` (default false; if true, `conn.ping()` before hand-out).

**Example (recommended: `run`):**

```js
import { createPool } from "lbug";

const pool = createPool({ databasePath: "./mydb", maxSize: 10 });

const rows = await pool.run(async (conn) => {
  const result = await conn.query("MATCH (u:User) RETURN u.name LIMIT 5");
  const rows = await result.getAll();
  result.close();
  return rows;
});
console.log(rows);

await pool.close();
```

**Manual acquire/release:** If you need the same connection for multiple operations, use `acquire()` and always call `release(conn)` in a `finally` block so the connection is returned even on throw.

```js
const conn = await pool.acquire();
try {
  await conn.query("...");
  // ...
} finally {
  pool.release(conn);
}
```

When shutting down, call **`pool.close()`**: it rejects new and pending `acquire()`, then closes all connections and the database.

### Transactions

**Manual:** Run `BEGIN TRANSACTION`, then your queries, then `COMMIT` or `ROLLBACK`. On error, call `ROLLBACK` before continuing.

```js
await conn.query("BEGIN TRANSACTION");
await conn.query("CREATE NODE TABLE Nodes(id INT64, PRIMARY KEY(id))");
await conn.query('COPY Nodes FROM "data.csv"');
await conn.query("COMMIT");
// or on error: await conn.query("ROLLBACK");
```

**Read-only transaction:** `BEGIN TRANSACTION READ ONLY` then queries, then `COMMIT` / `ROLLBACK`.

**Wrapper:** One write transaction with automatic commit on success and rollback on throw:

```js
await conn.transaction(async () => {
  await conn.query("CREATE NODE TABLE Nodes(id INT64, PRIMARY KEY(id))");
  await conn.query('COPY Nodes FROM "data.csv"');
  // commit happens automatically; on throw, rollback then rethrow
});
```

### Loading data from a Node.js stream

You can feed data from an **AsyncIterable** (generator, async generator, or any `Symbol.asyncIterator`) into Cypher using **scan replacement**: register a stream by name, then use `LOAD FROM name` in your query. Rows are pulled from JavaScript on demand during execution.

**API:**

* **`conn.registerStream(name, source, options)`** (async)  
  * `name` – string used in Cypher: `LOAD FROM name RETURN ...`  
  * `source` – AsyncIterable of rows. Each row is an **array** of column values (same order as `options.columns`) or an **object** keyed by column name.  
  * `options.columns` – **required**. Schema: array of `{ name: string, type: string }`. Supported types: `INT64`, `INT32`, `INT16`, `INT8`, `UINT64`, `UINT32`, `DOUBLE`, `FLOAT`, `STRING`, `BOOL`, `DATE`, `TIMESTAMP`.

* **`conn.unregisterStream(name)`**  
  Unregisters the source so the name can be reused or to avoid leaving stale entries. Call after the query (or when done with the stream).

**Example:**

```js
async function* generateRows() {
  yield [1, "Alice"];
  yield [2, "Bob"];
  yield [3, "Carol"];
}

await conn.registerStream("users", generateRows(), {
  columns: [
    { name: "id", type: "INT64" },
    { name: "name", type: "STRING" },
  ],
});

const result = await conn.query("LOAD FROM users RETURN *");
for await (const row of result) {
  console.log(row); // { id: 1, name: "Alice" }, ...
}

conn.unregisterStream("users");
```

You can combine the stream with other Cypher: e.g. `LOAD FROM stream RETURN * WHERE col > 0`, or `COPY MyTable FROM (LOAD FROM stream RETURN *)`.

### Database locked

Only one process can open the same database path for writing. If the file is already locked, **async `init()` retries for up to 5 seconds** by default (grace period), then throws. You can tune or disable this:

- **Default**: `new Database("./my.db")` — last ctor arg `openLockRetryMs` defaults to `5000` (retry for up to 5s on lock).
- **No retry**: `new Database("./my.db", 0, true, false, 0, true, -1, true, true, 0)` or pass `openLockRetryMs = 0` as the 10th argument to fail immediately.
- **Longer grace**: e.g. `openLockRetryMs = 3000` to wait up to 3s.

The error has **`code === 'LBUG_DATABASE_LOCKED'`** so you can catch and handle it if the grace period wasn’t enough:

```js
import { Database, Connection, LBUG_DATABASE_LOCKED } from "lbug";

const db = new Database("./my.db"); // already retries ~5s on lock
try {
  await db.init();
} catch (err) {
  if (err.code === LBUG_DATABASE_LOCKED) {
    console.error("Database still locked after grace period.");
  }
  throw err;
}
const conn = new Connection(db);
```

Use **read-only** mode for concurrent readers: `new Database(path, undefined, undefined, true)` so multiple processes can open the same DB for read.

See [docs/database_locked.md](docs/database_locked.md) for how other systems handle this and best practices.

---

## 🛠️ Local Development (for Contributors)

### Install Dev Dependencies

```bash
npm install --include=dev
```

### Build Project

```bash
npm run build
```

### Run Tests

```bash
npm test
```

When developing from the **monorepo root**, build the native addon first so tests see the latest C++ code:

```bash
# From repo root (D:\prj\ladybug or similar)
make nodejs
# Or: cmake --build build/release --target lbugjs
# Then from tools/nodejs_api:
cd tools/nodejs_api && npm test
```

---

## 🔧 Build and use in other projects (local)

To use the Node.js API from the Ladybug repo in another project without publishing to npm:

1. **Build the addon** (from the Ladybug repo root):

   ```bash
   make nodejs
   ```

   Or from this directory:

   ```bash
   npm run build
   ```

   This compiles the native addon into `build/lbugjs.node` and copies JS and types.

2. **In your other project**, add a file dependency in `package.json`:

   ```json
   "dependencies": {
     "lbug": "file:../path/to/ladybug/tools/nodejs_api"
   }
   ```

   Then run `npm install`. After that, `require("lbug")` or `import ... from "lbug"` will use your local build.

3. **Optional:** to pack and install a tarball instead:

   ```bash
   cd /path/to/ladybug/tools/nodejs_api
   npm run build
   npm pack
   ```

   In the other project: `npm install /path/to/ladybug/tools/nodejs_api/lbug-0.0.1.tgz`.

### Prebuilt in your fork (install from GitHub without building)

If you install from GitHub (e.g. `pnpm add lbug@github:user/ladybug#path:tools/nodejs_api`), the package runs `install.js`: if it finds a prebuilt binary, it uses it and does not build from source. To ship a prebuilt in your fork:

1. **Build once** in your clone (from repo root):

   ```bash
   make nodejs
   ```

2. **Create the prebuilt file** (name = `lbugjs-<platform>-<arch>.node`):

   - Windows x64: copy `tools/nodejs_api/build/lbugjs.node` → `tools/nodejs_api/prebuilt/lbugjs-win32-x64.node`
   - Linux x64: `lbugjs-linux-x64.node`
   - macOS x64: `lbugjs-darwin-x64.node`, arm64: `lbugjs-darwin-arm64.node`

   Example (from repo root). **Windows (PowerShell):**

   ```powershell
   New-Item -ItemType Directory -Force -Path tools/nodejs_api/prebuilt
   Copy-Item tools/nodejs_api/build/lbugjs.node tools/nodejs_api/prebuilt/lbugjs-win32-x64.node
   ```

   **Linux/macOS:**

   ```bash
   mkdir -p tools/nodejs_api/prebuilt
   cp tools/nodejs_api/build/lbugjs.node tools/nodejs_api/prebuilt/lbugjs-$(node -p "process.platform")-$(node -p "process.arch").node
   ```

3. **Commit and push** the `prebuilt/` folder. Then anyone (or you in another project) can do:

   ```bash
   pnpm add lbug@github:YOUR_USERNAME/ladybug#path:tools/nodejs_api
   ```

   and the addon will be used from prebuilt without a local build.

---

## 📦 Packaging and Binary Distribution

We bundle all prebuilt binaries directly into the npm package, inspired by the approach used by [prebuildify](https://github.com/prebuild/prebuildify).

>  All prebuilt binaries are shipped inside the package that is published to npm, which means there's no need for a separate download step like you find in [`prebuild`](https://github.com/prebuild/prebuild). The irony of this approach is that it is faster to download all prebuilt binaries for every platform when they are bundled than it is to download a single prebuilt binary as an install script.

### Requirements (for building from source)

If a prebuilt binary is unavailable for your platform, the module will be built from source during installation. Ensure the following tools are installed:

* **CMake** (≥ 3.15)
* **Python 3**
* A **C++20-compatible compiler**

### Packaging Prebuilt Binaries

1. Place your binaries inside the `prebuilt` directory.
2. Name them using the format:

   ```
   lbugjs-${platform}-${arch}.node
   ```
3. Run the packaging script:

```bash
node package
```

If no binaries are found, a source-only tarball will be generated.

---

## 🚀 Publishing

To publish the package to npm:

```bash
npm publish
```

Refer to the [npm documentation](https://docs.npmjs.com/cli/v9/commands/npm-publish) for full details on publishing and versioning.

---

## 🔗 Resources

* [Ladybug GitHub](https://github.com/lbugdb/lbug)
* [Ladybug Documentation](https://docs.ladybugdb.com)
* [Issue Tracker](https://github.com/LadybugDB/ladybug/issues)
