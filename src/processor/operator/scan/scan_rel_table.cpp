#include "processor/operator/scan/scan_rel_table.h"

#include <algorithm>

#include "binder/expression/expression_util.h"
#include "common/system_config.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_rel_table.h"
#include "storage/table/arrow_node_table.h"
#include "storage/table/arrow_rel_table.h"
#include "storage/table/foreign_rel_table.h"
#include "storage/table/ice_disk_node_table.h"
#include "storage/table/ice_disk_rel_table.h"
#include "storage/table/node_table.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace processor {

static std::unique_ptr<TableScanState> createSourceNodeTableScanState(NodeTable* table,
    ValueVector* nodeIDVector, const std::vector<ValueVector*>& outVectors,
    MemoryManager* memoryManager) {
    if (dynamic_cast<IceDiskNodeTable*>(table) != nullptr) {
        return std::make_unique<IceDiskNodeTableScanState>(*memoryManager, nodeIDVector, outVectors,
            nodeIDVector->state);
    }
    if (dynamic_cast<ArrowNodeTable*>(table) != nullptr) {
        return std::make_unique<ArrowNodeTableScanState>(*memoryManager, nodeIDVector, outVectors,
            nodeIDVector->state);
    }
    return std::make_unique<NodeTableScanState>(nodeIDVector, outVectors, nodeIDVector->state);
}

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
        UNREACHABLE_CODE;
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
    // Check if this is an external rel table and create the corresponding scan state.
    auto* arrowTable = dynamic_cast<storage::ArrowRelTable*>(tableInfo.table);
    auto* iceDiskTable = dynamic_cast<storage::IceDiskRelTable*>(tableInfo.table);
    auto* foreignTable = dynamic_cast<storage::ForeignRelTable*>(tableInfo.table);
    if (arrowTable) {
        scanState =
            std::make_unique<storage::ArrowRelTableScanState>(*MemoryManager::Get(*clientContext),
                boundNodeIDVector, outVectors, nbrNodeIDVector->state);
    } else if (iceDiskTable) {
        scanState =
            std::make_unique<storage::IceDiskRelTableScanState>(*MemoryManager::Get(*clientContext),
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
    if (sourceNodeScanMode) {
        sourceNodeOutVectors.clear();
        for (auto& pos : sourceNodeScanInfo.outVectorsPos) {
            sourceNodeOutVectors.push_back(resultSet->getValueVector(pos).get());
        }
    }
    if (sourceMode) {
        currentSourceTableIdx = 0;
        nextSourceOffset = 0;
        currentSourceTableNumRows = 0;
    }
}

void ScanRelTable::initGlobalStateInternal(ExecutionContext* context) {
    if (!sourceNodeScanMode) {
        return;
    }
    DASSERT(sourceNodeTableInfos.size() == sourceNodeSharedStates.size());
    for (auto i = 0u; i < sourceNodeTableInfos.size(); ++i) {
        sourceNodeSharedStates[i]->initialize(
            transaction::Transaction::Get(*context->clientContext),
            sourceNodeTableInfos[i].table->ptrCast<NodeTable>(), *sourceNodeProgressSharedState);
    }
}

static void initSourceNodeScanState(ScanNodeTableInfo& sourceInfo,
    std::unique_ptr<TableScanState>& sourceScanState, ValueVector* boundNodeIDVector,
    const std::vector<ValueVector*>& sourceNodeOutVectors, main::ClientContext* context) {
    sourceScanState = createSourceNodeTableScanState(sourceInfo.table->ptrCast<NodeTable>(),
        boundNodeIDVector, sourceNodeOutVectors, MemoryManager::Get(*context));
    sourceInfo.initScanState(*sourceScanState, sourceNodeOutVectors, context);
    if (dynamic_cast<IceDiskNodeTable*>(sourceInfo.table) ||
        dynamic_cast<ArrowNodeTable*>(sourceInfo.table)) {
        sourceInfo.table->initScanState(transaction::Transaction::Get(*context), *sourceScanState);
    }
}

bool ScanRelTable::fetchNextBoundNodeBatch(transaction::Transaction* transaction) {
    if (sourceNodeScanMode) {
        auto* boundNodeIDVector = scanState->nodeIDVector;
        auto context = transaction->getClientContext();
        while (currentSourceTableIdx < sourceNodeTableInfos.size()) {
            auto& sourceInfo = sourceNodeTableInfos[currentSourceTableIdx];
            if (!sourceNodeScanState) {
                initSourceNodeScanState(sourceInfo, sourceNodeScanState, boundNodeIDVector,
                    sourceNodeOutVectors, context);
            }
            while (sourceInfo.table->scan(transaction, *sourceNodeScanState)) {
                const auto outputSize = sourceNodeScanState->outState->getSelVector().getSelSize();
                if (outputSize > 0) {
                    sourceInfo.castColumns();
                    sourceNodeScanState->outState->setToUnflat();
                    tableInfo.table->initScanState(transaction, *scanState);
                    return true;
                }
            }
            sourceNodeSharedStates[currentSourceTableIdx]->nextMorsel(*sourceNodeScanState,
                *sourceNodeProgressSharedState);
            if (sourceNodeScanState->source == TableScanSource::NONE) {
                currentSourceTableIdx++;
                sourceNodeScanState = nullptr;
            } else {
                sourceInfo.table->initScanState(transaction, *sourceNodeScanState);
            }
        }
        return false;
    }

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

void ScanRelTable::updatePackedChildSlices(sel_t outputSize) const {
    if (operatorType != PhysicalOperatorType::PACKED_EXTEND) {
        scanState->outState->clearPackedChildSlices();
        return;
    }
    // The CSR scan sets nodeIDVector to flat, pointing its selVector[0] at the actual parent
    // whose children are currently materialized in the output vector (see
    // RelTableScanState::setNodeIDVectorToFlat). We must use that position as the parent
    // position, NOT currBoundNodeIdx, because currBoundNodeIdx may already have advanced past
    // this parent by the time we get here (it is incremented when a parent's CSR list is fully
    // consumed within a single scan() call). The current scan architecture processes one parent
    // per output batch, so we set a single-parent slice (overwriting any previous one).
    const auto& boundSelVector = scanState->nodeIDVector->state->getSelVector();
    DASSERT(boundSelVector.getSelSize() == 1);
    scanState->outState->setSingleParentPackedChildSlice(boundSelVector[0], outputSize);
}

void ScanRelTable::reservePackedChildSlicesForBatch() const {
    if (operatorType != PhysicalOperatorType::PACKED_EXTEND) {
        return;
    }
    // cachedBoundNodeSelVector holds the bound-node positions for the current input batch and is
    // (re)populated by RelTableScanState::initCachedBoundNodeIDSelVector() during initScanState.
    // Its selSize is the number of parents that may produce children in this batch.
    //
    // Only reserve when more than one parent is in flight: the single-parent path uses
    // setSingleParentPackedChildSlice (overwrite), which replaces the descriptor and would throw
    // away a reservation. Reserving for numParents > 1 keeps the multi-parent append() path
    // reallocation-free without adding a wasted allocation to the common single-parent path.
    const auto numParents = scanState->cachedBoundNodeSelVector.getSelSize();
    if (numParents <= 1) {
        return;
    }
    scanState->outState->reservePackedChildSlices(numParents);
}

bool ScanRelTable::getNextTuplesInternal(ExecutionContext* context) {
    const auto transaction = transaction::Transaction::Get(*context->clientContext);
    if (sourceMode) {
        while (true) {
            while (tableInfo.table->scan(transaction, *scanState)) {
                const auto outputSize = scanState->outState->getSelVector().getSelSize();
                if (outputSize > 0) {
                    updatePackedChildSlices(outputSize);
                    tableInfo.castColumns();
                    metrics->numOutputTuple.increase(outputSize);
                    return true;
                }
            }
            if (!fetchNextBoundNodeBatch(transaction)) {
                return false;
            }
            // fetchNextBoundNodeBatch established a new input batch (and repopulated
            // cachedBoundNodeSelVector via initScanState); reserve for the new parent count.
            reservePackedChildSlicesForBatch();
        }
    }
    while (true) {
        while (tableInfo.table->scan(transaction, *scanState)) {
            const auto outputSize = scanState->outState->getSelVector().getSelSize();
            if (outputSize > 0) {
                updatePackedChildSlices(outputSize);
                tableInfo.castColumns();
                metrics->numOutputTuple.increase(outputSize);
                return true;
            }
        }
        if (!children[0]->getNextTuple(context)) {
            return false;
        }
        tableInfo.table->initScanState(transaction, *scanState);
        // A new input batch was just pulled and initScanState repopulated
        // cachedBoundNodeSelVector; reserve for the new parent count.
        reservePackedChildSlicesForBatch();
    }
}

} // namespace processor
} // namespace lbug
