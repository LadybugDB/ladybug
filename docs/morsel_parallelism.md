# Morsel-Driven Parallelism in ladybug

## Overview

Morsel-driven parallelism is ladybug's approach to parallel query execution where work is divided into small, independently processable chunks called "morsels." This document explains how it works for native node tables versus columnar formats (Arrow, Parquet).

## Architecture

### Key Components

```
┌─────────────────────────────────────────────────────────────┐
│                    ScanNodeTable Operator                   │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  getNextTuplesInternal() - Main scan loop              │ │
│  │  ├─ calls table->scan(transaction, scanState)          │ │
│  │  ├─ calls nextMorsel() when morsel scan exhausted      │ │
│  │  └─ calls initScanState() for new morsel               │ │
│  └────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                   Shared State (per table)                  │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  ScanNodeTableSharedState                              │ │
│  │  ├─ nextMorsel() - assigns next morsel                 │ │
│  │  │                    (based on table's morsel config) │ │
│  │  ├─ currentCommittedGroupIdx (atomic counter)          │ │
│  │  └─ numCommittedNodeGroups                             │ │
│  └────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                   Scan State (per thread)                   │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  NodeTableScanState                                    │ │
│  │  ├─ nodeGroupIdx - current morsel being processed      │ │
│  │  ├─ source - COMMITTED/UNCOMMITTED/NONE                │ │
│  │  └─ Table-specific state                               │ │
|  |                                                        | |
|  |  ArrowNodeTableScanState                               │ │
│  |  ├─ currentBatchIdx                                    │ │
│  |  ├─ currentMorselStartOffset                           │ │
│  |  ├─ currentMorselEndOffset                             │ │
│  |  └─ ...                                                │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Native Node Table Scanning

### Flow

1. **Initialization** (`initGlobalStateInternal`):
   - `ScanNodeTableSharedState::initialize()` counts node groups
   - Sets `numCommittedNodeGroups` from `table->getNumCommittedNodeGroups()`

2. **Morsel Assignment** (`nextMorsel` in `scan_node_table.cpp:74-91`):
   ```cpp
   if (currentCommittedGroupIdx < numCommittedNodeGroups) {
       nodeScanState.nodeGroupIdx = currentCommittedGroupIdx++;  // Atomic assign
       nodeScanState.source = TableScanSource::COMMITTED;
       return;
   }
   ```
   - Each thread gets one node group (typically ~128K rows)
   - Simple atomic counter increment

3. **Scanning** (`getNextTuplesInternal` in `scan_node_table.cpp:154-178`):
   ```cpp
   while (info.table->scan(transaction, *scanState)) {
       // Process entire node group
       if (outputSize > 0) return true;
   }
   sharedStates[currentTableIdx]->nextMorsel(*scanState, *progressSharedState);
   ```
   - Each `scan()` call processes the entire assigned node group
   - When exhausted, calls `nextMorsel()` to get next node group

4. **Table-Level Scan** (`NodeTable::scanInternal` in `node_table.cpp:301-304`):
   ```cpp
   bool NodeTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
       scanState.resetOutVectors();
       return scanState.scanNext(transaction);  // One call = one node group
   }
   ```

### Characteristics

- **Morsel size**: Entire node group (~128K rows by default)
- **Granularity**: Coarse - one morsel per thread at a time
- **NextMorsel usage**: Called after each node group is fully scanned

## Arrow Node Table Scanning

### Flow

1. **Morsel Assignment** (`nextMorsel` in `scan_node_table.cpp:74-91`):
   ```cpp
   if (const auto arrowTable = dynamic_cast<ArrowNodeTable*>(this->table)) {
        const auto tableSharedState = arrowTable->getTableScanSharedState();
        if (tableSharedState->getNextMorsel(static_cast<ColumnarNodeTableScanState*>(&scanState))) {
            scanState.source = TableScanSource::COMMITTED;
            progressSharedState.numMorselsScanned++;
        }
   }
   ```
   - morsel assignment is delegated to ArrowNodeTableScanSharedState

2. **Scanning** (`scanInternal` in `arrow_node_table.cpp:127-196`):
   ```cpp
   // Calculate the size of the current morsel
   auto morselStart = arrowScanState.currentMorselStartOffset;
   auto morselEnd = std::min((uint64_t)arrowScanState.currentMorselEndOffset, batchLength);
   auto outputSize = static_cast<uint64_t>(morselEnd - morselStart);

   auto nextGlobalRowOffset = batchStartOffsets[arrowScanState.currentBatchIdx] + morselStart;

   scanState.outState->getSelVectorUnsafe().setSelSize(outputSize);

   NodeTable::applySemiMaskFilter(scanState, nextGlobalRowOffset, outputSize,
        scanState.outState->getSelVectorUnsafe());

   if (scanState.outState->getSelVector().getSelSize() == 0) {
        return false;
    }

   DASSERT(scanState.outputVectors.size() == arrowScanState.outputToArrowColumnIdx.size());
   copyArrowMorselToOutputVectors(batch, arrowScanState.currentMorselStartOffset, outputSize,
   ```
   - Returns after processing ONE morsel (2048 rows)

3. **Operator-Level Loop** (`getNextTuplesInternal`):
   ```cpp
   while (info.table->scan(transaction, *scanState)) {
       // Called once per morsel, NOT per arrowBatch!
   }
   ```
   - Inner while loop runs for each morsel (2048 rows)
   - `nextMorsel()` only called after each morsel scan

### Characteristics

- **Morsel size**: 2048 rows (configurable)
- **Granularity**: Fine-grained within each batch
- **NextMorsel usage**: Called after evey morsel scan

## Key Differences Summary

| Aspect | Native Node Tables | Arrow Tables |
|--------|-------------------|---------------------|
| **Morsel Size** | ~128K rows (full node group) | 2048 rows (sub-arrow-batch) |
| **Inner Loop** | Entire node group in one scan() | One morsel per scan() call |

## Appendix: Related Files

- `src/processor/operator/scan/scan_node_table.cpp` - Main scan operator
- `src/storage/table/arrow_node_table.cpp` - Arrow table implementation
- `src/storage/table/node_table.cpp` - Native table implementation
