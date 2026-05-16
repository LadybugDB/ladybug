#include "processor/operator/index_lookup.h"

#include <mutex>
#include <unordered_map>

#include "binder/expression/expression_util.h"
#include "common/assert.h"
#include "common/exception/message.h"
#include "common/type_utils.h"
#include "common/types/types.h"
#include "common/utils.h"
#include "common/vector/value_vector.h"
#include "main/client_context.h"
#include "processor/warning_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/index/hash_index.h"
#include "storage/storage_utils.h"
#include "storage/table/node_group.h"
#include "storage/table/node_table.h"
#include "storage/table/table.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace processor {

namespace {

template<typename T>
using StoredPKValue = std::conditional_t<std::same_as<T, string_t>, std::string, T>;

template<typename T>
StoredPKValue<T> readPKValue(const ValueVector& vector, sel_t pos) {
    if constexpr (std::same_as<T, string_t>) {
        return vector.getValue<string_t>(pos).getAsString();
    } else {
        return vector.getValue<T>(pos);
    }
}

template<typename T>
struct PKHash {
    size_t operator()(const T& value) const { return std::hash<T>{}(value); }
};

template<>
struct PKHash<int128_t> {
    size_t operator()(const int128_t& value) const {
        return std::hash<uint64_t>{}(value.low) ^ (std::hash<int64_t>{}(value.high) << 1);
    }
};

template<>
struct PKHash<uint128_t> {
    size_t operator()(const uint128_t& value) const {
        return std::hash<uint64_t>{}(value.low) ^ (std::hash<uint64_t>{}(value.high) << 1);
    }
};

template<typename T>
struct NoIndexLookupCacheImpl final : NoIndexLookupCache {
    void buildIfNeeded(NodeTable* nodeTable, transaction::Transaction* transaction,
        main::ClientContext* context) override {
        std::lock_guard lck{mtx};
        if (built) {
            return;
        }
        offsets.reserve(nodeTable->getNumTotalRows(transaction));
        std::vector<LogicalType> dataTypes;
        dataTypes.push_back(nodeTable->getColumn(nodeTable->getPKColumnID()).getDataType().copy());
        auto dataChunk =
            Table::constructDataChunk(MemoryManager::Get(*context), std::move(dataTypes));
        std::vector<ValueVector*> outVectors = {&dataChunk.getValueVectorMutable(0)};
        auto scanState =
            std::make_unique<NodeTableScanState>(nullptr, std::move(outVectors), dataChunk.state);
        scanState->source = TableScanSource::COMMITTED;
        scanState->setToTable(transaction, nodeTable, {nodeTable->getPKColumnID()}, {});
        const auto numNodeGroups = nodeTable->getNumNodeGroups();
        for (node_group_idx_t nodeGroupIdx = 0; nodeGroupIdx < numNodeGroups; ++nodeGroupIdx) {
            auto* nodeGroup = nodeTable->getNodeGroupNoLock(nodeGroupIdx);
            if (nodeGroup->getNumChunkedGroups() == 0) {
                continue;
            }
            scanState->nodeGroup = nodeGroup;
            scanState->nodeGroupIdx = nodeGroupIdx;
            nodeGroup->initializeScanState(transaction, *scanState);
            while (true) {
                const auto scanResult = nodeGroup->scan(transaction, *scanState);
                if (scanResult == NODE_GROUP_SCAN_EMPTY_RESULT) {
                    break;
                }
                auto* scannedVector = scanState->outputVectors[0];
                for (idx_t i = 0; i < scannedVector->state->getSelSize(); ++i) {
                    const auto pos = scannedVector->state->getSelVector()[i];
                    if (scannedVector->isNull(pos)) {
                        continue;
                    }
                    const auto offset = StorageUtils::getStartOffsetOfNodeGroup(nodeGroupIdx) +
                                        scanResult.startRow + pos;
                    if (nodeTable->isVisibleNoLock(transaction, offset)) {
                        offsets.emplace(readPKValue<T>(*scannedVector, pos), offset);
                    }
                }
            }
        }
        built = true;
    }

    bool lookup(ValueVector* keyVector, sel_t pos, offset_t& result) const override {
        auto entry = offsets.find(readPKValue<T>(*keyVector, pos));
        if (entry == offsets.end()) {
            return false;
        }
        result = entry->second;
        return true;
    }

    std::mutex mtx;
    bool built = false;
    // Temporary in-memory lookup index for no-hash-index rel COPY. This can be replaced by an
    // on-disk persistent index in the future; until then, rel ingest is limited by the node PKs
    // that fit in RAM.
    std::unordered_map<StoredPKValue<T>, offset_t, PKHash<StoredPKValue<T>>> offsets;
};

std::shared_ptr<NoIndexLookupCache> createNoIndexLookupCache(const LogicalType& pkType) {
    return TypeUtils::visit(
        pkType,
        []<IndexHashable T>(T) -> std::shared_ptr<NoIndexLookupCache> {
            return std::make_shared<NoIndexLookupCacheImpl<T>>();
        },
        [](auto) -> std::shared_ptr<NoIndexLookupCache> { UNREACHABLE_CODE; });
}

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
                auto found = info.noIndexLookupCache ?
                                 info.noIndexLookupCache->lookup(keyVector, pos, lookupOffset) :
                                 false;
                if (!found) {
                    found = info.nodeTable->lookupPK(transaction, keyVector, pos, lookupOffset);
                }
                if (!found) {
                    errorHandler->handleError(ExceptionMessage::nonExistentPKException(
                                                  keyVector->getAsValue(pos)->toString()),
                        getWarningSourceData(warningDataVectors, pos));
                } else {
                    resultManager.insertEntry(lookupOffset, pos);
                }
            }
        },
        [&](auto) { UNREACHABLE_CODE; });
}

template<bool hasNoNullsGuarantee>
void fillOffsetArraysFromVector(transaction::Transaction* transaction, const IndexLookupInfo& info,
    ValueVector* keyVector, ValueVector* resultVector,
    const std::vector<ValueVector*>& warningDataVectors, BatchInsertErrorHandler* errorHandler) {
    DASSERT(resultVector->dataType.getPhysicalType() == PhysicalTypeID::INT64);
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

IndexLookup::IndexLookup(std::vector<IndexLookupInfo> infos,
    std::vector<DataPos> warningDataVectorPos, std::unique_ptr<PhysicalOperator> child, idx_t id,
    std::unique_ptr<OPPrintInfo> printInfo)
    : PhysicalOperator{type_, std::move(child), id, std::move(printInfo)}, infos{std::move(infos)},
      warningDataVectorPos{std::move(warningDataVectorPos)} {
    std::unordered_map<NodeTable*, std::shared_ptr<NoIndexLookupCache>> noIndexLookupCaches;
    for (auto& info : this->infos) {
        if (!info.nodeTable->tryGetPrimaryKeyIndex()) {
            auto& cache = noIndexLookupCaches[info.nodeTable];
            if (!cache) {
                cache = createNoIndexLookupCache(
                    info.nodeTable->getColumn(info.nodeTable->getPKColumnID()).getDataType());
            }
            info.noIndexLookupCache = cache;
        }
    }
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
        if (info.noIndexLookupCache) {
            info.noIndexLookupCache->buildIfNeeded(info.nodeTable,
                transaction::Transaction::Get(*context->clientContext), context->clientContext);
        }
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
