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
│  │  ├─ calls nextMorsel() when table scan exhausted       │ │
│  │  └─ calls initScanState() for new morsel               │ │
│  └────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                   Shared State (per table)                  │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  ScanNodeTableSharedState                              │ │
│  │  ├─ nextMorsel() - assigns node groups/batches         │ │
│  │  ├─ currentCommittedGroupIdx (atomic counter)          │ │
│  │  └─ numCommittedNodeGroups                             │ │
│  └────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                   Scan State (per thread)                   │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  NodeTableScanState / ArrowNodeTableScanState          │ │
│  │  ├─ nodeGroupIdx - current morsel being processed      │ │
│  │  ├─ source - COMMITTED/UNCOMMITTED/NONE                │ │
│  │  └─ Table-specific state                               │ │
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

## Arrow/Parquet Node Table Scanning (Current Implementation)

### Two-Level Parallelism

Recent commits (bf5a2922c and 8796783f1) introduced sub-batch morsel parallelism:

```
Level 1: Batch/Row Group Assignment (coarse)
   └── nextMorsel() assigns batches via ColumnarNodeTableSharedState

Level 2: Sub-batch Morsel Processing (fine)  
   └── scanInternal() processes morsels within batch
```

### ArrowNodeTableScanState Fields

```cpp
struct ArrowNodeTableScanState : NodeTableScanState {
    size_t currentBatchIdx = 0;         // Current Arrow batch
    size_t currentBatchOffset = 0;    // Position within batch (for data copying)
    
    // Sub-batch morsel fields (Option B - bf5a2922c)
    size_t morselSize = 2048;                    // Rows per morsel
    size_t currentMorselStartOffset = 0;         // Start of current morsel
    size_t currentMorselEndOffset = 0;           // End of current morsel
    
    size_t nextGlobalRowOffset = 0;     // Global offset for node IDs
};
```

### Flow

1. **Batch Assignment** (`initArrowScanForBatch` in `arrow_node_table.cpp:91-125`):
   - Gets batch from `sharedState->getNextBatch()` (atomic)
   - Initializes morsel boundaries for first morsel in batch

2. **Scanning** (`scanInternal` in `arrow_node_table.cpp:127-196`):
   ```cpp
   // Check if current morsel exhausted
   if (arrowScanState.currentMorselStartOffset >= batchLength) {
       // Get next batch via initArrowScanForBatch
       initArrowScanForBatch(transaction, arrowScanState);
   }
   
   // Calculate morsel boundaries
   auto morselStart = arrowScanState.currentMorselStartOffset;
   auto morselEnd = std::min(arrowScanState.currentMorselEndOffset, batchLength);
   
   // Copy data and advance
   copyArrowBatchToOutputVectors(batch, morselStart, outputSize, ...);
   
   // Advance to next morsel
   arrowScanState.currentMorselStartOffset = morselEnd;
   arrowScanState.currentMorselEndOffset = morselEnd + arrowScanState.morselSize;
   ```
   - Returns after processing ONE morsel (2048 rows)
   - Has internal loop logic for advancing morsels within batch
   - Only fetches new batch when current batch's morsels exhausted

3. **Operator-Level Loop** (`getNextTuplesInternal`):
   ```cpp
   while (info.table->scan(transaction, *scanState)) {
       // Called once per morsel, NOT per batch!
   }
   ```
   - Inner while loop runs for each morsel (2048 rows)
   - `nextMorsel()` only called after ALL morsels in batch exhausted

### Characteristics

- **Two-level parallelism**: Batch-level (via nextMorsel) + sub-batch morsels
- **Morsel size**: 2048 rows (configurable)
- **Granularity**: Fine-grained within each batch
- **NextMorsel usage**: Called only when entire batch exhausted

## Key Differences Summary

| Aspect | Native Node Tables | Arrow/Parquet Tables |
|--------|-------------------|---------------------|
| **Morsel Assignment** | One node group per nextMorsel() | One batch per nextMorsel() |
| **Morsel Size** | ~128K rows (full node group) | 2048 rows (sub-batch) |
| **Inner Loop** | Entire node group in one scan() | One morsel per scan() call |
| **Granularity** | Coarse | Fine-grained |
| **nextMorsel() Call Frequency** | After each node group | After each batch (many morsels) |
| **Parallelism Level** | Single-level | Two-level |

## Code Structure Comparison

### Native Tables

```cpp
// scan_node_table.cpp:getNextTuplesInternal
while (info.table->scan(transaction, *scanState)) {  // Scans full node group
    return true;  // Return data
}
// Node group exhausted, get next
sharedStates[idx]->nextMorsel(*scanState, ...);  // Assign next node group

// node_table.cpp:scanInternal
bool NodeTable::scanInternal(...) {
    return scanState.scanNext(transaction);  // Full node group
}
```

### Arrow Tables

```cpp
// scan_node_table.cpp:getNextTuplesInternal (SAME code!)
while (info.table->scan(transaction, *scanState)) {  // Scans ONE morsel
    return true;
}
// Batch exhausted (after many morsels), get next batch
sharedStates[idx]->nextMorsel(*scanState, ...);  // Assign next batch

// arrow_node_table.cpp:scanInternal
bool ArrowNodeTable::scanInternal(...) {
    // Check if morsel exhausted, advance within batch
    if (arrowScanState.currentMorselStartOffset >= batchLength) {
        initArrowScanForBatch(...);  // Get new batch
    }
    
    // Process ONE morsel
    auto morselStart = arrowScanState.currentMorselStartOffset;
    auto morselEnd = ...;
    copyArrowBatchToOutputVectors(batch, morselStart, ...);
    
    // Advance to next morsel (within batch)
    arrowScanState.currentMorselStartOffset = morselEnd;
    return true;
}
```

## Evaluation of Proposed Changes

### Proposal Summary

> "scanState.currentBatchOffset can be replaced by scanState.currentMorselStartOffset"
> 
> "why not leave the batch level orchestration to scan_node_table? The inner while loop in getNextTuplesInternal is run for each batch. But with this change, it runs for full table instead and nextMorsel in scan_node_table is never used."

### Analysis

**Current Issue Identified:**

The user's observation is **correct** - there is an inconsistency in how morsel-driven parallelism is implemented:

1. **Native tables**: `nextMorsel()` assigns work at the granularity of a full node group, and `scan()` processes the entire group
2. **Arrow tables**: `nextMorsel()` assigns work at the granularity of a full batch, but `scan()` only processes a sub-batch morsel (2048 rows)

This leads to:
- Inconsistent semantics of what `nextMorsel()` assigns
- Inconsistent return behavior of `scan()` (full group vs partial morsel)
- `nextMorsel()` in `scan_node_table.cpp` is underutilized for Arrow tables

### Evaluation of Proposal

**Proposal 1: "scanState.currentBatchOffset can be replaced by scanState.currentMorselStartOffset"**

- **Status**: Partially correct
- **Analysis**: 
  - `currentBatchOffset` is used for data copying position (`copyArrowBatchToOutputVectors`)
  - `currentMorselStartOffset` is used for morsel boundary tracking
  - These serve different purposes but could be unified
  - The copy function could use `currentMorselStartOffset` directly

**Proposal 2: "Leave batch level orchestration to scan_node_table"**

- **Status**: **Valid architectural concern**
- **Analysis**:
  - Currently `ArrowNodeTable::scanInternal()` has implicit inner loop logic for morsel advancement
  - This breaks the pattern where `getNextTuplesInternal()` drives the scanning loop
  - The `nextMorsel()` in `scan_node_table` is designed to orchestrate morsel distribution, but Arrow tables bypass it for sub-batch morsels

### Recommended Refactoring

**Option A: Unified Morsel Approach (Recommended)**

Make all tables use the same pattern:

1. `nextMorsel()` assigns morsels at the finest granularity (2048 rows for Arrow, node groups for native)
2. Remove sub-batch morsel logic from `ArrowNodeTable::scanInternal()`
3. Have `ColumnarNodeTableSharedState` track morsel position within batches
4. Each `scan()` call processes exactly one morsel

**Changes needed:**

```cpp
// Modify ColumnarNodeTableSharedState to track sub-batch position
struct ColumnarNodeTableSharedState {
    std::mutex mtx;
    node_group_idx_t currentBatchIdx = 0;
    size_t currentMorselOffset = 0;  // New: track position within batch
    size_t numBatches = 0;
    std::vector<size_t> batchLengths;
    
    bool getNextMorsel(node_group_idx_t& batchIdx, size_t& startOffset, size_t& endOffset) {
        std::lock_guard<std::mutex> lock(mtx);
        while (currentBatchIdx < numBatches) {
            auto batchLength = batchLengths[currentBatchIdx];
            if (currentMorselOffset < batchLength) {
                batchIdx = currentBatchIdx;
                startOffset = currentMorselOffset;
                endOffset = std::min(currentMorselOffset + morselSize, batchLength);
                currentMorselOffset = endOffset;
                if (currentMorselOffset >= batchLength) {
                    currentBatchIdx++;
                    currentMorselOffset = 0;
                }
                return true;
            }
            currentBatchIdx++;
            currentMorselOffset = 0;
        }
        return false;
    }
};

// Simplify ArrowNodeTable::scanInternal
bool ArrowNodeTable::scanInternal(...) {
    auto& arrowScanState = scanState.cast<ArrowNodeTableScanState>();
    
    // Just copy data at the assigned morsel position
    auto batchIdx = arrowScanState.nodeGroupIdx;
    auto morselStart = arrowScanState.currentMorselStartOffset;
    auto morselEnd = arrowScanState.currentMorselEndOffset;
    auto outputSize = morselEnd - morselStart;
    
    copyArrowBatchToOutputVectors(arrays[batchIdx], morselStart, outputSize, ...);
    
    return true;  // Single morsel processed
}
```

**Benefits:**
1. **Consistency**: All table types use same morsel-driven pattern
2. **Predictability**: `scan()` always processes exactly one morsel
3. **Better load balancing**: Finer granularity allows better work distribution
4. **Simpler code**: Remove inner loop logic from table implementations

**Option B: Revert to Native Pattern**

Remove sub-batch morsel processing and treat entire Arrow batch as one morsel:

1. Each batch = one morsel
2. Remove `currentMorselStartOffset/EndOffset` fields
3. Simpler but loses fine-grained parallelism for large batches

### Conclusion

The user's concern is valid - the current implementation creates inconsistent behavior:

1. **The inconsistency exists**: Arrow tables handle sub-batch morsels internally while native tables don't
2. **The proposal is directionally correct**: Moving morsel orchestration to `scan_node_table` would unify the architecture
3. **Recommended action**: Refactor to Option A above, making morsel assignment the responsibility of the shared state and operator, not individual table implementations

This would allow Parquet, Arrow, and any future columnar formats to follow the exact same pattern as native node tables, with only the data copying logic being format-specific.

## Appendix: Related Files

- `src/processor/operator/scan/scan_node_table.cpp` - Main scan operator
- `src/storage/table/arrow_node_table.cpp` - Arrow table implementation
- `src/storage/table/parquet_node_table.cpp` - Parquet table implementation  
- `src/storage/table/node_table.cpp` - Native table implementation
- `src/include/storage/table/columnar_node_table_base.h` - Shared state for columnar tables
