#pragma once

#include "common/enums/table_type.h"
#include "storage/table/table.h"

namespace lbug {
namespace transaction {
class Transaction;
} // namespace transaction

namespace storage {
class MemoryManager;

struct TableAddColumnState;
struct TableInsertState;
struct TableUpdateState;
struct TableDeleteState;
class LocalTable {
public:
    virtual ~LocalTable() = default;

    virtual bool insert(transaction::Transaction* transaction, TableInsertState& insertState) = 0;
    virtual bool update(transaction::Transaction* transaction, TableUpdateState& updateState) = 0;
    virtual bool delete_(transaction::Transaction* transaction, TableDeleteState& deleteState) = 0;
    virtual bool addColumn(TableAddColumnState& addColumnState) = 0;
    virtual void clear(MemoryManager& mm) = 0;
    virtual common::TableType getTableType() const = 0;
    virtual common::row_idx_t getNumTotalRows() = 0;

    const Table& getTable() const { return table; }
    // Schema epoch snapshotted when this local table was created (first touch in the
    // transaction). If the shared table's schema epoch moved since, a concurrent DDL changed
    // the columns under us and the commit must abort. Refreshed when this transaction itself
    // performs the DDL (its local columns are updated in place, so it stays consistent).
    uint64_t getSchemaEpoch() const { return schemaEpoch; }
    void refreshSchemaEpoch() { schemaEpoch = table.getSchemaEpoch(); }

    template<class TARGET>
    const TARGET& constCast() {
        return common::dynamic_cast_checked<const TARGET&>(*this);
    }
    template<class TARGET>
    TARGET& cast() {
        return common::dynamic_cast_checked<TARGET&>(*this);
    }
    template<class TARGET>
    TARGET* ptrCast() {
        return common::dynamic_cast_checked<TARGET*>(this);
    }
    template<class TARGET>
    const TARGET* ptrCast() const {
        return common::dynamic_cast_checked<TARGET*>(this);
    }

protected:
    // TODO(Guodong): Revisit this interface. We don't need to pass in Table here, instead should
    // pass in a struct that describes Table, e.g., TableInfo.
    explicit LocalTable(const Table& table) : table{table}, schemaEpoch{table.getSchemaEpoch()} {}

protected:
    const Table& table;
    uint64_t schemaEpoch;
};

} // namespace storage
} // namespace lbug
