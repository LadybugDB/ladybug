#include "processor/operator/query_primary_key_lookup.h"

#include "binder/expression/expression_util.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace processor {

std::string QueryPrimaryKeyLookupPrintInfo::toString() const {
    std::string result = "Table: " + tableName;
    result += ", Key: " + key;
    if (!alias.empty()) {
        result += ", Alias: " + alias;
    }
    if (!properties.empty()) {
        result += ", Properties: " + binder::ExpressionUtil::toString(properties);
    }
    return result;
}

void QueryPrimaryKeyLookup::initLocalStateInternal(ResultSet* resultSet,
    ExecutionContext* context) {
    nodeIDVector = resultSet->getValueVector(opInfo.nodeIDPos).get();
    for (auto& pos : opInfo.outVectorsPos) {
        outVectors.push_back(resultSet->getValueVector(pos).get());
    }
    scanState = createNodeTableScanState(table, nodeIDVector, outVectors,
        MemoryManager::Get(*context->clientContext));
    tableInfo.initScanState(*scanState, outVectors, context->clientContext);
    keyEvaluator->init(*resultSet, context->clientContext);
}

bool QueryPrimaryKeyLookup::getNextTuplesInternal(ExecutionContext* context) {
    auto transaction = transaction::Transaction::Get(*context->clientContext);
    sel_t outputSize = 0;
    do {
        // The node-id vector shares its DataChunkState with the child (the key expression lives
        // in the same group). Restoring the saved selection vector hands control back to the
        // child so it can advance, and saving afterwards swaps in a private selection vector we
        // are free to rewrite. Without this restore/save dance, filtering the selection vector
        // here would clobber the child's state (e.g. Flatten's currentSelVector) and leave it
        // reporting an empty selection on the next call.
        restoreSelVector(*nodeIDVector->state);
        if (!children[0]->getNextTuple(context)) {
            return false;
        }
        saveSelVector(*nodeIDVector->state);
        keyEvaluator->evaluate();
        auto* keyVector = keyEvaluator->resultVector.get();
        const auto& inputSelVector = keyVector->state->getSelVector();
        auto& outputSelVector = nodeIDVector->state->getSelVectorUnsafe();
        outputSelVector.setToFiltered();
        outputSize = 0;
        for (sel_t i = 0; i < inputSelVector.getSelSize(); ++i) {
            const auto pos = inputSelVector[i];
            if (keyVector->isNull(pos)) {
                continue;
            }
            offset_t offset;
            if (!table->lookupPK(transaction, keyVector, pos, offset)) {
                continue;
            }
            nodeIDVector->setValue<nodeID_t>(pos, {offset, table->getTableID()});
            outputSelVector[outputSize++] = pos;
        }
        outputSelVector.setSelSize(outputSize);
    } while (outputSize == 0);
    table->lookupMultiple(transaction, *scanState);
    tableInfo.castColumns();
    scanState->outState->setToUnflat();
    metrics->numOutputTuple.increase(outputSize);
    return true;
}

} // namespace processor
} // namespace lbug
