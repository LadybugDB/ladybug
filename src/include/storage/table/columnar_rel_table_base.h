#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/api.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "processor/operator/persistent/reader/parquet/parquet_reader.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace storage {

// Shared scan state for columnar-format rel tables (icebug-disk, Arrow, and
// extension-backed formats such as lance). It unions the per-scan cursor state that
// every columnar backend needs: the CSR/parquet batch cursors and per-state file
// readers plus the Arrow batch cursors. Keeping all of it on this common base (rather
// than splitting per format) is what lets one shared state serve a multi-rel scan over
// mixed backends: each table's initScanState reconfigures the same state for its own
// file, and each table's scan only touches the cursor subset its backend uses.
// Extension backends must extend this state (not a format-specific subclass) for
// ColumnarTableHooks::supportsMixedColumnarScan.
struct LBUG_API ColumnarRelTableScanState : RelTableScanState {
    std::unique_ptr<processor::ParquetReaderScanState> parquetScanState;

    // cached data for the current batch in current row group
    std::unique_ptr<common::DataChunk> cachedBatchData;
    common::offset_t currentBatchStartOffset =
        0; // Global row index of the start of the current batch of the current row group
    common::offset_t currentLocalRowIdx =
        0; // Row index within the current batch of the current row group
    std::unordered_map<common::offset_t, common::sel_t>
        boundNodeOffsets; // Map from bound node offset to selection vector index

    // Monotonic cursor into the indptr array for CSR scans. Since rows are examined in
    // increasing global (CSR) order, the source node of the current row is found by advancing
    // this cursor (O(1) amortized) instead of a binary search per row.
    common::offset_t csrSrcNodeIdx = 0;

    // Per-scan-state readers for thread safety. A single scan state is shared across all
    // rel tables in a multi-rel scan (see RelTableCollectionScanner), so the readers must be
    // re-created whenever the scan switches to a table backed by a different file. The paths
    // below track which file each reader was opened for.
    std::unique_ptr<processor::ParquetReader> indicesReader;
    std::string indicesReaderPath;
    std::unique_ptr<processor::ParquetReader> indptrReader;
    std::string indptrReaderPath;

    // Arrow backend cursors. Only touched by Arrow-backed tables; other backends leave
    // them at their defaults.
    size_t arrowCurrentBatchIdx = 0;
    size_t arrowCurrentBatchOffset = 0;
    size_t arrowCSRBoundIdx = 0;
    common::offset_t arrowCSRCurrentRelOffset = common::INVALID_OFFSET;
    std::unordered_map<common::offset_t, common::sel_t> arrowBoundNodeOffsetToSelPos;
    std::unique_ptr<common::ValueVector> arrowSrcKeyVector;
    std::unique_ptr<common::ValueVector> arrowDstKeyVector;
    bool arrowScanCompleted = true;

    ColumnarRelTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : RelTableScanState{mm, nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {
        parquetScanState = std::make_unique<processor::ParquetReaderScanState>();
    }

    void setToTable(const transaction::Transaction* transaction, Table* table_,
        std::vector<common::column_id_t> columnIDs_,
        std::vector<ColumnPredicateSet> columnPredicateSets_,
        common::RelDataDirection direction_) override;

    void reset(std::unordered_map<common::offset_t, common::sel_t> boundNodeOffsets_) {
        cachedBatchData = nullptr;
        currentBatchStartOffset = 0;
        currentLocalRowIdx = 0;
        boundNodeOffsets = std::move(boundNodeOffsets_);
    }

    void reloadCachedBatchData(transaction::Transaction* transaction);
};

// Abstract base class for columnar-format relationship tables (Icebug-Disk, Arrow, etc.)
// Exported for extensions providing columnar table storage (e.g. the lance extension).
class LBUG_API ColumnarRelTableBase : public RelTable {
public:
    ColumnarRelTableBase(catalog::RelGroupCatalogEntry* relGroupEntry,
        common::table_id_t fromTableID, common::table_id_t toTableID,
        const StorageManager* storageManager, MemoryManager* memoryManager)
        : RelTable{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
          relGroupEntry{relGroupEntry} {}

    virtual ~ColumnarRelTableBase() = default;

    // Columnar tables don't support modifications
    void insert([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableInsertState& insertState) final {
        throw common::RuntimeException(
            "Cannot insert into " + getColumnarFormatName() + "-backed rel table");
    }

    void update([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableUpdateState& updateState) final {
        throw common::RuntimeException(
            "Cannot update " + getColumnarFormatName() + "-backed rel table");
    }

    bool delete_([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableDeleteState& deleteState) final {
        throw common::RuntimeException(
            "Cannot delete from " + getColumnarFormatName() + "-backed rel table");
        return false;
    }

    common::row_idx_t getNumTotalRows(const transaction::Transaction* transaction) override;
    common::row_idx_t getNumActiveBoundNodes(const transaction::Transaction* transaction,
        common::RelDataDirection direction) override;
    std::vector<std::pair<common::offset_t, common::row_idx_t>> getDegreeEntries(
        const transaction::Transaction* transaction, common::RelDataDirection direction) override;
    common::row_idx_t getDegreeForOffset(const transaction::Transaction* transaction,
        common::RelDataDirection direction, common::offset_t nodeOffset) override;
    std::vector<std::pair<common::offset_t, common::row_idx_t>> getTopKDegrees(
        const transaction::Transaction* transaction, common::RelDataDirection direction,
        common::idx_t k) override;
    RelDegreeSummary getDegreeSummary(const transaction::Transaction* transaction,
        common::RelDataDirection direction) override;

protected:
    catalog::RelGroupCatalogEntry* relGroupEntry;
    mutable std::mutex dataAccessMutex;

    // Template method pattern: subclasses implement format-specific operations
    virtual std::string getColumnarFormatName() const = 0;
    virtual common::row_idx_t getTotalRowCount(
        const transaction::Transaction* transaction) const = 0;
    virtual common::row_idx_t getActiveBoundNodeCount(const transaction::Transaction* transaction,
        common::RelDataDirection direction) const = 0;
    virtual std::vector<std::pair<common::offset_t, common::row_idx_t>> getAllDegreeEntries(
        const transaction::Transaction* transaction, common::RelDataDirection direction) const = 0;
    virtual common::row_idx_t getDegreeForOffsetInternal(
        const transaction::Transaction* transaction, common::RelDataDirection direction,
        common::offset_t nodeOffset) const = 0;
    virtual std::vector<std::pair<common::offset_t, common::row_idx_t>> getTopKDegreeEntries(
        const transaction::Transaction* transaction, common::RelDataDirection direction,
        common::idx_t k) const = 0;

    // Helper for finding source node in CSR format
    // Subclasses should cache indptr data and provide it via this interface
    virtual common::offset_t findSourceNodeForRowInternal(common::offset_t globalRowIdx,
        const std::vector<common::offset_t>& indptrData) const;
};

} // namespace storage
} // namespace lbug
