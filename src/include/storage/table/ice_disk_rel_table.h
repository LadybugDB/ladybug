#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "storage/table/columnar_rel_table_base.h"
#include "transaction/transaction.h"

namespace lbug {
namespace main {
class ClientContext;
} // namespace main
namespace storage {

enum class IceDiskRelTableLayout : uint8_t { CSR, FLAT };

// Concrete columnar scan state for icebug-disk rel tables. Carries no fields of its own:
// all shared cursor state lives on ColumnarRelTableScanState so one state can serve mixed
// multi-rel scans. Kept (non-final for now) because the lance extension subclasses it;
// new extension backends should extend ColumnarRelTableScanState directly.
// TODO: re-final once the lance extension migrates to ColumnarRelTableScanState.
struct LBUG_API IceDiskRelTableScanState : ColumnarRelTableScanState {
    using ColumnarRelTableScanState::ColumnarRelTableScanState;
};

class IceDiskRelTable final : public ColumnarRelTableBase {
public:
    IceDiskRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager, main::ClientContext* context = nullptr);

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

protected:
    // Implement ColumnarRelTableBase interface
    std::string getColumnarFormatName() const override { return "icebug-disk"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;
    common::row_idx_t getActiveBoundNodeCount(const transaction::Transaction* transaction,
        common::RelDataDirection direction) const override;
    std::vector<std::pair<common::offset_t, common::row_idx_t>> getAllDegreeEntries(
        const transaction::Transaction* transaction,
        common::RelDataDirection direction) const override;
    common::row_idx_t getDegreeForOffsetInternal(const transaction::Transaction* transaction,
        common::RelDataDirection direction, common::offset_t nodeOffset) const override;
    std::vector<std::pair<common::offset_t, common::row_idx_t>> getTopKDegreeEntries(
        const transaction::Transaction* transaction, common::RelDataDirection direction,
        common::idx_t k) const override;

private:
    IceDiskRelTableLayout layout;
    std::string indicesFilePath;
    std::string indptrFilePath;
    mutable std::mutex indptrDataMutex;
    // Cached total row count, populated on first getTotalRowCount() call. A temporary
    // ParquetReader is used instead of a shared cached reader so no lock is needed
    // (at worst two threads populate the same value concurrently).
    mutable std::vector<common::offset_t> indptrData; // Cached indptr data for CSR format
    // Set to true once indptrData has been populated (even if the file contained zero rows,
    // so an empty indptr doesn't trigger a reload on every scan). Read with acquire /
    // written with release to publish the vector contents to lock-free readers.
    mutable std::atomic<bool> indptrDataLoaded{false};
    mutable std::atomic<common::row_idx_t> cachedRowCount{common::INVALID_ROW_IDX};

    void loadIndptrData(transaction::Transaction* transaction) const;
    bool scanCSR(transaction::Transaction* transaction, ColumnarRelTableScanState& scanState);
    bool scanFlat(transaction::Transaction* transaction, ColumnarRelTableScanState& scanState);
};

} // namespace storage
} // namespace lbug
