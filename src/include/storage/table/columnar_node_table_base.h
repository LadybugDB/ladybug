#pragma once

#include <mutex>
#include <vector>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "storage/table/node_table.h"

namespace lbug {
namespace storage {

// Shared state to coordinate row group/batch assignment across parallel scan states
struct ColumnarNodeTableSharedState {
    std::mutex mtx;
    common::node_group_idx_t currentBatchIdx = 0;
    common::node_group_idx_t numBatches = 0;

    void reset(common::node_group_idx_t totalBatches) {
        std::lock_guard<std::mutex> lock(mtx);
        currentBatchIdx = 0;
        numBatches = totalBatches;
    }

    bool getNextBatch(common::node_group_idx_t& assignedBatchIdx) {
        std::lock_guard<std::mutex> lock(mtx);
        if (currentBatchIdx < numBatches) {
            assignedBatchIdx = currentBatchIdx++;
            return true;
        }
        return false;
    }
};

// Abstract base class for columnar-format node tables (Parquet, Arrow, etc.)
class ColumnarNodeTableBase : public NodeTable {
public:
    ColumnarNodeTableBase(const StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager)
        : NodeTable{storageManager, nodeTableEntry, memoryManager},
          nodeTableCatalogEntry{nodeTableEntry} {
        sharedState = std::make_unique<ColumnarNodeTableSharedState>();
    }

    virtual ~ColumnarNodeTableBase() = default;

    // Override to reset shared state for batch coordination at the start of each scan operation
    void initializeScanCoordination(const transaction::Transaction* transaction) override;

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
    mutable std::unique_ptr<ColumnarNodeTableSharedState> sharedState;

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
};

} // namespace storage
} // namespace lbug
