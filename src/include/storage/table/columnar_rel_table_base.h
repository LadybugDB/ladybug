#pragma once

#include <mutex>
#include <unordered_set>
#include <vector>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "storage/table/rel_table.h"

namespace lbug {
namespace storage {

// Abstract base class for columnar-format relationship tables (Parquet, Arrow, etc.)
class ColumnarRelTableBase : public RelTable {
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

protected:
    catalog::RelGroupCatalogEntry* relGroupEntry;
    mutable std::mutex dataAccessMutex;

    // Template method pattern: subclasses implement format-specific operations
    virtual std::string getColumnarFormatName() const = 0;
    virtual common::row_idx_t getTotalRowCount(
        const transaction::Transaction* transaction) const = 0;

    // Helper for constructing storage paths for CSR format
    struct CSRFilePaths {
        std::string indices;
        std::string indptr;
        std::string metadata;
    };

    CSRFilePaths constructCSRPaths(const std::string& prefix, const std::string& suffix) const {
        std::string relName = relGroupEntry->getName();
        return {prefix + "_indices_" + relName + suffix, prefix + "_indptr_" + relName + suffix,
            prefix + "_metadata_" + relName + suffix};
    }

    // Helper for finding source node in CSR format
    // Subclasses should cache indptr data and provide it via this interface
    virtual common::offset_t findSourceNodeForRowInternal(common::offset_t globalRowIdx,
        const std::vector<common::offset_t>& indptrData) const;
};

} // namespace storage
} // namespace lbug
