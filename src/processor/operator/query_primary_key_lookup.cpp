#include "processor/operator/query_primary_key_lookup.h"

#include <algorithm>

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
        // The key evaluator's result vector may own a DataChunkState that is independent of the
        // input chunk. When the key is not a direct reference to an in-scope expression (e.g. an
        // implicit CAST wrapped around a correlated variable, such as comparing a SERIAL primary
        // key with an INT64 UNWIND element), ExpressionMapper hands back an evaluator whose
        // already-flat children give it a fresh state pinned at position 0, so its selection does
        // not track the rows exposed by the child operator. The evaluator contract aligns the i-th
        // selected entry of a result with the i-th selected entry of each operand, so zip the two
        // selections by index and drive everything from the input chunk's selection: indexing the
        // node-id vector with the key result's positions resolved node IDs into slot 0 and
        // collapsed the shared output selection to [0], freezing every downstream read of
        // pre-lookup variables (and any later lookup's key) at the first input row.
        // If the key result shares this state, both selections are the same object; writing
        // outputSelVector[outputSize] with outputSize <= i only rewrites slots at or before the
        // one being read, so no snapshot is needed.
        const auto& inputSelVector = nodeIDVector->state->getSelVector();
        const auto& keySelVector = keyVector->state->getSelVector();
        const auto numRows =
            std::min<sel_t>(inputSelVector.getSelSize(), keySelVector.getSelSize());
        auto& outputSelVector = nodeIDVector->state->getSelVectorUnsafe();
        outputSelVector.setToFiltered();
        outputSize = 0;
        for (sel_t i = 0; i < numRows; ++i) {
            const auto inputPos = inputSelVector[i];
            const auto keyPos = keySelVector[i];
            if (keyVector->isNull(keyPos)) {
                continue;
            }
            offset_t offset;
            if (!table->lookupPK(transaction, keyVector, keyPos, offset)) {
                continue;
            }
            nodeIDVector->setValue<nodeID_t>(inputPos, {offset, table->getTableID()});
            outputSelVector[outputSize++] = inputPos;
        }
        outputSelVector.setSelSize(outputSize);
    } while (outputSize == 0);
    table->lookupMultiple(transaction, *scanState);
    tableInfo.castColumns();
    // The out vectors live in the same group as the node-id vector, whose state is the one the
    // child drives -- marking it unflat here would leak that flip into the child's next batch (e.g.
    // a hash join probe expecting flat keys). The group's flatness is already managed by the
    // operators below us.
    if (scanState->outState != nodeIDVector->state) {
        scanState->outState->setToUnflat();
    }
    metrics->numOutputTuple.increase(outputSize);
    return true;
}

} // namespace processor
} // namespace lbug
