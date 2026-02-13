#include "processor/operator/scan/scan_rel_table.h"

#include <algorithm>

#include "binder/expression/expression_util.h"
#include "common/system_config.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_rel_table.h"
#include "storage/table/foreign_rel_table.h"
#include "storage/table/node_table.h"
#include "storage/table/parquet_rel_table.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace processor {

std::string ScanRelTablePrintInfo::toString() const {
    std::string result = "Tables: ";
    for (auto& tableName : tableNames) {
        result += tableName;
        if (tableName != tableNames.back()) {
            result += ", ";
        }
    }
    if (!alias.empty()) {
        result += ",Alias: ";
        result += alias;
    }
    result += ",Direction: (";
    result += boundNode->toString();
    result += ")";
    switch (direction) {
    case ExtendDirection::FWD: {
        result += "-[";
        result += rel->detailsToString();
        result += "]->";
    } break;
    case ExtendDirection::BWD: {
        result += "<-[";
        result += rel->detailsToString();
        result += "]-";
    } break;
    case ExtendDirection::BOTH: {
        result += "<-[";
        result += rel->detailsToString();
        result += "]->";
    } break;
    default:
        KU_UNREACHABLE;
    }
    result += "(";
    result += nbrNode->toString();
    result += ")";
    if (!properties.empty()) {
        result += ",Properties: ";
        result += binder::ExpressionUtil::toString(properties);
    }
    return result;
}

void ScanRelTableInfo::initScanState(TableScanState& scanState,
    const std::vector<ValueVector*>& outVectors, main::ClientContext* context) {
    auto transaction = transaction::Transaction::Get(*context);
    scanState.setToTable(transaction, table, columnIDs, copyVector(columnPredicates), direction);
    initScanStateVectors(scanState, outVectors, MemoryManager::Get(*context));
}

void ScanRelTable::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    ScanTable::initLocalStateInternal(resultSet, context);
    auto clientContext = context->clientContext;
    auto boundNodeIDVector = resultSet->getValueVector(opInfo.nodeIDPos).get();
    auto nbrNodeIDVector = outVectors[0];
    // Check if this is a ParquetRelTable or ForeignRelTable and create appropriate scan state
    auto* parquetTable = dynamic_cast<storage::ParquetRelTable*>(tableInfo.table);
    auto* foreignTable = dynamic_cast<storage::ForeignRelTable*>(tableInfo.table);
    if (parquetTable) {
        scanState =
            std::make_unique<storage::ParquetRelTableScanState>(*MemoryManager::Get(*clientContext),
                boundNodeIDVector, outVectors, nbrNodeIDVector->state);
    } else if (foreignTable) {
        scanState =
            std::make_unique<storage::ForeignRelTableScanState>(*MemoryManager::Get(*clientContext),
                boundNodeIDVector, outVectors, nbrNodeIDVector->state);
    } else {
        scanState = std::make_unique<RelTableScanState>(*MemoryManager::Get(*clientContext),
            boundNodeIDVector, outVectors, nbrNodeIDVector->state);
    }
    tableInfo.initScanState(*scanState, outVectors, clientContext);
    if (sourceMode) {
        currentSourceTableIdx = 0;
        nextSourceOffset = 0;
        currentSourceTableNumRows = 0;
    }
}

bool ScanRelTable::fetchNextBoundNodeBatch(transaction::Transaction* transaction) {
    auto* boundNodeIDVector = scanState->nodeIDVector;
    while (currentSourceTableIdx < sourceNodeTables.size()) {
        auto* nodeTable = sourceNodeTables[currentSourceTableIdx];
        if (currentSourceTableNumRows == 0) {
            currentSourceTableNumRows = nodeTable->getNumTotalRows(transaction);
        }
        if (nextSourceOffset >= currentSourceTableNumRows) {
            currentSourceTableIdx++;
            nextSourceOffset = 0;
            currentSourceTableNumRows = 0;
            continue;
        }
        const auto numToGenerate = std::min<row_idx_t>(DEFAULT_VECTOR_CAPACITY,
            currentSourceTableNumRows - nextSourceOffset);
        boundNodeIDVector->state->setToUnflat();
        boundNodeIDVector->state->getSelVectorUnsafe().setToUnfiltered(numToGenerate);
        for (auto i = 0u; i < numToGenerate; ++i) {
            boundNodeIDVector->setValue<nodeID_t>(i,
                nodeID_t{nextSourceOffset + i, nodeTable->getTableID()});
        }
        nextSourceOffset += numToGenerate;
        tableInfo.table->initScanState(transaction, *scanState);
        return true;
    }
    return false;
}

bool ScanRelTable::getNextTuplesInternal(ExecutionContext* context) {
    const auto transaction = transaction::Transaction::Get(*context->clientContext);
    if (sourceMode) {
        while (true) {
            while (tableInfo.table->scan(transaction, *scanState)) {
                const auto outputSize = scanState->outState->getSelVector().getSelSize();
                if (outputSize > 0) {
                    metrics->numOutputTuple.increase(outputSize);
                    return true;
                }
            }
            if (!fetchNextBoundNodeBatch(transaction)) {
                return false;
            }
        }
    }
    while (true) {
        while (tableInfo.table->scan(transaction, *scanState)) {
            const auto outputSize = scanState->outState->getSelVector().getSelSize();
            if (outputSize > 0) {
                // No need to perform column cast because this is single table scan.
                metrics->numOutputTuple.increase(outputSize);
                return true;
            }
        }
        if (!children[0]->getNextTuple(context)) {
            return false;
        }
        tableInfo.table->initScanState(transaction, *scanState);
    }
}

} // namespace processor
} // namespace lbug
