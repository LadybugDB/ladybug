#pragma once

#include <mutex>
#include <vector>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/mask.h"
#include "common/types/internal_id_util.h"
#include "storage/table/node_table.h"

namespace lbug {
namespace storage {

// Abstract class : State to track table scan
struct ColumnarNodeTableScanState : NodeTableScanState {
    bool initialized = false; // Track if this scan state has been initialized for scanning
    size_t totalRows = 0;
    bool scanCompleted = false; // Track if this scan state has finished reading

    ColumnarNodeTableScanState([[maybe_unused]] MemoryManager& mm,
        common::ValueVector* nodeIDVector, std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : NodeTableScanState{nodeIDVector, std::move(outputVectors), std::move(outChunkState)} {}
};

// Interface: Shared state to coordinate morsel assignment across parallel scan states
// batch - RowGroup for Parquet, RecordBatch for Arrow
struct ColumnarNodeTableScanSharedState {
    virtual ~ColumnarNodeTableScanSharedState() = default;

    virtual bool getNextMorsel(ColumnarNodeTableScanState* scanState) = 0;
};

// Abstract base class for columnar-format node tables (Parquet, Arrow, etc.)
class ColumnarNodeTableBase : public NodeTable {
public:
    ColumnarNodeTableBase(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager,
        std::unique_ptr<ColumnarNodeTableScanSharedState> tableScanSharedState)
        : NodeTable{storageManager, nodeTableEntry, memoryManager},
          nodeTableCatalogEntry{nodeTableEntry},
          tableScanSharedState{std::move(tableScanSharedState)} {}

    virtual ~ColumnarNodeTableBase() = default;

    // Columnar tables don't support modifications
    void insert([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableInsertState& insertState) final {
        throw common::RuntimeException(
            "Cannot insert into " + getColumnarFormatName() + "-backed node table");
    }

    void update([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableUpdateState& updateState) final {
        throw common::RuntimeException(
            "Cannot update " + getColumnarFormatName() + "-backed node table");
    }

    bool delete_([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableDeleteState& deleteState) final {
        throw common::RuntimeException(
            "Cannot delete from " + getColumnarFormatName() + "-backed node table");
        return false;
    }

    common::row_idx_t getNumTotalRows(const transaction::Transaction* transaction) override;

protected:
    const catalog::NodeTableCatalogEntry* nodeTableCatalogEntry;
    mutable std::unique_ptr<ColumnarNodeTableScanSharedState> tableScanSharedState;

    // Template method pattern: subclasses implement format-specific operations
    virtual std::string getColumnarFormatName() const = 0;
    virtual common::node_group_idx_t getNumBatches(
        const transaction::Transaction* transaction) const = 0;
    virtual common::row_idx_t getTotalRowCount(
        const transaction::Transaction* transaction) const = 0;

    // Helper for constructing storage paths
    std::string constructStoragePath(const std::string& prefix, const std::string& suffix) const {
        std::string tableName = nodeTableCatalogEntry->getName();
        return prefix + "_nodes_" + tableName + suffix;
    }

public:
    ColumnarNodeTableScanSharedState* getTableScanSharedState() const {
        return tableScanSharedState.get();
    }
};

} // namespace storage
} // namespace lbug
