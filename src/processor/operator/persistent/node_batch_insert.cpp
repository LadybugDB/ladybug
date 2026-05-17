#include "processor/operator/persistent/node_batch_insert.h"

#include <mutex>
#include <set>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/cast.h"
#include "common/data_chunk/data_chunk_state.h"
#include "common/exception/message.h"
#include "common/exception/runtime.h"
#include "common/finally_wrapper.h"
#include "common/type_utils.h"
#include "common/vector/value_vector.h"
#include "processor/execution_context.h"
#include "processor/operator/persistent/index_builder.h"
#include "processor/result/factorized_table_util.h"
#include "processor/warning_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_storage.h"
#include "storage/storage_manager.h"
#include "storage/table/chunked_node_group.h"
#include "storage/table/node_table.h"
#include "storage/table/string_chunk_data.h"
#include "transaction/transaction.h"
#include <format>

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace processor {

namespace {

template<typename T>
using StoredPKValue = std::conditional_t<std::same_as<T, string_t>, std::string, T>;

template<typename T>
StoredPKValue<T> readPKValue(const ColumnChunkData& pkChunk, offset_t pos) {
    if constexpr (std::same_as<T, string_t>) {
        return pkChunk.cast<StringChunkData>().getValue<std::string>(pos);
    } else {
        return pkChunk.getValue<T>(pos);
    }
}

template<typename T>
std::string pkValueToString(const StoredPKValue<T>& value) {
    if constexpr (std::same_as<T, string_t>) {
        return value;
    } else {
        return TypeUtils::toString(value);
    }
}

template<typename T>
struct NoIndexPKValidatorImpl final : NoIndexPKValidator {
    void validate(const ColumnChunkData& pkChunk, offset_t startOffset,
        length_t numValues) override {
        std::lock_guard lck{mtx};
        for (auto i = 0u; i < numValues; ++i) {
            const auto pos = startOffset + i;
            if (pkChunk.isNull(pos)) {
                throw RuntimeException(ExceptionMessage::nullPKException());
            }
            const auto value = readPKValue<T>(pkChunk, pos);
            if (!seenValues.insert(value).second) {
                throw RuntimeException(
                    ExceptionMessage::duplicatePKException(pkValueToString<T>(value)));
            }
        }
    }

    std::mutex mtx;
    // Temporary in-memory uniqueness index for no-hash-index COPY. This can be replaced by an
    // on-disk persistent index in the future; until then, it is a scalability limitation because
    // ingest is limited to the primary keys that fit in RAM.
    std::set<StoredPKValue<T>> seenValues;
};

std::unique_ptr<NoIndexPKValidator> createNoIndexPKValidator(const LogicalType& pkType) {
    return TypeUtils::visit(pkType, []<typename T>(T) -> std::unique_ptr<NoIndexPKValidator> {
        if constexpr (std::same_as<T, bool> || std::same_as<T, int8_t> ||
                      std::same_as<T, int16_t> || std::same_as<T, int32_t> ||
                      std::same_as<T, int64_t> || std::same_as<T, uint8_t> ||
                      std::same_as<T, uint16_t> || std::same_as<T, uint32_t> ||
                      std::same_as<T, uint64_t> || std::same_as<T, int128_t> ||
                      std::same_as<T, uint128_t> || std::same_as<T, float> ||
                      std::same_as<T, double> || std::same_as<T, string_t>) {
            return std::make_unique<NoIndexPKValidatorImpl<T>>();
        } else {
            return nullptr;
        }
    });
}

} // namespace

std::string NodeBatchInsertPrintInfo::toString() const {
    std::string result = "Table Name: ";
    result += tableName;
    return result;
}

void NodeBatchInsertSharedState::initPKIndex(const ExecutionContext* context) {
    auto* nodeTable = dynamic_cast_checked<NodeTable*>(table);
    auto* pkIndex = nodeTable->tryGetPKIndex();
    if (!pkIndex) {
        if (nodeTable->tryGetPrimaryKeyIndex() != nullptr) {
            globalIndexBuilder.reset();
            noIndexPKValidator.reset();
            usePrimaryKeyIndexCommitInsert = true;
            return;
        }
        if (nodeTable->getNumTotalRows(Transaction::Get(*context->clientContext)) != 0) {
            throw RuntimeException(
                "COPY into a non-empty primary-key node table without a hash index is not "
                "supported.");
        }
        globalIndexBuilder.reset();
        noIndexPKValidator = createNoIndexPKValidator(pkType);
        usePrimaryKeyIndexCommitInsert = false;
        if (!noIndexPKValidator) {
            throw RuntimeException(ExceptionMessage::invalidPKType(pkType.toString()));
        }
        return;
    }
    noIndexPKValidator.reset();
    usePrimaryKeyIndexCommitInsert = false;
    globalIndexBuilder = IndexBuilder(std::make_shared<IndexBuilderSharedState>(
        Transaction::Get(*context->clientContext), nodeTable));
}

void NodeBatchInsert::initGlobalStateInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    auto catalog = Catalog::Get(*clientContext);
    auto transaction = Transaction::Get(*clientContext);
    auto nodeTableEntry = catalog->getTableCatalogEntry(transaction, info->tableName)
                              ->ptrCast<NodeTableCatalogEntry>();
    auto nodeTable = StorageManager::Get(*clientContext)->getTable(nodeTableEntry->getTableID());
    const auto& pkDefinition = nodeTableEntry->getPrimaryKeyDefinition();
    auto pkColumnID = nodeTableEntry->getColumnID(pkDefinition.getName());
    // Init info
    info->compressionEnabled = StorageManager::Get(*clientContext)->compressionEnabled();
    auto dataColumnIdx = 0u;
    for (auto& property : nodeTableEntry->getProperties()) {
        info->columnTypes.push_back(property.getType().copy());
        info->insertColumnIDs.push_back(nodeTableEntry->getColumnID(property.getName()));
        info->outputDataColumns.push_back(dataColumnIdx++);
    }
    for (auto& type : info->warningColumnTypes) {
        info->columnTypes.push_back(type.copy());
        info->warningDataColumns.push_back(dataColumnIdx++);
    }
    // Init shared state
    auto nodeSharedState = sharedState->ptrCast<NodeBatchInsertSharedState>();
    nodeSharedState->table = nodeTable;
    nodeSharedState->pkColumnID = pkColumnID;
    nodeSharedState->pkType = pkDefinition.getType().copy();
    nodeSharedState->initPKIndex(context);
}

void NodeBatchInsert::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    const auto nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
    const auto numColumns = nodeInfo->columnEvaluators.size();

    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    localState = std::make_unique<NodeBatchInsertLocalState>(
        std::span{nodeInfo->columnTypes.begin(), nodeInfo->outputDataColumns.size()});
    const auto nodeLocalState = localState->ptrCast<NodeBatchInsertLocalState>();
    if (nodeSharedState->globalIndexBuilder) {
        nodeLocalState->localIndexBuilder = nodeSharedState->globalIndexBuilder->clone();
    }
    nodeLocalState->errorHandler = createErrorHandler(context);
    nodeLocalState->optimisticAllocator =
        Transaction::Get(*context->clientContext)->getLocalStorage()->addOptimisticAllocator();

    nodeLocalState->columnVectors.resize(numColumns);

    for (auto i = 0u; i < numColumns; ++i) {
        auto& evaluator = nodeInfo->columnEvaluators[i];
        evaluator->init(*resultSet, context->clientContext);
        nodeLocalState->columnVectors[i] = evaluator->resultVector.get();
    }
    nodeLocalState->chunkedGroup =
        std::make_unique<InMemChunkedNodeGroup>(*MemoryManager::Get(*context->clientContext),
            nodeInfo->columnTypes, info->compressionEnabled, StorageConfig::NODE_GROUP_SIZE, 0);
    DASSERT(resultSet->dataChunks[0]);
    nodeLocalState->columnState = resultSet->dataChunks[0]->state;
}

void NodeBatchInsert::executeInternal(ExecutionContext* context) {
    const auto clientContext = context->clientContext;
    std::optional<ProducerToken> token;
    auto nodeLocalState = localState->ptrCast<NodeBatchInsertLocalState>();
    if (nodeLocalState->localIndexBuilder) {
        token = nodeLocalState->localIndexBuilder->getProducerToken();
    }
    auto transaction = Transaction::Get(*clientContext);
    while (children[0]->getNextTuple(context)) {
        const auto originalSelVector = nodeLocalState->columnState->getSelVectorShared();
        // Evaluate expressions if needed.
        const auto numTuples = nodeLocalState->columnState->getSelVector().getSelSize();
        evaluateExpressions(numTuples);
        copyToNodeGroup(transaction, MemoryManager::Get(*clientContext)),
            nodeLocalState->columnState->setSelVector(originalSelVector);
    }
    if (nodeLocalState->chunkedGroup->getNumRows() > 0) {
        appendIncompleteNodeGroup(transaction, std::move(nodeLocalState->chunkedGroup),
            nodeLocalState->localIndexBuilder, MemoryManager::Get(*context->clientContext));
    }
    if (nodeLocalState->localIndexBuilder) {
        DASSERT(token);
        token->quit();

        DASSERT(nodeLocalState->errorHandler.has_value());
        nodeLocalState->localIndexBuilder->finishedProducing(nodeLocalState->errorHandler.value());
        nodeLocalState->errorHandler->flushStoredErrors();
    }
    const auto nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
    sharedState->table->cast<NodeTable>().mergeStats(nodeInfo->insertColumnIDs,
        nodeLocalState->stats);
}

void NodeBatchInsert::evaluateExpressions(uint64_t numTuples) const {
    const auto nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
    for (auto i = 0u; i < nodeInfo->evaluateTypes.size(); ++i) {
        switch (nodeInfo->evaluateTypes[i]) {
        case ColumnEvaluateType::DEFAULT: {
            nodeInfo->columnEvaluators[i]->evaluate(numTuples);
        } break;
        case ColumnEvaluateType::CAST: {
            nodeInfo->columnEvaluators[i]->evaluate();
        } break;
        default:
            break;
        }
    }
}

void NodeBatchInsert::copyToNodeGroup(transaction::Transaction* transaction,
    MemoryManager* mm) const {
    auto numAppendedTuples = 0ul;
    const auto nodeLocalState = dynamic_cast_checked<NodeBatchInsertLocalState*>(localState.get());
    const auto numTuplesToAppend = nodeLocalState->columnState->getSelVector().getSelSize();
    while (numAppendedTuples < numTuplesToAppend) {
        const auto numAppendedTuplesInNodeGroup =
            nodeLocalState->chunkedGroup->append(nodeLocalState->columnVectors, numAppendedTuples,
                numTuplesToAppend - numAppendedTuples);
        numAppendedTuples += numAppendedTuplesInNodeGroup;
        if (nodeLocalState->chunkedGroup->isFull()) {
            writeAndResetNodeGroup(transaction, nodeLocalState->chunkedGroup,
                nodeLocalState->localIndexBuilder, mm, *nodeLocalState->optimisticAllocator);
        }
    }
    const auto nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
    nodeLocalState->stats.update(nodeLocalState->columnVectors, nodeInfo->outputDataColumns.size());
    sharedState->incrementNumRows(numAppendedTuples);
}

NodeBatchInsertErrorHandler NodeBatchInsert::createErrorHandler(ExecutionContext* context) const {
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    auto* nodeTable = dynamic_cast_checked<NodeTable*>(sharedState->table);
    return NodeBatchInsertErrorHandler{context, nodeSharedState->pkType.getLogicalTypeID(),
        nodeTable, WarningContext::Get(*context->clientContext)->getIgnoreErrorsOption(),
        sharedState->numErroredRows, &sharedState->erroredRowMutex};
}

static void commitPrimaryKeyIndexInsertions(Transaction* transaction, NodeTable& nodeTable,
    Index& index, const ColumnChunkData& pkChunk, offset_t nodeOffset, length_t numRows,
    main::ClientContext* context) {
    auto state = std::make_shared<DataChunkState>();
    ValueVector nodeIDVector{LogicalType::INTERNAL_ID()};
    ValueVector pkVector{pkChunk.getDataType().copy(), MemoryManager::Get(*context), state};
    nodeIDVector.setState(state);
    auto insertState = index.initInsertState(context, [&nodeTable, transaction](offset_t offset) {
        return nodeTable.isVisible(transaction, offset);
    });
    for (auto start = 0u; start < numRows; start += DEFAULT_VECTOR_CAPACITY) {
        const auto size = std::min<length_t>(DEFAULT_VECTOR_CAPACITY, numRows - start);
        state->getSelVectorUnsafe().setToUnfiltered(size);
        pkChunk.scan(pkVector, start, size);
        for (auto i = 0u; i < size; ++i) {
            nodeIDVector.setValue<nodeID_t>(i, {nodeOffset + start + i, nodeTable.getTableID()});
        }
        index.commitInsert(transaction, nodeIDVector, {&pkVector}, *insertState);
    }
}

void NodeBatchInsert::clearToIndex(MemoryManager* mm,
    std::unique_ptr<InMemChunkedNodeGroup>& nodeGroup, offset_t startIndexInGroup) const {
    // Create a new chunked node group and move the unwritten values to it
    // TODO(bmwinger): Can probably re-use the chunk and shift the values
    const auto oldNodeGroup = std::move(nodeGroup);
    const auto nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
    nodeGroup = std::make_unique<InMemChunkedNodeGroup>(*mm, nodeInfo->columnTypes,
        nodeInfo->compressionEnabled, StorageConfig::NODE_GROUP_SIZE, 0);
    nodeGroup->append(*oldNodeGroup, startIndexInGroup,
        oldNodeGroup->getNumRows() - startIndexInGroup);
}

void NodeBatchInsert::writeAndResetNodeGroup(transaction::Transaction* transaction,
    std::unique_ptr<InMemChunkedNodeGroup>& nodeGroup, std::optional<IndexBuilder>& indexBuilder,
    MemoryManager* mm, PageAllocator& pageAllocator) const {
    const auto nodeLocalState = localState->ptrCast<NodeBatchInsertLocalState>();
    DASSERT(nodeLocalState->errorHandler.has_value());
    writeAndResetNodeGroup(transaction, nodeGroup, indexBuilder, mm,
        nodeLocalState->errorHandler.value(), pageAllocator);
}

void NodeBatchInsert::writeAndResetNodeGroup(transaction::Transaction* transaction,
    std::unique_ptr<InMemChunkedNodeGroup>& nodeGroup, std::optional<IndexBuilder>& indexBuilder,
    MemoryManager* mm, NodeBatchInsertErrorHandler& errorHandler,
    PageAllocator& pageAllocator) const {
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    const auto nodeTable = dynamic_cast_checked<NodeTable*>(sharedState->table);

    uint64_t nodeOffset{};
    uint64_t numRowsWritten{};
    {
        // The chunked group in batch insert may contain extra data to populate error messages
        // When we append to the table we only want the main data so this class is used to slice the
        // original chunked group
        // The slice must be restored even if an exception is thrown to prevent other threads from
        // reading invalid data
        InMemChunkedNodeGroup sliceToWriteToDisk{*nodeGroup, info->outputDataColumns};
        FinallyWrapper sliceRestorer{
            [&]() { nodeGroup->merge(sliceToWriteToDisk, info->outputDataColumns); }};
        std::tie(nodeOffset, numRowsWritten) = nodeTable->appendToLastNodeGroup(transaction,
            info->insertColumnIDs, sliceToWriteToDisk, pageAllocator);
    }

    if (indexBuilder) {
        std::vector<ColumnChunkData*> warningChunkData;
        for (const auto warningDataColumn : info->warningDataColumns) {
            warningChunkData.push_back(&nodeGroup->getColumnChunk(warningDataColumn));
        }
        indexBuilder->insert(nodeGroup->getColumnChunk(nodeSharedState->pkColumnID),
            warningChunkData, nodeOffset, numRowsWritten, errorHandler);
    } else if (nodeSharedState->usePrimaryKeyIndexCommitInsert) {
        auto* index = nodeTable->tryGetPrimaryKeyIndex();
        DASSERT(index != nullptr);
        commitPrimaryKeyIndexInsertions(transaction, *nodeTable, *index,
            nodeGroup->getColumnChunk(nodeSharedState->pkColumnID), nodeOffset, numRowsWritten,
            transaction->getClientContext());
    } else if (nodeSharedState->noIndexPKValidator) {
        nodeSharedState->noIndexPKValidator->validate(
            nodeGroup->getColumnChunk(nodeSharedState->pkColumnID), 0, numRowsWritten);
    }
    if (numRowsWritten == nodeGroup->getNumRows()) {
        nodeGroup->resetToEmpty();
    } else {
        clearToIndex(mm, nodeGroup, numRowsWritten);
    }
}

void NodeBatchInsert::appendIncompleteNodeGroup(transaction::Transaction* transaction,
    std::unique_ptr<InMemChunkedNodeGroup> localNodeGroup,
    std::optional<IndexBuilder>& indexBuilder, MemoryManager* mm) const {
    std::unique_lock xLck{sharedState->mtx};
    const auto nodeLocalState = dynamic_cast_checked<NodeBatchInsertLocalState*>(localState.get());
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    if (!nodeSharedState->sharedNodeGroup) {
        nodeSharedState->sharedNodeGroup = std::move(localNodeGroup);
        return;
    }
    uint64_t numNodesAppended = 0;
    while (numNodesAppended < localNodeGroup->getNumRows()) {
        if (nodeSharedState->sharedNodeGroup->isFull()) {
            writeAndResetNodeGroup(transaction, nodeSharedState->sharedNodeGroup, indexBuilder, mm,
                *nodeLocalState->optimisticAllocator);
        }
        numNodesAppended += nodeSharedState->sharedNodeGroup->append(*localNodeGroup,
            numNodesAppended /* offsetInNodeGroup */,
            localNodeGroup->getNumRows() - numNodesAppended);
    }
    DASSERT(numNodesAppended == localNodeGroup->getNumRows());
}

void NodeBatchInsert::finalize(ExecutionContext* context) {
    DASSERT(localState == nullptr);
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    auto errorHandler = createErrorHandler(context);
    auto clientContext = context->clientContext;
    auto transaction = Transaction::Get(*clientContext);
    auto& pageAllocator = *transaction->getLocalStorage()->addOptimisticAllocator();
    if (nodeSharedState->sharedNodeGroup) {
        while (nodeSharedState->sharedNodeGroup->getNumRows() > 0) {
            writeAndResetNodeGroup(transaction, nodeSharedState->sharedNodeGroup,
                nodeSharedState->globalIndexBuilder, MemoryManager::Get(*clientContext),
                errorHandler, pageAllocator);
        }
    }
    if (nodeSharedState->globalIndexBuilder) {
        nodeSharedState->globalIndexBuilder->finalize(context, errorHandler);
        errorHandler.flushStoredErrors();
    }

    auto& nodeTable = nodeSharedState->table->cast<NodeTable>();
    for (auto& index : nodeTable.getIndexes()) {
        index.finalize(clientContext);
    }
    // we want to flush all index errors before children call finalize
    // as the children (if they are table function calls) are responsible for populating the errors
    // and sending it to the warning context
    PhysicalOperator::finalize(context);

    // if the child is a subquery it will not send the errors to the warning context
    // sends any remaining warnings in this case
    // if the child is a table function call it will have already sent the warnings so this line
    // will do nothing
    WarningContext::Get(*clientContext)->defaultPopulateAllWarnings(context->queryID);
}

void NodeBatchInsert::finalizeInternal(ExecutionContext* context) {
    auto outputMsg = std::format("{} tuples have been copied to the {} table.",
        sharedState->getNumRows() - sharedState->getNumErroredRows(), info->tableName);
    auto clientContext = context->clientContext;
    FactorizedTableUtils::appendStringToTable(sharedState->fTable.get(), outputMsg,
        MemoryManager::Get(*clientContext));

    const auto warningCount =
        WarningContext::Get(*clientContext)->getWarningCount(context->queryID);
    if (warningCount > 0) {
        auto warningMsg =
            std::format("{} warnings encountered during copy. Use 'CALL "
                        "show_warnings() RETURN *' to view the actual warnings. Query ID: {}",
                warningCount, context->queryID);
        FactorizedTableUtils::appendStringToTable(sharedState->fTable.get(), warningMsg,
            MemoryManager::Get(*clientContext));
    }
}

} // namespace processor
} // namespace lbug
