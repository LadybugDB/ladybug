# PostgreSQL-style Table Partitioning

Ladybug now supports declarative table partitioning modeled on
[PostgreSQL's partitioning](https://www.postgresql.org/docs/current/ddl-partitioning.html):
a **logical (parent) node table** is split into several **partitions**, and each partition is
backed by its own node-table **subgraph**. This page describes the feature, its architecture,
current limitations, and the roadmap (including remote partitions accessed over a columnar
protocol such as ADBC).

> Status: **v1 (foundational).** DDL, catalog, storage, persistence, drop-cascade and query
> (read over all partitions) are implemented. Write-routing (COPY / CREATE / MERGE into the
> parent) is implemented for HASH partitioning. **`PARTITION BY RANGE` is refused at DDL time**:
> real range partitioning must derive partition bounds from the actual value distribution, which
> is not implemented yet — refusing beats silently falling back to hash routing. Predicate-based
> partition pruning is still future work.
>
> Invariant-protection implemented in response to design review: partitions cannot be dropped or
> altered individually, `DROP GRAPH` refuses node-table subgraphs, dropping a partitioned table is
> refused while rel tables still reference its partitions, and updates to a partition column are
> rejected (rows cannot move between partitions). See "Design notes" below.

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

## Design notes

Answers to the questions raised while reviewing the partitioning PR.

### (a) The partition/subgraph coupling cannot be broken by accident

Every node table owns a same-named subgraph (`GraphCatalogEntry`), and a partition additionally
carries a back-reference (`parentTableID`, `partitionIndex`) plus a forward link on the parent
(`childTableIDs`). Three guards keep the coupling intact:

* **`DROP GRAPH <partition>` is refused.** Node-table subgraphs are owned by their table, so
  `DROP GRAPH Orders_p0` fails with *"it is a node-table subgraph. Drop the node table instead."*
  (Before this guard, the command failed with a confusing "does not exist", even though `SHOW_GRAPHS`
  lists the subgraph.)
* **`DROP TABLE <partition>` is refused.** Dropping one partition would leave the parent holding a
  dangling `childTableID`, after which every scan of the parent fails with *Cannot find table
  catalog entry*. The error points at the parent: drop the whole partitioned table instead.
* **Renaming the parent renames the partitions.** The `<parent>_p<i>` naming stays consistent
  because `ALTER TABLE ... RENAME` cascades to the children (and their subgraphs). Child renames
  are not separately WAL-logged: replaying the parent's single rename record re-runs the
  child-rename step, and rollback reverts children before the parent, so replay and undo both see
  one consistent transition.

PostgreSQL reaches the same place with different mechanics (it refuses `DROP TABLE` of a parent
with partitions and requires explicit `DETACH PARTITION`). Our v1 equivalent of detach is "drop
the parent".

### (b) Other tables coupled to partitions: refuse, never silently detach

Two kinds of "other tables" can be coupled to a partitioned table:

* **Rel tables.** A rel attaches to node tables by *table ID* through per-pair CSR indexes. A
  partitioned parent owns no storage, so declaring `FROM Orders TO Item` resolves to **one pair per
  partition**: `(Orders_p0, Item), ..., (Orders_p3, Item)`. Rels therefore attach to real storage,
  and reads through the parent work unchanged. Consequently:
  * Dropping the parent is **refused** while any rel still references *any* partition — the same
    check a plain node table has always had, applied to every table the cascade would remove, and
    evaluated before anything is dropped.
  * We do **not** detach-delete relationships automatically. Cascade-dropping dependent rels is
    deliberate future work (see roadmap); until then the error names the blocking rel table and the
    user drops it explicitly.
  * Creating a rel *pattern* against the parent (`MATCH (o:Orders)-[r:R]->(i:Item) CREATE ...`) is
    refused for now — the write must go through a concrete partition (`Orders_p0`). Routing pattern
    writes by the source row's actual partition at runtime is roadmap work; the binder error says so.
  * **Self-relations and cross-partition edges are allowed.** `FROM Orders TO Orders` expands to
    the full cross product `(p_i, p_j)` for all partition pairs, so an edge may connect two nodes
    in different partitions (or the same one); parent-pattern reads see every edge regardless of
    which partitions it spans, and `DETACH DELETE` removes a node's edges in both directions.
    Note the cost: a self-rel on an n-partition table materializes n² rel tables (each with its own
    CSR indexes), which is negligible for typical n but grows quadratically — keep partition counts
    modest on tables with self-relations until pair creation can be made lazy.
* **User graphs.** A `GraphCatalogEntry` carries no table membership — it is only a name marker
  surfaced by `SHOW_GRAPHS` — so nothing else can live "inside" a partition's subgraph.

### (c) Updates to the partition column are refused (no row movement yet)

A row's home partition is decided by `hash(key) % n` at write time. If an update changed the key in
place, the row would sit in a partition that no longer matches its key — invisible to future
partition pruning and inconsistent with direct-partition scans. Ladybug node offsets are also
referenced by rel tables' `INTERNAL_ID`s, so a "move" is really delete + insert with reference
rewiring, which is not implemented today. The binder therefore rejects any SET (including
`MERGE ... ON MATCH SET`) of the partition column — via the parent or via a partition — with the
same actionable guidance used for primary keys: delete the row and insert it with the new value.
PostgreSQL-style row movement remains roadmap work.

### (d) One node-table implementation; "partitioned" is metadata, not a type

Two *roles* exist (logical parent vs physical partition), as in every declarative partitioning
design, but only **one** node-table implementation and one storage path:

* Every node table — plain, partition child, or foreign-backed — is backed by a subgraph and stored
  as a `NodeTable`. Partition children differ from plain tables only by a back-reference field.
* The "partitioned" state of a parent is catalog metadata (`partitionMethod`, key column,
  `childTableIDs`), not a different table type: reads reuse the existing multi-table node scan,
  writes reuse the ordinary insert/batch-insert executors behind a small routing shim
  (`NodePartitionWriteInfo`), and WAL / MVCC / checkpoint run per child exactly as for any table.
* Storage iteration skips storage-less parents through one shared helper
  (`erasePartitionedParents`) instead of bespoke branches at each call site.

What remains intentionally asymmetric (and enforced): the parent owns no storage, accepts writes
only by routing, and propagates only `RENAME` — other `ALTER`s are refused with a clean error until
alter-propagation lands.

## Cypher syntax

```cypher
-- Hash partitioning on an eligible column.
CREATE NODE TABLE Orders (
    id     INT64  PRIMARY KEY,
    region STRING,
    amount INT64
) PARTITION BY HASH (region) PARTITIONS 4;
```

The grammar still accepts `PARTITION BY RANGE (<col>) PARTITIONS n`, but the binder refuses it:
RANGE partitioning is not implemented yet (see the status note above).

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

* **Write-routing.** `COPY INTO <parent>`, `CREATE (n:<parent>)` and `MERGE` against a
  partitioned parent are routed into the partition matching each row's partition-key value. HASH
  partitions use the same value hashing as the built-in `HASH()` function. Primary-key uniqueness
  is enforced per partition, not across the parent.
* **No partition pruning on predicates.** A `WHERE` on the partition key is not yet used to skip
  partitions; all partitions are scanned and unioned.
* **`ALTER` is limited to `RENAME`.** Renaming the parent renames its `<parent>_p<i>` partitions
  along with it. Other alter operations (add/drop/rename property, sorted-by) are refused on the
  parent, and partitions cannot be altered directly at all, until alter propagation lands.
* **Partition columns are not updatable.** See design note (c): rows cannot move between
  partitions; delete and re-insert instead.
* **Rels attach per partition.** `FROM <parent>` expands to one pair per partition; rel pattern
  writes against the parent are refused (use a specific partition) until runtime rel routing lands.
* **RANGE is refused at DDL time.** Meaningful range partitioning needs bounds derived from the
  actual value distribution (declarative bounds or data-driven splitting); a static stand-in would
  misplace rows silently. See the roadmap.
* **No remote partitions yet.** Only local (in-process) partition subgraphs exist today.

## Roadmap

### 1. Write routing (COPY / CREATE / MERGE)

The canonical path is `COPY INTO Orders FROM file`. Implemented:

* `BoundCopyFromInfo`/`NodeBatchInsertInfo` carry the partition method, partition-key column id,
  and the child table list (`common::NodePartitionWriteInfo`).
* `NodeBatchInsert` evaluates each row's partition-key value, computes `hash(value) % n`, and
  routes consecutive same-partition runs into the correct child `NodeTable`'s node group. Each
  child is an ordinary `NodeTable`, so its own WAL/MVCC machinery (`appendToLastNodeGroup` +
  commit/undo records) applies the routed write.
* Primary-key duplicate detection is per partition (each child has its own PK index), matching how
  a direct `COPY` into a partition subgraph behaves.
* Single-row `CREATE`/`MERGE` routes at runtime in `NodeInsertExecutor`: the partition key is
  evaluated, the matching child table is selected, and the row is inserted there.

### 1b. RANGE partitioning (not implemented; DDL refuses it)

Real RANGE needs bounds that reflect the data: either user-declared bounds
(`PARTITION p0 VALUES < (...)`) or dynamic, distribution-aware splitting of the input (min/max or
equi-depth histograms computed during COPY, persisted with the parent). Equal splits of the *type
domain* were considered and rejected: every realistic timestamp lands in one middle bucket.
Until one of those exists, the binder refuses `PARTITION BY RANGE` instead of silently routing by
hash.

### 2. Rel writes against the parent

Route pattern-based rel creation/merge by the source row's actual partition at runtime (mirroring
`NodeInsertExecutor::resolveTargetTable`): carry the candidate per-pair rel tables in the insert
info and select by the resolved source node's table ID.

### 3. Partition pruning

Push a predicate on the partition column into the scan: for HASH only equality
(`region = 'east'`) can prune to a single partition; for RANGE relational comparisons
(`ts < '2024-01-01'`) prune to the relevant range(s). This reuses the optimizer's existing
filter-push-down / scan selection (`scan->setNumPartitionsToScan`, etc.) once the partition bounds
are materialized.

### 4. RANGE partitioning: dynamic, distribution-aware splits

Unblocks `PARTITION BY RANGE` (currently refused at DDL). Two shapes, in increasing ambition:

* **Declarative bounds** — extend the grammar to accept per-partition bounds:
  ```cypher
  CREATE NODE TABLE Events (...) PARTITION BY RANGE (ts) (
      PARTITION p2023 VALUES < DATE '2024-01-01',
      PARTITION p2024 VALUES >= DATE '2024-01-01' AND < DATE '2025-01-01'
  );
  ```
* **Dynamic splitting** — derive bounds from the actual value distribution (min/max or equi-depth
  histograms computed over the COPY input, persisted with the parent; late rows outside the
  learned range go to an overflow partition or trigger resplitting). This is what makes
  `RANGE(ts)` place rows monotonically without asking the user for bounds.

### 5. LIST partitioning (per-distinct-value partitions)

`PARTITION BY LIST (col)` where every distinct key value gets its own partition, created on demand:
100 distinct cluster IDs → 100 partitions. Requires dynamic partition creation at write time
(catalog entry + storage + WAL create record inside the inserting transaction) and a persisted
value→partition map on the parent entry.

### 6. Row movement, ALTER propagation, and DETACH PARTITION

Lift the v1 restrictions in dependency order:

* **ALTER propagation** — apply add/drop/rename property on the parent to every partition (each
  partition is a plain node table, so `NodeTable::addColumn` already does the per-table work; the
  parent alter just fans out to its children's storage).
* **Partition-column updates with row movement** — implement as delete + insert within one
  transaction, rewiring rel references, or keep the refusal if rel rewiring proves impractical.
* **`DETACH PARTITION` / `ATTACH PARTITION`** — split a partition off its parent into a standalone
  table and back, the PostgreSQL-style escape hatch that today is approximated by "drop the
  parent"; also revisit cascade-dropping dependent rels at that point.

### 7. Remote partitions over a columnar protocol (ADBC / Arrow Flight)

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
