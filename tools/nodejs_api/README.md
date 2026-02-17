
# Ladybug Node.js API

A high-performance graph database for knowledge-intensive applications. This Node.js wrapper enables interaction with the Ladybug database via JavaScript or TypeScript using either **CommonJS** or **ES Modules**.

---

## 📦 Installation

```bash
npm install lbug
```

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

The `lbug` package exposes the following primary classes:

* **Database** – `new Database(path, bufferPoolSize?, ...)`. Initialize with `init()` / `initSync()` (optional; done on first use). Close with `close()`.
* **Connection** – `new Connection(database, numThreads?)`. Run Cypher with `query(statement)` or `prepare(statement)` then `execute(preparedStatement, params)`. Use `transaction(fn)` for a single write transaction, `ping()` for liveness checks. Configure with `setQueryTimeout(ms)`, `setMaxNumThreadForExec(n)`.
* **QueryResult** – Returned by `query()` / `execute()`. Consume with `getAll()`, `getNext()` / `hasNext()`, **async iteration** (`for await...of`), or **`toStream()`** (Node.js `Readable`). Metadata: `getColumnNames()`, `getColumnDataTypes()`, `getQuerySummary()`. Call `close()` when done (optional if fully consumed).
* **PreparedStatement** – Created by `conn.prepare(statement)`. Execute with `conn.execute(preparedStatement, params)`. Reuse for parameterized queries.

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
```

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
