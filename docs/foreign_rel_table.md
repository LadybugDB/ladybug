# Foreign Rel Tables — Representing Graphs in SQL Databases

## 1. What Is a Foreign Table?

In Ladybug, a **foreign table** is a table whose data lives **outside** the
native Ladybug storage engine — typically in an external SQL database such as
DuckDB, PostgreSQL, SQLite, or any database accessible via ADBC. Foreign tables
are registered through the `ATTACH` command:

```cypher
ATTACH 'path/to/database.db' AS mydb (dbtype duckdb);
ATTACH 'host=localhost dbname=mydb' AS mydb (dbtype PG_CLIENT);
```

When a database is attached, its tables become visible to Cypher queries through
a catalog extension (e.g., `DuckDBCatalog`, `PgClientCatalog`, `ADBCCatalog`).
Each foreign table is represented in the catalog as either a
`NodeTableCatalogEntry` (for node tables) or a `RelGroupCatalogEntry` (for rel
tables), backed by a scan function that delegates reads to the remote database
via SQL queries.

Because the data is not managed by Ladybug's transaction engine, foreign tables
are **read-only** — inserts, updates, and deletes are rejected at the storage
layer.

## 2. Representing Graphs in Foreign Tables

Ladybug uses a **convention-based** approach to recognize graph elements in a
foreign SQL database. When a database is attached, the catalog extension
enumerates the tables in the remote schema and classifies them by name prefix.

### 2.1. Naming Convention

| Prefix       | Classification        | Internal Representation                            |
|--------------|-----------------------|-----------------------------------------------------|
| `node_*`     | Node table            | `NodeTableCatalogEntry` (shadow entry)              |
| `rel_*`      | FK-based rel table    | `RelGroupCatalogEntry` + `ForeignRelTable`          |
| `csr_rel_*`  | CSR-style rel table   | Local on-disk CSR `RelTable` (TODO: materialize)    |
| Other        | Plain foreign table   | `NodeTableCatalogEntry` (generic, no graph semantics) |

The two rel-table variants correspond to different physical representations of
edges in the foreign database:

#### `rel_*` — Foreign-Key-backed Rel Table

A `rel_*` table stores edges whose endpoints are resolved at attach time by
querying the foreign database's foreign-key metadata against the corresponding
`node_*` tables. The naming mirrors the adjacency-list mental
model:

```
rel_knows (id INTEGER, src INTEGER, dst INTEGER, since DATE)
```

Here `src` REFERENCES `node_person(id)` and `dst` REFERENCES `node_person(id)`.
The table is backed by a `ForeignRelTable` whose scan function fetches rows from
the remote database. The catalog extension queries
`information_schema.table_constraints` to discover the FK relationships:

```sql
SELECT kcu.column_name, ccu.table_name
FROM information_schema.table_constraints tc
JOIN information_schema.key_column_usage kcu
  ON tc.constraint_name = kcu.constraint_name
JOIN information_schema.constraint_column_usage ccu
  ON ccu.constraint_name = tc.constraint_name
WHERE tc.constraint_type = 'FOREIGN KEY'
  AND tc.table_name = 'rel_knows';
```

This produces a scan-driven rel table — the optimizer generates hash joins
between the foreign node tables and the rel table at query time rather than
pre-building CSR columns.

#### `csr_rel_*` — CSR-style Rel Table

A `csr_rel_*` table signals the intent to **materialize the remote data into a
local on-disk CSR rel table** during `ATTACH` (§3.4). Rather than scanning the
foreign table at every query, the data would be bulk-copied once into Ladybug's
native CSR storage format. This is not yet implemented — the catalog currently
registers `csr_rel_*` tables through the same scan-driven path as `rel_*`
tables.

### 2.2. Initialization Order

Node tables must be registered **before** rel tables because `rel_*` and
`csr_rel_*` entries need to resolve their `src`/`dst` node table IDs at catalog
initialization time. The catalog extension performs a two-pass initialization:

```cpp
// First pass: register node tables
for (auto& table : tables) {
    if (lowerName.rfind("node_", 0) == 0) {
        createForeignNodeTable(tableName);
    }
}
// Second pass: register rel tables (need node table IDs)
for (auto& table : tables) {
    if (lowerName.rfind("rel_", 0) == 0) {
        createForeignRelTable(tableName);  // FK-driven, scan-based
    } else if (lowerName.rfind("csr_rel_", 0) == 0) {
        // TODO: COPY data into a local on-disk CSR RelTable
        createForeignRelTable(tableName);
    }
}
```

## 3. Alternatives & Design Trade-offs

There are several possible strategies for combining foreign SQL tables with
Ladybug's native graph engine. Each has different engineering costs, query
performance characteristics, and levels of planner integration.

### 3.1. Read Node/Rel Tables into Arrow Memory, Run Cypher Locally

The simplest approach: stream all rows from the foreign tables into Arrow
columnar batches, then run Cypher queries entirely inside Ladybug's native
engine. This is the default path for the DuckDB scan function — `LOAD FROM`
and `MATCH` on a single foreign node table pull data through the
`TableFunction` interface into Arrow vectors, which are then processed by the
standard Cypher operator pipeline.

**Pros:**
- Full Cypher expressiveness (filters, projections, graph patterns)
- No planner changes needed for single-table scans
- Works with any foreign database

**Cons:**
- High data movement cost — every query materializes the full column data
- No pushdown of filters or projections to the foreign database
- Scales poorly with large tables (edges must all be moved into memory)

### 3.2. Mixed Tables: Foreign Node Tables + Foreign Rel Tables (Shipped Design)

This is the **primary shipped design**. When attaching a foreign database, the
catalog extension creates **two catalog entries** for each foreign table:

1. An **attached catalog entry** (e.g., `PgClientTableCatalogEntry`) in the
   extension's own catalog set — this holds the scan function and bind data.
2. A **shadow entry** in the main Ladybug catalog (a
   `NodeTableCatalogEntry` constructed with the `ShadowTag{}` marker) — this
   makes the table visible to the Cypher query planner and binder.

The shadow entry holds a pointer (`referencedEntry`) back to the attached
entry, so the planner can find the scan function when it needs to read data:

```cpp
// From PgClientCatalog::createForeignNodeTable
auto mainTableEntry = std::make_unique<catalog::NodeTableCatalogEntry>(
    tableName, pkName, foreignDatabaseName, catalog::ShadowTag{});
mainTableEntry->setReferencedEntry(attachedEntryPtr);
context_->getDatabase()->getCatalog()->addTableEntry(std::move(mainTableEntry));
```

For rel tables, a `RelGroupCatalogEntry` is created in the main catalog so
`MATCH ... -[...]-> ...` patterns can find the relationship. The rel table is
backed by a `ForeignRelTable` (which extends `RelTable`) and uses the foreign
scan function directly, mixing foreign node table data (read via scan function)
with the foreign rel table (also read via scan function). Both are resolved at
query time through hash joins.

**The shadow mechanism** does mean there are two catalog entries for the same
logical table — one in the extension's catalog and one as a shadow in the main
catalog. The planner resolves the shadow to find the scan function, then
delegates to the attached entry for actual data access. This allows Cypher
queries to reference foreign tables transparently without the core planner
needing to know about each extension's catalog layout.

### 3.3. Transpile Graph Traversal into SQL Joins

The `ForeignJoinPushDownOptimizer` attempts to detect Cypher graph patterns
where **all** nodes and relationships are backed by foreign tables from the
**same** external database, and rewrite the entire pattern into a single SQL
`JOIN` query that is pushed down to the foreign database.

The optimizer detects a pattern like:

```
HASH_JOIN (c._ID)
  ├── [FLATTEN]
  │     └── HASH_JOIN (a._ID)
  │           ├── EXTEND (a)-[b]->(c)
  │           │     └── SCAN_NODE_TABLE (a)
  │           └── TABLE_FUNCTION_CALL (a's SQL scan)
  └── TABLE_FUNCTION_CALL (c's SQL scan)
```

And rewrites it into a single `TABLE_FUNCTION_CALL` that executes one SQL JOIN
query on the foreign database.

**Requirements for the rewrite:**
1. Both node scans must use `TABLE_FUNCTION_CALL` with `supportsPushDown = true`
2. The rel table must have a `scanFunction` (foreign-backed)
3. All three tables must be from the **same** foreign database

**Current status:** This optimizer exists (`ForeignJoinPushDownOptimizer`) but
is not yet producing optimal SQL for complex graph traversals. The Ladybug
planner does not currently optimize general graph traversal patterns into
recursive SQL joins — the pushdown is limited to simple star patterns.
Optimizing arbitrary graph traversals (e.g., variable-length path patterns,
multi-hop traversals) into efficient SQL remains open work.

### 3.4. Copy `csr_rel_*` Tables into the Ladybug Catalog at Attach Time

For `csr_rel_*` tables (CSR-style), another strategy is to **materialize the remote
data into a local on-disk CSR rel table** during `ATTACH`. Rather than scanning
the foreign table at every query, the data is bulk-copied once into Ladybug's
native CSR storage format. Subsequent queries run entirely against the local
copy with full performance (CSR adjacency-list traversal, columnar scans,
compression).

This approach is noted in the source as a TODO:

```cpp
} else if (lowerName.rfind("csr_rel_", 0) == 0) {
    // CSR-based rel table: materialized into a local on-disk CSR rel table.
    // TODO: COPY data from PostgreSQL into a local RelTable.
    createForeignRelTable(tableName);
}
```

Once materialized, the local rel table can be combined with foreign node tables
(the **mixed table** approach from §3.2) — node data is still read from the
foreign database via scan functions, but edge traversal uses the local CSR
storage, giving fast extend operations without moving all edge data on every
query.

**Pros:**
- One-time copy cost at attach time vs. per-query data movement
- Full native CSR performance for graph traversals
- Works with databases that have no rowid mechanism

**Cons:**
- Stale data: the local copy is a snapshot. Writes to the foreign database
  after attach are invisible until re-attach.
- Storage cost: edges are duplicated locally.
- Copy time for very large rel tables.

## 4. Rowid Challenges

A critical challenge when representing graphs in foreign SQL databases is the
**internal node identifier** — Ladybug's native storage uses a compact `offset`
within each table as the internal `_ID` of a node. When the data lives in a
foreign database, there is no universal mechanism to get a cheap internal ID.

### 4.1. DuckDB: Rowid Support

DuckDB exposes a `rowid` pseudo-column for each table (unless the table is a
view or has a WITHOUT ROWID-like structure). The DuckDB scan function uses
`rowid` as the internal `_ID`:

```cpp
// From DuckDBTableCatalogEntry::getBoundScanInfo
if (!nodeUniqueName.empty()) {
    auto idUniqueName = nodeUniqueName + "." + std::string(common::InternalKeyword::ID);
    columns.push_back(std::make_shared<binder::VariableExpression>(
        common::LogicalType::INT64(), idUniqueName, "rowid"));
}
```

This works well because DuckDB's `rowid` is a stable 64-bit identifier for each
row that can be used as the node's internal offset. The `internalID_t` struct
pairs this `offset` with a `tableID` to form a globally unique node reference:

```cpp
struct LBUG_API internalID_t {
    offset_t offset;   // rowid from the foreign database
    table_id_t tableID; // Ladybug table ID (shadow entry's ID)
};
```

### 4.2. PostgreSQL: No Rowid

PostgreSQL does **not** have a stable `rowid` analogue. The `ctid` system column
is a physical location (`(page, tuple index)`) that changes on `VACUUM` or row
update and is not a stable identifier. There is no `rowid` pseudo-column.

For the PostgreSQL connector (`pg_client` extension), the workaround is to use
the **primary key** column of the node table as the internal ID. The first
column of the table is convention-bound to be the primary key:

```cpp
// From PgClientCatalog::createForeignNodeTable
auto columnInfo = getTableColumnInfoFromConnector(connector, catalogName,
    defaultSchemaName, tableName);
if (columnInfo.empty()) return;
std::string pkName = columnInfo[0].name;
// ... use pkName as the internal identifier
```

This imposes a constraint: **every `node_*` table in PostgreSQL must have a
numeric primary key** (typically `INTEGER` or `BIGINT`) as its first column.
String or composite primary keys would not work as internal IDs because Ladybug
expects `INT64` offsets.

### 4.3. ADBC: Delegated to the Driver

ADBC (Arrow Database Connectivity) delegates the rowid question to the
underlying driver. The ADBC extension uses the primary key column as the
internal ID, similar to the PostgreSQL approach, but relies on the ADBC driver
to provide a stable ordering.

### 4.4. Implications for Rel Tables

The absence of stable rowids affects how rel tables reference nodes. For
`rel_*` tables, the `src` / `dst` columns must contain values that match the
internal ID scheme of the corresponding node tables:

- **DuckDB**: `src` and `dst` should contain `rowid` values from the node tables
- **PostgreSQL**: `src` and `dst` should contain primary key values from the
  node tables

The FK discovery code in `PgClientCatalog::createForeignRelTable` handles this
by looking up the referenced tables and using their catalog entry IDs, ensuring
the rel table's `srcTableID` / `dstTableID` match the entries that `MATCH`
queries will resolve to:

```cpp
// Look up src/dst node tables in the attached catalog
auto* srcEntry = tables->getEntry(&transaction::DUMMY_TRANSACTION, srcTableName);
auto* dstEntry = tables->getEntry(&transaction::DUMMY_TRANSACTION, dstTableName);
common::table_id_t srcTableID = srcEntry->cast<catalog::TableCatalogEntry>().getTableID();
common::table_id_t dstTableID = dstEntry->cast<catalog::TableCatalogEntry>().getTableID();
```

## 5. ForeignRelTable Implementation

The `ForeignRelTable` class (in `src/include/storage/table/foreign_rel_table.h`)
is the storage-layer adapter that bridges a foreign rel table into Ladybug's
native rel table abstraction. It extends `RelTable` but overrides the scan
path to use the external scan function instead of native CSR columns.

Key design points:

- **Scan-driven**: Unlike native rel tables which read pre-built CSR adjacency
  lists from disk, `ForeignRelTable` delegates to a `TableFunction` that issues
  SQL queries to the foreign database.

- **No CSR columns**: The `setToTable` override skips the CSR-column resolution
  that native `RelTable` performs (which would dereference an empty
  `directedRelData` and throw). All column pointers are set to `nullptr`.

- **No mutations**: `insert`, `update`, and `delete_` all throw
  `RuntimeException`.

- **Morsel-driven parallelism**: The scan function's shared state is lazily
  created on the first `initScanState` call and shared across all worker
  threads. Each worker acquires disjoint offset ranges (morsels) from the
  foreign table via the scan function.

- **Owns its own scan state**: `ForeignRelTableScanState` holds a local
  `dataChunk`, a `sharedState`, and a `localState` — all managed by the
  scan function's lifecycle.

```cpp
class ForeignRelTable final : public RelTable {
public:
    ForeignRelTable(catalog::RelGroupCatalogEntry* relGroupEntry,
        common::table_id_t fromTableID, common::table_id_t toTableID,
        const StorageManager* storageManager, MemoryManager* memoryManager,
        function::TableFunction scanFunction,
        std::shared_ptr<function::TableFuncBindData> scanBindData);
    // scanInternal delegates to scanFunction->match -> initSharedState -> initLocalState -> ...
};
```

## 6. Summary

| Approach                    | Node Data | Rel Data      | Planner Integration  | Freshness | Performance |
|-----------------------------|-----------|---------------|----------------------|-----------|-------------|
| All in Arrow memory         | Foreign   | Foreign       | Minimal              | Per-query | Low (full scan) |
| Foreign nodes + foreign rels| Foreign   | Foreign       | Mixed (shadow)       | Per-query | Medium (hash joins) |
| SQL transpilation           | Foreign   | Foreign       | Full pushdown        | Per-query | High (in-db joins) |
| Copy rels at attach         | Foreign   | **Copied**    | Mixed (shadow)       | Snapshot  | High (CSR traversals) |

The **mixed tables** approach (foreign node tables shadowed in the main catalog
+ foreign rel tables backed by `ForeignRelTable`) is Ladybug's shipped design as
of this writing. The `ForeignJoinPushDownOptimizer` represents a path toward
full SQL transpilation, and the `rel_*` COPY strategy offers a middle ground
for deployments that want fast CSR traversal without sacrificing relational
storage for edges.