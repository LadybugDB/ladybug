# Design: opaque partition-routing hook for remote partition subgraphs

## Goal

Partitioned node tables store their rows in partition subgraphs (`<parent>_p<i>` node
tables). Today every subgraph is local. In a distributed deployment a partition may live
on another host. We want ladybugdb to stay **embedded and distribution-agnostic**: the
core never learns about hosts, sockets, or serialization. Instead it exposes one opaque
interface that a *distributed wrapper* installs at startup; every place the engine would
touch a partition subgraph consults the interface first, and falls back to local storage
when the interface is absent (the default, and the behavior of every existing test).

## Inventory: every embedded "subgraph call" that needs a seam

| # | Seam | Location | What happens today |
|---|------|----------|--------------------|
| 1 | Partition lifecycle (create/drop/rename subgraphs, subgraph registration) | `src/catalog/catalog.cpp:179-243, 615-669` | Parent DDL creates/drops/renames `<parent>_p<i>` node-table subgraphs |
| 2 | Local storage creation for partitions | `src/storage/storage_manager.cpp:271` | A `NodeTable` is created per partition subgraph |
| 3 | Read binding: expand parent → partitions | `expandPartitionedNodeTables`, `src/binder/bind/bind_graph_pattern.cpp:779-860` | Pattern on a partitioned parent is rewritten into a multi-table scan over all child entries |
| 4 | Rel endpoint binding: parent → n×m FROM/TO pairs | `resolveRelEndpoints`, `src/binder/bind/bind_ddl.cpp:236-255` | Rel tables attach to each partition subgraph |
| 5 | Point-write routing (INSERT/SET/MERGE) | `NodeInsertExecutor::resolveTargetTable` / `resolveTableForNodeID`, `src/processor/operator/persistent/insert_executor.cpp:95-116` | `computePartitionIndexes` picks the child table for the evaluated key |
| 6 | Bulk-write routing (INSERT ... FROM / COPY FROM) | `NodeBatchInsert` targets + `computePartitionIndexes`, `src/processor/operator/persistent/node_batch_insert.cpp:~500-680`; binder carries `NodePartitionWriteInfo` (`bind_copy_from.cpp:182-193`) | Rows are hash/range-routed into per-partition targets |

Everything else (planner, processor, WAL, GDS `graph::Graph`) is already subgraph-blind
or consumes the same bound entries, so these six seams are the complete surface.

## The interface

One plain struct of plain function pointers plus an opaque context handle — no virtual
inheritance. `nullptr` members mean "handle locally", so the wrapper only overrides what
it owns. See `src/include/common/partition_routing_hook.h` for the authoritative
definition; summary:

| Hook | Seam | Contract |
|------|------|----------|
| `locate(ctx, ref, &handle)` | placement | Called before any touch of partition `ref`. true + handle = wrapper owns it (remote); false = local. Must be consistent; answers are cached. |
| `onPartitionCreate(ctx, ref, handle)` | lifecycle | Fires for **every** partition creation — this is how a wrapper learns about new partitions and decides placement. |
| `onPartitionDrop(ctx, ref, handle)` | lifecycle | Fires for claimed partitions when their subgraph entry is dropped. Renames are not reported (`PartitionRef` is ID-based and IDs survive renames). |
| `bindScan(ctx, ref, handle, &spec)` | reads (bind time) | Wrapper fills a `PartitionScanSpec`: its table function + a bind-data factory keyed by the node's unique expression name. The engine attaches the spec to an internal clone of the partition's catalog entry (preserving schema, table ID, lineage). Bind columns must be named `<nodeUniqueName>.<prop>` / `<nodeUniqueName>._ID` (same convention as extension foreign tables). |
| `insertRow(ctx, ref, handle, tx, keyVec, colVecs)` | point writes | Single already-evaluated row; wrapper ships it and returns the remotely-assigned nodeID. |
| `insertChunk(ctx, ref, handle, tx, keyVec, colVecs, startRow, numRows)` | bulk writes | Run of rows; row j lives at selection position `selVector[startRow + j]` (same convention as `InMemChunkedNodeGroup::append`). |
| `lookupRow(ctx, ref, handle, tx, nodeID, outVecs)` | MERGE lookups | Fetch an existing remote row into output vectors. |

Registration is process-global (`setPartitionRoutingHooks`), must happen before the first
Database is opened, and the hooks object must outlive the registration. `nullptr` hooks
(default) preserve embedded behavior bit-for-bit.

### Registration and lifetime

- `main::Database` gains `setPartitionRouting(const PartitionRoutingHooks*)`, callable
  only between construction and the first query / recovery start. The pointer is then
  immutable and copied into each `ClientContext` (read path) and handed to `Catalog` /
  `StorageManager` (DDL path) — no locks on the hot path.
- Default is `nullptr` everywhere: an un-hooked build behaves bit-for-bit as today.
- The wrapper registers its own scan/insert table functions with the normal function
  registry before opening the database, so `bindScan` only needs to name them.

## How each seam changes

1. **Catalog lifecycle** — `createNodeTableSubgraph` / drop / rename paths call
   `onPartition*` after (or instead of, when `locate` claims the partition) the local
   catalog mutation. The catalog metadata (child table IDs, partition method, key
   column) is *always* recorded locally: it is the distributed system's source of truth
   for placement, and it survives restarts so `locate` can be re-consulted.
2. **Storage manager** — when creating tables for partition children
   (`storage_manager.cpp:271`), skip local storage for partitions claimed by `locate`.
   No local files, no WAL records, no checkpoint work for them. (Checkpoint/replay must
   consult `locate` before assuming a child table has local state — the one place the
   recovery path needs the hook.)
3. **Read binding** — in `expandPartitionedNodeTables`, a claimed partition contributes
   the wrapper's scan function entry instead of the local child entry; the existing
   multi-table union scan handles the mix of local and remote partitions unchanged.
4. **Rel endpoints** — `resolveRelEndpoints` does the same substitution. Note: for a
   rel table between two remote-partitioned parents this expands to n×m pairs; if that
   becomes a problem, the optional escape hatch is a second hook that lets the wrapper
   bind the whole rel table at once, but start without it.
5. **Point writes** — `resolveTargetTable()` first computes the partition index (core
   logic, unchanged), then consults `locate`; claimed partitions go through
   `insertRow` and the returned nodeID flows into the existing output-vector path.
6. **Bulk writes** — `NodeBatchInsert` keeps `computePartitionIndexes` as-is, then
   partitions each key chunk's selection into local targets (existing code path) and
   remote targets (one `insertChunk` call per claimed partition per chunk).

## Invariants the core keeps (why this stays "no distribution knowledge")

- The engine owns the **partition function** (hash/range over the key column). Placement
  is therefore computable anywhere without RPC; the wrapper only owns *where* a
  partition lives, never *which* partition a row belongs to.
- The engine owns the **catalog** (parent/child IDs, schemas). Remote partitions are
  first-class catalog entries with local metadata.
- All transport concerns — hosts, connections, serialization, retries, pushing down
  predicates — live behind `PartitionHandle` in the wrapper. Ladybug passes the handle
  back verbatim and never inspects it.
- Every hook is optional and every callback site falls through to the current local
  code path when unclaimed.

## Alternatives considered

- **`RemoteNodeTable : storage::NodeTable`** — most transparent (no binder/executor
  changes), but drags WAL, checkpoint, versioning, and scan-state internals into the
  public seam, coupling the wrapper to ladybug's storage ABI release-to-release.
- **Subclass `graph::Graph`** — covers only the GDS neighbor-scan interface, not
  INSERT/COPY/DDL, which are the majority of subgraph touch points.
- **Distributed planning in the core** — the thing we explicitly do not want.

## Implementation notes / deltas from the first sketch

- `bindScan` hands over a `PartitionScanSpec` (function + bind-data factory) instead of a
  catalog entry: the engine clones the partition's own catalog entry and stamps the
  wrapper's scan onto it, so schema/table-ID/lineage stay consistent and write paths can
  resolve the parent from catalog truth even when pattern entries are substituted.
- `NodeTableCatalogEntry::CreateBindDataFunc` now receives `nodeUniqueName` so
  foreign-backed entries can name their output columns the way the planner expects
  (mirrors what the duckdb/postgres extensions do inside their own `getBoundScanInfo`).
- `NodePartitionWriteInfo` carries the parent table ID so executors can build
  `PartitionRef`s without re-deriving lineage.
- Mixed local/remote scans of one parent are rejected at bind time (the multi-entry
  `ScanNodeTable` union cannot host scan-function-backed entries); fully-claimed parents
  collapse to a single substitute entry and use the existing table-function scan path.
- Checkpoint, metadata-snapshot serialization, rollback, and storage creation all skip
  claimed partitions (no local table/WAL/checkpoint state exists for them).
- Not wired in this first landing (documented limitations): UPDATE/DELETE on remote rows,
  rel tables referencing remote-partitioned parents, GDS algorithms over remote
  partitions, and direct writes to individual remote partition subgraphs by name.

## Suggested landing order
