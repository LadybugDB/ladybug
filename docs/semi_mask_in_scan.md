# Semi Masks and Node Table Scanning in Ladybug

This document explains how `scan_node_table.cpp` interacts with semi masks, the key data structures involved, and how this optimization reduces disk I/O in hash join scenarios.

## Core Data Structure Concepts

### 1. Nodes
A **node** represents an entity in the graph database. Each node has:
- A unique **node ID** (`nodeID_t`) consisting of:
  - `tableID`: The table the node belongs to
  - `offset`: The position of the node in the table

Nodes are stored in node tables, which are the primary storage unit for graph entities.

### 2. Node Groups
A **node group** is a physical storage unit which enables horizontal partitioning by storing a fixed contiguous range of nodes. Key characteristics:
- Contains a fixed number of nodes (typically determined by `NODE_GROUP_SIZE_LOG2`)
- Each node group is identified by a `node_group_idx_t` (0-indexed)
- Node offsets within a node group: `global_offset = node_group_idx * NODE_GROUP_SIZE + offset_in_group`
- Multiple node groups form the complete node table

The node table in `scan_node_table.cpp` iterates over node groups (lines 79-90):
```cpp
if (currentCommittedGroupIdx < numCommittedNodeGroups) {
    nodeScanState.nodeGroupIdx = currentCommittedGroupIdx++;
    // ... scan this node group
}
```

### 3. Column Chunk (ColumnChunkData)
A **column chunk** stores all values of a single column for a node group:
- One column chunk per property column per node group
- Contains the actual data values (e.g., INT64, STRING, etc.)
- Optionally contains null bitmap data
- Uses compression for efficient storage

### 4. Data Chunk
A **data chunk** is the in-memory representation of results during query execution:
- Contains multiple **value vectors** (one per column being scanned)
- Has a selection vector that indicates which rows are valid/selected
- Size is typically `DEFAULT_VECTOR_CAPACITY` (2048 rows)

From `data_chunk.h`:
```cpp
// A DataChunk represents tuples as a set of value vectors and a selector array.
class DataChunk {
    std::vector<std::shared_ptr<ValueVector>> valueVectors;
    std::shared_ptr<DataChunkState> state;
};
```

### 5. Value Vector
A **value vector** holds values of a single column:
- Fixed capacity (1 for sequences, `DEFAULT_VECTOR_CAPACITY` for general use)
- Contains:
  - `valueBuffer`: Raw data buffer
  - `nullMask`: Bitmap indicating null values
  - `state`: Shared state including selection vector

From `value_vector.h`:
```cpp
class ValueVector {
    LogicalType dataType;
    std::shared_ptr<DataChunkState> state;
    std::unique_ptr<uint8_t[]> valueBuffer;
    NullMask nullMask;
};
```

## Semi Masks

### What is a Semi Mask?
A **semi mask** is a bitmap that tracks which node offsets are "interesting" for a given query. It's used to filter node table scans to only return relevant nodes, avoiding unnecessary disk I/O.

Key interfaces (from `mask.h`):
```cpp
class SemiMask {
    virtual void mask(offset_t nodeOffset) = 0;           // Mark a node as interesting
    virtual void maskRange(offset_t start, offset_t end) = 0;  // Mark a range
    virtual bool isMasked(offset_t offset) = 0;        // Check if masked
    virtual offset_vec_t range(uint32_t start, uint32_t end) = 0;  // Get masked offsets in range
};
```

### Implementation: Roaring Bitmaps
Semi masks are implemented using **Roaring Bitmaps** for memory efficiency:
- `Roaring32BitmapSemiMask`: For tables with ≤ 2^32 nodes
- `Roaring64BitmapSemiMask`: For larger tables
- These provide compressed bitmap storage with fast operations

## How Semi Masks Work with Hash Joins

### Build Side and Probe Side
In a **hash join**, there are two sides:
- **Build side**: The smaller table that gets hashed into a hash table
- **Probe side**: The larger table being probed against the hash table

### Semi Mask Flow in Hash Joins

1. **Build to Probe SIP (Semi-side Information Passing)**:
   - Build side is scanned first
   - Nodes that match join keys are recorded in the semi mask
   - When probing, only masked nodes need to be checked

2. **Probe to Build SIP**:
   - Probe side is scanned first
   - Build side uses semi mask to filter what needs to be looked up

From `acc_hash_join_optimizer.cpp`:
```cpp
// Try build to probe SIP first
if (tryBuildToProbeHJSIP(op)) {
    // Semi mask is applied on build side
    sipInfo.direction = SIPDirection::BUILD_TO_PROBE;
}
// If that fails, try probe to build
tryProbeToBuildHJSIP(op);
```

## Reducing Disk I/O: The Key Optimization

### Without Semi Masks
When scanning a node table during hash join probing:
- **Every node group must be read from disk** (unless filtered by other predicates)
- Even if only 1% of nodes match the join condition, 100% of data is read

### With Semi Masks
The optimization works as follows:

1. **Mask Population Phase** (`semi_masker.cpp`):
   - A SemiMasker operator runs on one side of the join
   - It iterates over the result tuples and masks the node IDs:
   ```cpp
   bool SingleTableSemiMasker::getNextTuplesInternal(ExecutionContext* context) {
       // ... get child tuples ...
       for (auto i = 0u; i < selVector.getSelSize(); i++) {
           auto nodeID = keyVector->getValue<nodeID_t>(pos);
           localState->maskSingleTable(nodeID.offset);  // Mark this node
       }
   }
   ```

2. **Scan with Filter** (`node_group.cpp`):
   - During node table scan, the semi mask is applied:
   ```cpp
   bool enableSemiMask = state.source == TableScanSource::COMMITTED 
                      && state.semiMask 
                      && state.semiMask->isEnabled();
   if (enableSemiMask) {
       applySemiMaskFilter(state, numRowsToScan, state.outState->getSelVectorUnsafe());
       // Only masked rows are kept in the selection vector
   }
   ```

3. **The Filter Logic** (`node_group.cpp`):
   ```cpp
   void applySemiMaskFilter(const TableScanState& state, row_idx_t numRowsToScan,
       SelectionVector& selVector) {
       const auto& arr = state.semiMask->range(startNodeOffset, endNodeOffset);
       // Keep only offsets that are in the semi mask
       // All other rows are filtered out
   }
   ```

4. **How SelVector Reduces Disk I/O** (`column.cpp`):
   
   The key insight is that the SelVector is used at the **column scan level** to skip reading unnecessary data from disk:
   
   ```cpp
   // From column.cpp - scanSegment function
   if (!resultVector->state || resultVector->state->getSelVector().isUnfiltered()) {
       // Unfiltered: read all values. Unfiltered flag is abused here,
       // it actually means continuous range of rows from 0
       columnReadWriter->readCompressedValuesToVector(...);
   } else {
       // Filtered: only read values at positions in selVector
       columnReadWriter->readCompressedValuesToVector(...,
           Filterer{resultVector->state->getSelVector(), offsetInVector});
   }
   ```
   
   The `Filterer` class (lines 229-249 in `column.cpp`) uses the SelVector to determine which rows to read:
   ```cpp
   struct Filterer {
       bool operator()(offset_t startIdx, offset_t endIdx) {
           // Only return true if there's a selVector position in this range
           return (posInSelVector < selVector.getSelSize() &&
                   isInRange(selVector[posInSelVector] - offsetInVector, startIdx, endIdx));
       }
   };
   ```
   
   This means:
   - **Without SelVector**: Each column segment is decompressed/read and then filters are applied
   - **With SelVector**: Column segments are analyzed, and only segments containing valid positions are decompressed/read
   
   The compression layer (e.g., RLE, bit-packing) can skip entire blocks when no positions in the SelVector fall within that block.

### I/O Reduction Example
Consider a query finding all friends of a specific user:
- **Without semi mask**: Scan entire Person node table (millions of rows)
- **With semi mask**: 
  1. First scan the `knows` relationship table to find all friend node IDs
  2. Build a semi mask with those node IDs
  3. When scanning Person table, only load chunks containing masked nodes

This can reduce I/O by orders of magnitude when the join result is much smaller than the table.

## Integration in scan_node_table.cpp

The scan operator integrates semi masks at line 137:
```cpp
void ScanNodeTable::initCurrentTable(ExecutionContext* context) {
    // ... 
    scanState->semiMask = sharedStates[currentTableIdx]->getSemiMask();
}
```

And retrieves them for the hash join (lines 93-100):
```cpp
table_id_map_t<SemiMask*> ScanNodeTable::getSemiMasks() const {
    for (auto i = 0u; i < sharedStates.size(); ++i) {
        result.insert({tableInfos[i].table->getTableID(), sharedStates[i]->getSemiMask()});
    }
    return result;
}
```

## Local Tables and Semi Masks

Ladybug distinguishes between two sources of data when scanning node tables:

1. **Committed data** (`TableScanSource::COMMITTED`): Data that has been persisted to disk
2. **Uncommitted data** (`TableScanSource::UNCOMMITTED`): In-memory data from the current transaction (local storage)

### How Local Tables Work

During a write transaction, new or modified nodes are first stored in the **local storage** (in-memory):
- Located in `LocalNodeTable` 
- Organized into local node groups
- Eventually flushed to disk on commit

From `scan_node_table.cpp` (lines 64-70):
```cpp
if (transaction->isWriteTransaction()) {
    if (const auto localTable =
            transaction->getLocalStorage()->getLocalTable(this->table->getTableID())) {
        auto& localNodeTable = localTable->cast<LocalNodeTable>();
        this->numUnCommittedNodeGroups = localNodeTable.getNumNodeGroups();
    }
}
```

The scanner processes committed node groups first, then uncommitted ones:
```cpp
if (currentCommittedGroupIdx < numCommittedNodeGroups) {
    nodeScanState.nodeGroupIdx = currentCommittedGroupIdx++;
    nodeScanState.source = TableScanSource::COMMITTED;
    // ... scan from disk
}
if (currentUnCommittedGroupIdx < numUnCommittedNodeGroups) {
    nodeScanState.nodeGroupIdx = currentUnCommittedGroupIdx++;
    nodeScanState.source = TableScanSource::UNCOMMITTED;
    // ... scan from local storage
}
```

### Semi Masks and Disk Block Skipping

**Critical insight**: Semi masks are ONLY applied when scanning COMMITTED (disk) data:

From `node_group.cpp` (lines 238-239 and 256-257):
```cpp
bool enableSemiMask =
    state.source == TableScanSource::COMMITTED && state.semiMask && state.semiMask->isEnabled();
```

This is an important design decision:
- **Disk blocks**: Can skip reading entire blocks using semi masks (huge I/O savings)
- **Local storage**: Always scanned in full (in-memory, so typically small)

### Why Semi Masks Don't Apply to Local Storage

The SelVector is used extensively in local storage operations, but not for semi-mask filtering:

1. **Local table scanning uses SelVector for different purposes** (`local_rel_table.cpp`):
   ```cpp
   // Setting up which rows to scan from local storage
   localScanState.rowIdxVector->state->getSelVectorUnsafe().setSelSize(numToScan);
   
   // For intersection operations in local storage
   scanChunk.state->getSelVectorUnsafe().setSelSize(intersectRows.size());
   ```

2. **Local storage is accessed via lookup operations**:
   - Local storage (`LocalNodeTable`, `LocalRelTable`) uses direct lookups rather than full scans
   - The data is already in memory, so selective reading provides less benefit
   - Operations like inserts/deletes use the SelVector to identify specific rows to modify

3. **No semi mask application in local path**:
   - From `node_table.cpp` lines 271-276, when `source == TableScanSource::UNCOMMITTED`, the local NodeGroup is retrieved directly
   - The semi mask check in `node_group.cpp` explicitly requires `COMMITTED` source
   - This means the SelVector filtering path is never triggered for local data

4. **Local storage lookup pattern** (`local_rel_table.cpp` line 234-235):
   ```cpp
   [[maybe_unused]] auto lookupRes =
       localNodeGroup->lookupMultiple(transaction, localScanState);
   ```
   This uses direct lookups rather than filtered scans.

### Why This Design?

1. **Disk I/O is the bottleneck**: Disk reads are orders of magnitude slower than memory access, so optimizing disk reads provides the biggest benefit

2. **Local tables are typically small**: In a well-designed workload, uncommitted data is a small fraction of the table

3. **Simplified consistency**: Semi masks are built from join results which themselves come from various sources; applying them consistently to local storage would require additional complexity

4. **Post-commit optimization**: After the transaction commits, uncommitted data becomes committed and future queries can benefit from semi mask filtering

### Practical Impact

When executing a hash join in a write transaction:
1. **Build side** may include both committed and uncommitted nodes
2. **Probe side** benefits from semi mask filtering when scanning committed (disk) data
3. Uncommitted local data is scanned without filtering (but typically small)

This design maximizes I/O savings where they matter most while keeping the implementation straightforward.

## Summary

Semi masks provide a crucial optimization for graph database queries involving joins:

1. **Concept**: Bitmap tracking which nodes are relevant to the query
2. **Storage**: Roaring bitmaps for memory efficiency
3. **Hash Join Integration**: Built side passes information to probe side (or vice versa)
4. **SelVector Conversion**: Semi mask is converted to a SelVector in `applySemiMaskFilter()` which specifies which row positions are valid
5. **I/O Reduction**: Column scan uses the SelVector to skip reading disk blocks that contain no relevant data
6. **Local Tables**: Semi masks only apply to committed (disk) data; local storage uses direct lookups and is always scanned (but typically small)

This is especially impactful in graph workloads where pattern matching often involves finding small subsets of large node tables.
