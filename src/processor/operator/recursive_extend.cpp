#include "processor/operator/recursive_extend.h"

#include "binder/expression/node_expression.h"
#include "binder/expression/property_expression.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/task_system/progress_bar.h"
#include "function/gds/compute.h"
#include "function/gds/gds_function_collection.h"
#include "function/gds/gds_utils.h"
#include "processor/execution_context.h"
#include "processor/result/factorized_table.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::function;

namespace lbug {
namespace processor {

// All recursive join computation have the same vertex compute. This vertex compute writes
// result (could be dst, length or path) from a dst node ID to given source node ID.
class RJVertexCompute : public VertexCompute {
public:
    RJVertexCompute(storage::MemoryManager* mm, RecursiveExtendSharedState* sharedState,
        std::unique_ptr<RJOutputWriter> writer, table_id_set_t nbrTableIDSet)
        : mm{mm}, sharedState{sharedState}, writer{std::move(writer)},
          nbrTableIDSet{std::move(nbrTableIDSet)} {
        localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    }
    ~RJVertexCompute() override { sharedState->factorizedTablePool.returnLocalTable(localFT); }

    bool beginOnTable(table_id_t tableID) override {
        // Nbr node table IDs might be different from graph node table IDs
        // See comment below in RecursiveExtend::executeInternal.
        if (!nbrTableIDSet.contains(tableID)) {
            return false;
        }
        writer->beginWriting(tableID);
        return true;
    }

    void vertexCompute(offset_t startOffset, offset_t endOffset, table_id_t tableID) override {
        for (auto i = startOffset; i < endOffset; ++i) {
            if (sharedState->exceedLimit()) {
                return;
            }
            auto nodeID = nodeID_t{i, tableID};
            writer->write(*localFT, nodeID, sharedState->counter.get());
        }
    }

    void vertexCompute(table_id_t tableID) override {
        writer->write(*localFT, tableID, sharedState->counter.get());
    }

    std::unique_ptr<VertexCompute> copy() override {
        return std::make_unique<RJVertexCompute>(mm, sharedState, writer->copy(), nbrTableIDSet);
    }

private:
    storage::MemoryManager* mm;
    // Shared state storing ftables to materialize output.
    RecursiveExtendSharedState* sharedState;
    FactorizedTable* localFT;
    std::unique_ptr<RJOutputWriter> writer;
    table_id_set_t nbrTableIDSet;
};

static double getRJProgress(offset_t totalNumNodes, offset_t completedNumNodes) {
    if (totalNumNodes == 0) {
        return 0;
    }
    return (double)completedNumNodes / totalNumNodes;
}

static bool requireRelID(const RJAlgorithm& function) {
    if (function.getFunctionName() == WeightedSPPathsFunction::name ||
        function.getFunctionName() == SingleSPPathsFunction::name ||
        function.getFunctionName() == AllSPPathsFunction::name ||
        function.getFunctionName() == AllWeightedSPPathsFunction::name ||
        function.getFunctionName() == VarLenJoinsFunction::name) {
        return true;
    }
    return false;
}

static bool canUseFunctionalChainFastPath(const RJAlgorithm& function, const RJBindData& bindData,
    RecursiveExtendSharedState* sharedState) {
    if (function.getFunctionName() != VarLenJoinsFunction::name ||
        bindData.extendDirection != ExtendDirection::FWD || bindData.writePath ||
        bindData.semantic != PathSemantic::WALK || sharedState->getPathNodeMaskMap() != nullptr) {
        return false;
    }
    const auto inputTableIDs = bindData.nodeInput->constCast<NodeExpression>().getTableIDs();
    const auto outputTableIDs = bindData.nodeOutput->constCast<NodeExpression>().getTableIDs();
    if (inputTableIDs.size() != 1 || outputTableIDs.size() != 1 ||
        inputTableIDs[0] != outputTableIDs[0]) {
        return false;
    }
    const auto relInfos = sharedState->graph->getRelInfos(inputTableIDs[0]);
    if (relInfos.size() != 1) {
        return false;
    }
    const auto& relInfo = relInfos[0];
    const auto* relGroup = relInfo.relGroupEntry->ptrCast<catalog::RelGroupCatalogEntry>();
    return relInfo.srcTableID == inputTableIDs[0] && relInfo.dstTableID == inputTableIDs[0] &&
           relGroup->getNumRelTables() == 1;
}

static void appendFunctionalChainRows(FactorizedTable& localTable,
    const std::vector<ValueVector*>& vectors, SelectionVector& selVector, uint64_t& numRows) {
    if (numRows == 0) {
        return;
    }
    selVector.setToUnfiltered(numRows);
    localTable.append(vectors);
    numRows = 0;
}

static bool tryExecuteFunctionalChainFastPath(ExecutionContext* context,
    const RJAlgorithm& function, const RJBindData& bindData,
    RecursiveExtendSharedState* sharedState) {
    if (!canUseFunctionalChainFastPath(function, bindData, sharedState)) {
        return false;
    }
    auto clientContext = context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto graph = sharedState->graph.get();
    const auto nodeTableID = bindData.nodeInput->constCast<NodeExpression>().getTableIDs()[0];
    const auto maxOffset = graph->getMaxOffset(transaction, nodeTableID);
    const auto relInfo = graph->getRelInfos(nodeTableID)[0];

    std::vector<offset_t> nextOffsets(maxOffset, INVALID_OFFSET);
    auto relScanState = graph->prepareRelScan(*relInfo.relGroupEntry, relInfo.relTableID,
        nodeTableID, {} /* propertyColumnIDs */);
    auto isFunctionalChain = true;
    for (offset_t offset = 0; offset < maxOffset; ++offset) {
        const auto srcNodeID = nodeID_t{offset, nodeTableID};
        for (auto chunk : graph->scanFwd(srcNodeID, *relScanState)) {
            chunk.forEach([&](auto neighbors, auto, auto i) {
                const auto nbr = neighbors[i];
                if (nbr.tableID != nodeTableID || nbr.offset >= maxOffset) {
                    return;
                }
                if (nextOffsets[offset] != INVALID_OFFSET) {
                    isFunctionalChain = false;
                    return;
                }
                nextOffsets[offset] = nbr.offset;
            });
        }
        if (!isFunctionalChain) {
            return false;
        }
    }

    auto mm = storage::MemoryManager::Get(*clientContext);
    auto localTable = sharedState->factorizedTablePool.claimLocalTable(mm);
    auto state = std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY);
    auto srcVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm, state);
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm, state);
    auto lengthVector = std::make_unique<ValueVector>(LogicalType::UINT16(), mm, state);
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), lengthVector.get()};
    auto& selVector = state->getSelVectorUnsafe();
    uint64_t numRows = 0;

    auto outputMask = sharedState->getOutputNodeMaskMap();
    if (outputMask != nullptr) {
        outputMask->pin(nodeTableID);
    }
    const auto outputNodeAllowed = [&](offset_t offset) {
        if (outputMask == nullptr || !outputMask->hasPinnedMask() ||
            !outputMask->getPinnedMask()->isEnabled()) {
            return true;
        }
        return outputMask->getPinnedMask()->isMasked(offset);
    };
    const auto appendRow = [&](nodeID_t srcNodeID, nodeID_t dstNodeID, uint16_t length) {
        if (!outputNodeAllowed(dstNodeID.offset)) {
            return false;
        }
        srcVector->setValue<nodeID_t>(numRows, srcNodeID);
        dstVector->setValue<nodeID_t>(numRows, dstNodeID);
        lengthVector->setValue<uint16_t>(numRows, length);
        ++numRows;
        if (numRows == DEFAULT_VECTOR_CAPACITY) {
            appendFunctionalChainRows(*localTable, vectors, selVector, numRows);
        }
        if (sharedState->counter != nullptr) {
            sharedState->counter->increase(1);
            return sharedState->counter->exceedLimit();
        }
        return false;
    };

    auto inputNodeMaskMap = sharedState->getInputNodeMaskMap();
    auto nodeTable = storage::StorageManager::Get(*clientContext)
                         ->getTable(nodeTableID)
                         ->ptrCast<storage::NodeTable>();
    const auto visitSource = [&](offset_t offset) {
        const auto srcNodeID = nodeID_t{offset, nodeTableID};
        if (bindData.lowerBound == 0 && appendRow(srcNodeID, srcNodeID, 0)) {
            return true;
        }
        auto nextOffset = nextOffsets[offset];
        auto length = 1u;
        for (; length < bindData.lowerBound && nextOffset != INVALID_OFFSET; ++length) {
            nextOffset = nextOffsets[nextOffset];
        }
        for (; length <= bindData.upperBound && nextOffset != INVALID_OFFSET; ++length) {
            if (appendRow(srcNodeID, nodeID_t{nextOffset, nodeTableID}, length)) {
                return true;
            }
            nextOffset = nextOffsets[nextOffset];
        }
        return false;
    };
    if (inputNodeMaskMap != nullptr && inputNodeMaskMap->containsTableID(nodeTableID) &&
        inputNodeMaskMap->getOffsetMask(nodeTableID)->isEnabled()) {
        auto mask = inputNodeMaskMap->getOffsetMask(nodeTableID);
        for (const auto offset : mask->range(0, maxOffset)) {
            if (visitSource(offset)) {
                break;
            }
        }
    } else {
        for (offset_t offset = 0; offset < maxOffset; ++offset) {
            if (!nodeTable->isVisible(transaction, offset)) {
                continue;
            }
            if (visitSource(offset)) {
                break;
            }
        }
    }
    appendFunctionalChainRows(*localTable, vectors, selVector, numRows);
    sharedState->factorizedTablePool.returnLocalTable(localTable);
    sharedState->factorizedTablePool.mergeLocalTables();
    return true;
}

void RecursiveExtend::executeInternal(ExecutionContext* context) {
    if (tryExecuteFunctionalChainFastPath(context, *function, bindData, sharedState.get())) {
        return;
    }
    auto clientContext = context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto progressBar = ProgressBar::Get(*clientContext);
    auto graph = sharedState->graph.get();
    auto inputNodeMaskMap = sharedState->getInputNodeMaskMap();
    offset_t totalNumNodes = 0;
    if (inputNodeMaskMap != nullptr) {
        totalNumNodes = inputNodeMaskMap->getNumMaskedNode();
    } else {
        for (auto& tableID : graph->getNodeTableIDs()) {
            auto nodeTable = storage::StorageManager::Get(*clientContext)
                                 ->getTable(tableID)
                                 ->ptrCast<storage::NodeTable>();
            auto maxOffset = graph->getMaxOffset(transaction, tableID);
            for (auto offset = 0u; offset < maxOffset; ++offset) {
                totalNumNodes += nodeTable->isVisible(transaction, offset);
            }
        }
    }
    std::vector<std::string> propertyNames;
    if (requireRelID(*function)) {
        propertyNames.push_back(InternalKeyword::ID);
    }
    if (bindData.weightPropertyExpr != nullptr) {
        propertyNames.push_back(
            bindData.weightPropertyExpr->ptrCast<PropertyExpression>()->getPropertyName());
    }
    offset_t completedNumNodes = 0;
    auto inputNodeTableIDSet = bindData.nodeInput->constCast<NodeExpression>().getTableIDsSet();
    for (auto& tableID : graph->getNodeTableIDs()) {
        // Input node table IDs could be different from graph node table IDs, e.g.
        // Given schema, student-knows->student, teacher-knows->teacher
        // MATCH (a:student)-[e*knows]->(b:student)
        // the graph node table IDs will include both student and teacher.
        if (!inputNodeTableIDSet.contains(tableID)) {
            continue;
        }
        auto calcFunc = [tableID, propertyNames, graph, context, this](offset_t offset) {
            auto clientContext = context->clientContext;
            auto computeState = function->getComputeState(context, bindData, sharedState.get());
            auto sourceNodeID = nodeID_t{offset, tableID};
            computeState->initSource(sourceNodeID);
            GDSUtils::runRecursiveJoinEdgeCompute(context, *computeState, graph,
                bindData.extendDirection, bindData.upperBound, sharedState->getOutputNodeMaskMap(),
                propertyNames);
            auto writer = function->getOutputWriter(context, bindData, *computeState, sourceNodeID,
                sharedState.get());
            auto vertexCompute = std::make_unique<RJVertexCompute>(
                storage::MemoryManager::Get(*clientContext), sharedState.get(), writer->copy(),
                bindData.nodeOutput->constCast<NodeExpression>().getTableIDsSet());
            GDSUtils::runVertexCompute(context, computeState->frontierPair->getState(), graph,
                *vertexCompute);
        };
        auto maxOffset = graph->getMaxOffset(transaction, tableID);
        if (inputNodeMaskMap && inputNodeMaskMap->getOffsetMask(tableID)->isEnabled()) {
            for (const auto& offset :
                inputNodeMaskMap->getOffsetMask(tableID)->range(0, maxOffset)) {
                calcFunc(offset);
                progressBar->updateProgress(context->queryID,
                    getRJProgress(totalNumNodes, completedNumNodes++));
                if (sharedState->exceedLimit()) {
                    break;
                }
            }
        } else {
            auto nodeTable = storage::StorageManager::Get(*clientContext)
                                 ->getTable(tableID)
                                 ->ptrCast<storage::NodeTable>();
            for (auto offset = 0u; offset < maxOffset; ++offset) {
                if (!nodeTable->isVisible(transaction, offset)) {
                    continue;
                }
                calcFunc(offset);
                progressBar->updateProgress(context->queryID,
                    getRJProgress(totalNumNodes, completedNumNodes++));
                if (sharedState->exceedLimit()) {
                    break;
                }
            }
        }
    }
    sharedState->factorizedTablePool.mergeLocalTables();
}

} // namespace processor
} // namespace lbug
