#pragma once

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "function/table/table_function.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace storage {

struct ForeignRelTableScanState final : RelTableScanState {
    std::shared_ptr<function::TableFuncSharedState> sharedState;
    std::shared_ptr<function::TableFuncLocalState> localState;
    common::DataChunk dataChunk;

    ForeignRelTableScanState(MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState);
};

class ForeignRelTable final : public RelTable {
public:
    ForeignRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const StorageManager* storageManager,
        MemoryManager* memoryManager, function::TableFunction scanFunction,
        std::shared_ptr<function::TableFuncBindData> scanBindData);

    void initScanState(transaction::Transaction* transaction, TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction, TableScanState& scanState) override;

    // For foreign-backed tables, we don't support modifications
    void insert([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableInsertState& insertState) override {
        throw common::RuntimeException("Cannot insert into foreign-backed rel table");
    }
    void update([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableUpdateState& updateState) override {
        throw common::RuntimeException("Cannot update foreign-backed rel table");
    }
    bool delete_([[maybe_unused]] transaction::Transaction* transaction,
        [[maybe_unused]] TableDeleteState& deleteState) override {
        throw common::RuntimeException("Cannot delete from foreign-backed rel table");
        return false;
    }

    common::row_idx_t getNumTotalRows(const transaction::Transaction* transaction) override;

private:
    function::TableFunction scanFunction;
    std::shared_ptr<function::TableFuncBindData> scanBindData;
};

} // namespace storage
} // namespace lbug