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
    // For non-packed extend we clear any packed child slices. For packed extend we append the
    // current parent's slice to the outState's packedChildSlices so multiple parents processed
    // within the same output batch get represented as parentPositions + offsets.
    if (operatorType != PhysicalOperatorType::PACKED_EXTEND) {
        scanState->outState->clearPackedChildSlices();
        return;
    }

    // Determine the parent position corresponding to the current scan result.
    // Use the cachedBoundNodeSelVector which contains the original parent positions provided by
    // the input (it may contain multiple parents). currBoundNodeIdx points to the parent whose
    // CSR list is currently being scanned.
    const auto& cachedBoundSel = scanState->cachedBoundNodeSelVector;
    if (cachedBoundSel.getSelSize() == 0) {
        // Nothing to attach.
        return;
    }
    sel_t parentPos = 0;
    if (cachedBoundSel.getSelSize() == 1) {
        parentPos = cachedBoundSel[0];
    } else {
        // currBoundNodeIdx tracks which parent within cachedBoundSel is currently scanned.
        DASSERT(scanState->currBoundNodeIdx < cachedBoundSel.getSelSize());
        parentPos = cachedBoundSel[scanState->currBoundNodeIdx];
    }

    // Append to existing packed slices if present, otherwise create a new single-parent slice.
    // Use the append helper on DataChunkState which maintains offsets invariant.
    scanState->outState->appendPackedChildSlice(parentPos, outputSize);
}

bool ScanRelTable::getNextTuplesInternal(ExecutionContext* context) {
    const auto transaction = transaction::Transaction::Get(*context->clientContext);
    // Clear any previous packed child slices at the start of producing a new output batch for
    // packed extend. We will accumulate per-parent slices as we scan multiple parents.
    if (operatorType == PhysicalOperatorType::PACKED_EXTEND && scanState && scanState->outState) {
        scanState->outState->clearPackedChildSlices();
    }

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
    }
}

} // namespace processor
} // namespace lbug
