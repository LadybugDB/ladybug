#include "processor/operator/unwind_dedup.h"

#include "common/vector/value_vector.h"
#include "function/hash/vector_hash_functions.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"

using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::storage;

namespace lbug {
namespace processor {

void UnwindDedup::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* /*context*/) {
    keyVectors.clear();
    keyVectors.reserve(keyDataPositions.size());
    for (auto& keyDataPos : keyDataPositions) {
        keyVectors.push_back(resultSet->getValueVector(keyDataPos).get());
    }
}

bool UnwindDedup::getNextTuplesInternal(ExecutionContext* context) {
    while (true) {
        if (!children[0]->getNextTuple(context)) {
            return false;
        }

        // Compute hash for the key vector
        DASSERT(!keyVectors.empty());
        const auto& selVector = keyVectors[0]->state->getSelVector();
        if (selVector.getSelSize() == 0) {
            continue;
        }

        auto hashVector = std::make_unique<ValueVector>(LogicalType::HASH(),
            MemoryManager::Get(*context->clientContext));
        hashVector->state = keyVectors[0]->state;
        VectorHashFunction::computeHash(*keyVectors[0], selVector, *hashVector, selVector);
        if (keyVectors.size() > 1) {
            auto tmpHashVector = std::make_unique<ValueVector>(LogicalType::HASH(),
                MemoryManager::Get(*context->clientContext));
            tmpHashVector->state = keyVectors[0]->state;
            for (auto i = 1u; i < keyVectors.size(); ++i) {
                DASSERT(keyVectors[i]->state == keyVectors[0]->state);
                VectorHashFunction::computeHash(*keyVectors[i], selVector, *tmpHashVector,
                    selVector);
                VectorHashFunction::combineHash(*hashVector, selVector, *tmpHashVector, selVector,
                    *hashVector, selVector);
            }
        }

        // Filter out duplicate values by modifying the selection vector
        auto hashData = reinterpret_cast<hash_t*>(hashVector->getData());
        sel_t newSelectedPositions[DEFAULT_VECTOR_CAPACITY];
        auto newSelSize = 0u;

        for (auto i = 0u; i < selVector.getSelSize(); i++) {
            auto pos = selVector[i];
            auto hash = hashData[pos];

            if (seenHashes.find(hash) == seenHashes.end()) {
                seenHashes.insert(hash);
                newSelectedPositions[newSelSize++] = pos;
            }
        }

        if (newSelSize > 0) {
            // Update the selection vector to only include non-duplicate values
            auto& selVectorUnsafe = keyVectors[0]->state->getSelVectorUnsafe();
            selVectorUnsafe.setToFiltered(newSelSize);
            auto buffer = selVectorUnsafe.getMutableBuffer();
            for (auto i = 0u; i < newSelSize; i++) {
                buffer[i] = newSelectedPositions[i];
            }
            return true;
        }
        // If all values were duplicates, continue to next batch
    }
}

} // namespace processor
} // namespace lbug
