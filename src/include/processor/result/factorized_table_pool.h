#pragma once

#include <mutex>
#include <stack>

#include "processor/result/factorized_table.h"

namespace lbug {
namespace processor {

// We implement a local ftable pool to avoid generate many small ftables when running GDS.
// Alternative solutions are directly writing to global ftable with partition so conflict is
// minimized. Or we optimize ftable to be more memory efficient when number of tuples is small.
class LBUG_API FactorizedTablePool {
public:
    explicit FactorizedTablePool(std::shared_ptr<FactorizedTable> globalTable)
        : globalTable{std::move(globalTable)} {}
    DELETE_COPY_AND_MOVE(FactorizedTablePool);

    FactorizedTable* claimLocalTable(storage::MemoryManager* mm);

    void returnLocalTable(FactorizedTable* table);

    void mergeLocalTables();

    std::shared_ptr<FactorizedTable> getGlobalTable() const { return globalTable; }

    // Re-arm the pool for another execution of a cached physical plan. The global table is
    // shared with FTableScan operators (via their bind data), so it is cleared in place rather
    // than replaced; the per-execution local tables are dropped entirely.
    void resetForReuse() {
        globalTable->clear();
        availableLocalTables = {};
        localTables.clear();
    }

private:
    std::mutex mtx;
    std::shared_ptr<FactorizedTable> globalTable;
    std::stack<FactorizedTable*> availableLocalTables;
    std::vector<std::shared_ptr<FactorizedTable>> localTables;
};

} // namespace processor
} // namespace lbug