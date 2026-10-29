#pragma once

#include "common/copy_constructors.h"
#include "storage/local_storage/local_hash_index.h"
#include "storage/local_storage/local_table.h"
#include "storage/table/node_group_collection.h"

namespace lbug {
namespace storage {

struct TableScanState;
class MemoryManager;

class LocalNodeTable final : public LocalTable {
public:
    LocalNodeTable(const catalog::TableCatalogEntry* tableEntry, Table& table, MemoryManager& mm);
    DELETE_COPY_AND_MOVE(LocalNodeTable);

    bool insert(transaction::Transaction* transaction, TableInsertState& insertState) override;
    bool update(transaction::Transaction* transaction, TableUpdateState& updateState) override;
    bool delete_(transaction::Transaction* transaction, TableDeleteState& deleteState) override;
    bool addColumn(TableAddColumnState& addColumnState) override;

    common::offset_t validateUniquenessConstraint(const transaction::Transaction* transaction,
        const common::ValueVector& pkVector) const;

    common::TableType getTableType() const override { return common::TableType::NODE; }

    void clear(MemoryManager& mm) override;

    common::row_idx_t getNumTotalRows() override { return nodeGroups.getNumTotalRows(); }
    common::node_group_idx_t getNumNodeGroups() const { return nodeGroups.getNumNodeGroups(); }

    NodeGroup* getNodeGroup(common::node_group_idx_t nodeGroupIdx) const {
        return nodeGroups.getNodeGroup(nodeGroupIdx);
    }
    NodeGroupCollection& getNodeGroups() { return nodeGroups; }

    bool lookupPK(const transaction::Transaction* transaction, const common::ValueVector* keyVector,
        common::sel_t pos, common::offset_t& result) const;

    TableStats getStats() const { return nodeGroups.getStats(); }
    const std::vector<common::column_id_t>& getColumnIDs() const { return columnIDs; }
    common::offset_t getStartOffset() const { return startOffset; }
    bool isVisible(const transaction::Transaction* transaction, common::offset_t offset) const;

    static std::vector<common::LogicalType> getNodeTableColumnTypes(
        const catalog::TableCatalogEntry& table);

private:
    void initLocalHashIndex(MemoryManager& mm);

private:
    // This is equivalent to the num of committed nodes in the table.
    common::offset_t startOffset;
    PageCursor overflowCursor;
    std::unique_ptr<OverflowFile> overflowFile;
    OverflowFileHandle* overflowFileHandle;
    std::unique_ptr<LocalHashIndex> hashIndex;
    NodeGroupCollection nodeGroups;
    // Storage column of each local column, taken from the entry the table was built from. A
    // column dropped later in the transaction keeps its local column, so the entry seen at read
    // time can't map the local columns.
    std::vector<common::column_id_t> columnIDs;
};

} // namespace storage
} // namespace lbug
