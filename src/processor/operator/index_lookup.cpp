#include "processor/operator/index_lookup.h"

#include "binder/expression/expression_util.h"
#include "common/assert.h"
#include "common/exception/message.h"
#include "common/types/types.h"
#include "common/utils.h"
#include "common/vector/value_vector.h"
#include "processor/warning_context.h"
#include "storage/index/hash_index.h"
#include "storage/table/node_table.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace processor {

namespace {

std::optional<WarningSourceData> getWarningSourceData(
    const std::vector<ValueVector*>& warningDataVectors, sel_t pos) {
    std::optional<WarningSourceData> ret;
    if (!warningDataVectors.empty()) {
        ret.emplace(WarningSourceData::constructFromData(warningDataVectors,
            safeIntegerConversion<idx_t>(pos)));
    }
    return ret;
}

bool checkNullKey(ValueVector* keyVector, offset_t vectorOffset,
    BatchInsertErrorHandler* errorHandler, const std::vector<ValueVector*>& warningDataVectors) {
    bool isNull = keyVector->isNull(vectorOffset);
    if (isNull) {
        errorHandler->handleError(ExceptionMessage::nullPKException(),
            getWarningSourceData(warningDataVectors, vectorOffset));
    }
    return !isNull;
}

struct OffsetVectorManager {
    OffsetVectorManager(ValueVector* resultVector, BatchInsertErrorHandler* errorHandler)
        : ignoreErrors(errorHandler->getIgnoreErrors()), resultVector(resultVector),
          insertOffset(0) {
        // if we are ignoring errors we may need to filter the output sel vector
        if (ignoreErrors) {
            resultVector->state->getSelVectorUnsafe().setToFiltered();
        }
    }

    ~OffsetVectorManager() {
        if (ignoreErrors) {
            resultVector->state->getSelVectorUnsafe().setSelSize(insertOffset);
        }
    }

    void insertEntry(offset_t entry, sel_t posInKeyVector) {
        auto* offsets = reinterpret_cast<offset_t*>(resultVector->getData());
        offsets[posInKeyVector] = entry;
        if (ignoreErrors) {
            // if the lookup was successful we may add the current entry to the output selection
            resultVector->state->getSelVectorUnsafe()[insertOffset] = posInKeyVector;
        }
        ++insertOffset;
    }

    bool ignoreErrors;
    ValueVector* resultVector;

    offset_t insertOffset;
};

template<bool hasNoNullsGuarantee>
void fillOffsetArraysFromVectorInternal(transaction::Transaction* transaction,
    const IndexLookupInfo& info, ValueVector* keyVector, ValueVector* resultVector,
    const std::vector<ValueVector*>& warningDataVectors, BatchInsertErrorHandler* errorHandler,
    const sel_t* selVector, sel_t numKeys) {
    TypeUtils::visit(
        keyVector->dataType.getPhysicalType(),
        [&]<IndexHashable T>(T) {
            OffsetVectorManager resultManager{resultVector, errorHandler};
            for (sel_t i = 0u; i < numKeys; i++) {
                auto pos = selVector ? selVector[i] : i;
                if constexpr (!hasNoNullsGuarantee) {
                    if (!checkNullKey(keyVector, pos, errorHandler, warningDataVectors)) {
                        continue;
                    }
                }
                offset_t lookupOffset = 0;
                if (!info.nodeTable->lookupPK(transaction, keyVector, pos, lookupOffset)) {
                    errorHandler->handleError(ExceptionMessage::nonExistentPKException(
                                                  keyVector->getAsValue(pos)->toString()),
                        getWarningSourceData(warningDataVectors, pos));
                } else {
                    resultManager.insertEntry(lookupOffset, pos);
                }
            }
        },
        [&](auto) { KU_UNREACHABLE; });
}

template<bool hasNoNullsGuarantee>
void fillOffsetArraysFromVector(transaction::Transaction* transaction, const IndexLookupInfo& info,
    ValueVector* keyVector, ValueVector* resultVector,
    const std::vector<ValueVector*>& warningDataVectors, BatchInsertErrorHandler* errorHandler) {
    KU_ASSERT(resultVector->dataType.getPhysicalType() == PhysicalTypeID::INT64);
    auto& selVector = keyVector->state->getSelVector();
    auto numKeys = selVector.getSelSize();
    if (selVector.isUnfiltered()) {
        // Fast path: selection vector is unfiltered - pass a null selection vector
        fillOffsetArraysFromVectorInternal<hasNoNullsGuarantee>(transaction, info, keyVector,
            resultVector, warningDataVectors, errorHandler, nullptr /* selVector */, numKeys);
    } else {
        // Filtered case: copy selection positions since we may modify the selection vector
        std::vector<sel_t> lookupPos(numKeys);
        for (idx_t i = 0; i < numKeys; ++i) {
            lookupPos[i] = selVector[i];
        }
        fillOffsetArraysFromVectorInternal<hasNoNullsGuarantee>(transaction, info, keyVector,
            resultVector, warningDataVectors, errorHandler, lookupPos.data(), numKeys);
    }
}
} // namespace

std::string IndexLookupPrintInfo::toString() const {
    std::string result = "Indexes: ";
    result += binder::ExpressionUtil::toString(expressions);
    return result;
}

bool IndexLookup::getNextTuplesInternal(ExecutionContext* context) {
    if (!children[0]->getNextTuple(context)) {
        return false;
    }
    for (auto& info : infos) {
        info.keyEvaluator->evaluate();
        lookup(transaction::Transaction::Get(*context->clientContext), info);
    }
    localState->errorHandler->flushStoredErrors();
    return true;
}

void IndexLookup::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    auto errorHandler = std::make_unique<BatchInsertErrorHandler>(context,
        WarningContext::Get(*context->clientContext)->getIgnoreErrorsOption());
    localState = std::make_unique<IndexLookupLocalState>(std::move(errorHandler));
    for (auto& pos : warningDataVectorPos) {
        localState->warningDataVectors.push_back(resultSet->getValueVector(pos).get());
    }
    for (auto& info : infos) {
        info.keyEvaluator->init(*resultSet, context->clientContext);
    }
}

void IndexLookup::lookup(transaction::Transaction* transaction, const IndexLookupInfo& info) {
    auto keyVector = info.keyEvaluator->resultVector.get();
    auto resultVector = resultSet->getValueVector(info.resultVectorPos).get();

    if (keyVector->hasNoNullsGuarantee()) {
        fillOffsetArraysFromVector<true>(transaction, info, keyVector, resultVector,
            localState->warningDataVectors, localState->errorHandler.get());
    } else {
        fillOffsetArraysFromVector<false>(transaction, info, keyVector, resultVector,
            localState->warningDataVectors, localState->errorHandler.get());
    }
}

} // namespace processor
} // namespace lbug
