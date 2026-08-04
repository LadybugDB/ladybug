# PostgreSQL-style Table Partitioning

Ladybug now supports declarative table partitioning modeled on
[PostgreSQL's partitioning](https://www.postgresql.org/docs/current/ddl-partitioning.html):
a **logical (parent) node table** is split into several **partitions**, and each partition is
backed by its own node-table **subgraph**. This page describes the feature, its architecture,
current limitations, and the roadmap (including remote partitions accessed over a columnar
protocol such as ADBC).

> Status: **v1 (foundational).** DDL, catalog, storage, persistence, drop-cascade and query
> (read over all partitions) are implemented. Write-routing (COPY / CREATE into the parent) and
> predicate-based partition pruning are designed below but not yet wired in.

## Why subgraph-per-partition

The partitioning model is deliberately "one physical table per partition":

* It reuses the existing columnar node-table storage, catalog, WAL, checkpointer and serialization
  paths untouched — each partition is a perfectly ordinary node table.
* It makes the *partition* the unit of independent management. In the future a partition subgraph
  may be:
  * **local** (the default, an in-process node table), or
  * **remote** — accessed over a columnar protocol such as
    [ADBC](https://arrow.apache.org/adbc/) / Arrow Flight, mirroring the existing
    `ForeignRelTable` / scan-function infrastructure (`NodeTableCatalogEntry::getScanFunction`).
  A remote partition subgraph is just another node-table implementation behind the same
  `TableCatalogEntry` interface.
* The parent is a **logical** table: it owns the schema, partition method and partition-key column,
  but has *no physical storage of its own*. All tuples live in the partition subgraphs.

PostgreSQL's own distinction between *declarative* partitioning (a parent + child tables) and
*method* partitions (range/list/hash) maps directly: the parent is the declarative shell, the
children are the method partitions.

## Cypher syntax

```cypher
-- Hash partitioning on an eligible column.
CREATE NODE TABLE Orders (
    id     INT64  PRIMARY KEY,
    region STRING,
    amount INT64
) PARTITION BY HASH (region) PARTITIONS 4;

-- Range partitioning (bounds are derived; see design below).
CREATE NODE TABLE Events (
    id    INT64 PRIMARY KEY,
    ts    TIMESTAMP,
    value DOUBLE
) PARTITION BY RANGE (ts) PARTITIONS 5;
```

The grammar extension lives in `src/antlr4/Cypher.g4`:

```
iC_PartitionBy     : PARTITION SP BY SP ( iC_PartitionRange | iC_PartitionHash ) ;
iC_PartitionHash   : HASH  SP? '(' SP? oC_PropertyKeyName SP? ')' SP PARTITIONS SP oC_IntegerLiteral ;
iC_PartitionRange  : RANGE SP? '(' SP? oC_PropertyKeyName SP? ')' SP PARTITIONS SP oC_IntegerLiteral ;
```

`PARTITION`, `PARTITIONS`, `HASH` and `RANGE` were added to `src/antlr4/keywords.txt` and to the
`iC_NonReservedKeywords` list so they remain usable as identifiers elsewhere.

### Eligible partition columns

A partition column must be a column of the table whose type is orderable/comparable and stable:
integral, floating-point, date/timestamp, string, UUID, or blob. This is enforced at bind time by
`LogicalTypeUtils::isPartitionable` (`src/common/types/types.cpp`). Erroring, for example, on a
LIST/STRUCT/MAP partition key.

## Architecture & pipeline

The feature threads a small amount of partition metadata through the existing DDL pipeline:

| Layer | File(s) | Role |
|-------|---------|------|
| Grammar | `src/antlr4/Cypher.g4`, `keywords.txt` | `PARTITION BY ... PARTITIONS n` |
| Parser AST | `src/include/parser/ddl/create_table_info.h` (`ParsedPartitionInfo`) | parsed clause |
| Transformer | `src/parser/transform/transform_ddl.cpp` (`transformPartitionInfo`) | AST from parse tree |
| Binder | `binder/ddl/bound_create_table_info.h` (`BoundPartitionInfo`), `bind_ddl.cpp` | validation + carry metadata |
| Catalog | `catalog/catalog.cpp` (`createNodeTableEntry`), `node_table_catalog_entry.{h,cpp}` | parent + partition subgraph entries, persistence |
| Storage | `storage/storage_manager.cpp` (`createTable`) | create partition subgraph storage; parent has none |
| Query (read) | `binder/bind/bind_graph_pattern.cpp` (`expandPartitionedNodeTables`) | expand parent label → partition subgraphs |

### Catalog representation

A partitioned parent is a `NodeTableCatalogEntry` whose partition metadata is set via
`setPartitionInfo(...)`:

```cpp
std::optional<binder::BoundPartitionMethod> partitionMethod;  // HASH | RANGE (parent only)
std::string                              partitionColumnName;
common::property_id_t                    partitionColumnID;
uint64_t                                 numPartitions;
std::vector<common::table_id_t>          childTableIDs;       // parent -> its partitions
// Back-reference set on each child partition subgraph:
common::table_id_t                       parentTableID;       // child -> parent
uint64_t                                 partitionIndex;      // child's ordinal
```

* `NodeTableCatalogEntry::isPartitioned()` is true **only** on the logical parent.
* `NodeTableCatalogEntry::isPartitionChild()` is true **only** on a partition subgraph.
* `isParent(tableID)` returns true for a partitioned parent, mirroring rel-groups.

On `CREATE`, `Catalog::createNodeTableEntry`:

1. Creates the parent entry (schema + serial sequence).
2. For each `i in [0, numPartitions)`, creates a child `NodeTableCatalogEntry` subgraph named
   `<parent>_p<i>`, copies the property definitions, sets its back-reference
   (`setParentInfo(parentID, i)`), and registers it in the **same public catalog set** as the
   parent, recording its (normal, small) table-id in `childTableIDs`.

> **Why the public catalog set?** Internal catalog entries carry OIDs near 2^63, and several
> execution/storage structures index state by table-id with a `std::vector`, which would attempt a
> `resize(2^63)` and throw `vector::_M_default_append`. Keeping partitions in the public set gives
> them ordinary small table-ids, so the existing multi-table node-scan machinery works unchanged.

### Storage

`StorageManager::createTable` treats a partitioned parent specially: it creates **no** physical
storage for the parent, and instead creates storage for each child partition subgraph (each is a
plain `NodeTable`). `StorageManager::serialize`/`deserialize` skip the storage-less parents and
serialize the partitions individually (see `std::erase_if(..., isPartitioned())`).

### Dropping a partitioned table

`Catalog::dropTableEntry` cascades: dropping a partitioned parent drops each partition subgraph
(and its serial sequence) before dropping the parent.

### Querying: the parent is a read view over all partitions

`Binder::bindNodeTableEntries` expands any partitioned parent into its partition subgraphs
(`expandPartitionedNodeTables`). Because the query planner already unions over the multiple node
tables of a multi-label scan, `MATCH (n:Orders)` transparently reads across every partition:

```cypher
MATCH (o:Orders) RETURN o.id, o.amount ORDER BY o.id;  -- unions Orders_p0..p3
```

Because each partition is a real node table, you can also address a specific partition directly
(e.g. `MATCH (o:Orders_p2) ...`, `CREATE (o:Orders_p2 {...})`, `COPY Orders_p2 FROM ...`).

## Current limitations / v1 boundaries

* **Write-routing is not implemented.** `COPY INTO <parent>`, `CREATE (n:<parent>)` and
  `MERGE`/`SET` against a partitioned parent currently raise a clear, actionable
  `BinderException` telling you to target the partition subgraphs. Reads on the parent work.
* **No partition pruning on predicates.** A `WHERE` on the partition key is not yet used to skip
  partitions; all partitions are scanned and unioned.
* **No `ALTER` propagation.** Altering the parent schema does not alter its partitions.
* **Range bounds are not declarative.** `PARTITIONS n` builds the range split; per-partition bound
  lists (`PARTITION p0 VALUES < (...), p1 VALUES FROM ... `) are future work.
* **No remote partitions yet.** Only local (in-process) partition subgraphs exist today.

## Roadmap

### 1. Write routing (COPY / CREATE / MERGE)

The canonical path is `COPY INTO Orders FROM file`. Plan:

* Thread partition metadata into `BoundCopyFromInfo`/`NodeBatchInsertInfo` (partition method,
  partition-key column id, and the child `NodeTable` list).
* In `NodeBatchInsert`, evaluate each row's partition-key value, determine its partition index
  (`hash(value) % n` for HASH; compare against range bounds for RANGE — use a shared
  `valueToDouble`-style comparator already used by the histogram/percentile aggregates), and route
  the row into the correct child `NodeTable`'s node group.
* Primary-key duplicate detection becomes per-partition (each child has its own PK index), which
  the current single-table batch-insert already composes that way.

For single-row `CREATE`/`MERGE` with a *literal* partition key, the partition can be resolved at
bind time from the property literal; the general (expression/parameter) case routes at runtime in
the insert operator.

### 2. Partition pruning

Push a predicate on the partition column into the scan: for HASH only equality
(`region = 'east'`) can prune to a single partition; for RANGE relational comparisons
(`ts < '2024-01-01'`) prune to the relevant range(s). This reuses the optimizer's existing
filter-push-down / scan selection (`scan->setNumPartitionsToScan`, etc.) once the partition bounds
are materialized.

### 3. Declarative range bounds

Extend the grammar to accept per-partition bounds:

```cypher
CREATE NODE TABLE Events (...) PARTITION BY RANGE (ts) (
    PARTITION p2023 VALUES < DATE '2024-01-01',
    PARTITION p2024 VALUES >= DATE '2024-01-01' AND < DATE '2025-01-01'
);
```

### 4. Remote partitions over a columnar protocol (ADBC / Arrow Flight)

Each partition subgraph already *is* a `NodeTableCatalogEntry`. A remote partition would be a
flavor whose storage lives on a server:

* Add a partition subgraph variant that carries a `TableFunction` scan + bind-data, exactly like
  the existing foreign-table path (`NodeTableCatalogEntry::getScanFunction`,
  `ForeignRelTable`, `ArrowNodeTable`).
* The parent keeps the same declarative `childTableIDs` list; each entry in that list may point to
  either a local or a remote subgraph.
* `StorageManager::createNodeTable` already branches on `storageFormat`/`storage`/`scanFunction` —
  a remote partition simply selects the ADBC-backed branch.
* `COPY`-routing becomes the same as (1) but writing into remote subgraphs through the protocol's
  ingested-query API.

This gives a clean "pivot from local to remote without changing user Cypher" story — the partition is
the seam, exactly as `ATTACH`/`FOREIGN TABLE` already is for whole databases.
