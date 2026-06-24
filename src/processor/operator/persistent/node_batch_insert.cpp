#include "processor/operator/persistent/node_batch_insert.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/assert.h"
#include "common/cast.h"
#include "common/data_chunk/data_chunk_state.h"
#include "common/exception/message.h"
#include "common/exception/runtime.h"
#include "common/file_system/file_info.h"
#include "common/file_system/virtual_file_system.h"
#include "common/finally_wrapper.h"
#include "common/type_utils.h"
#include "common/vector/value_vector.h"
#include "main/client_context.h"
#include "main/db_config.h"
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

// Total read-buffer memory shared across all spilled runs during the final streaming merge,
// so that merge memory stays bounded regardless of how many runs were produced.
constexpr uint64_t PK_VALIDATOR_MERGE_BUFFER_BUDGET = 64 * 1024 * 1024;
// Minimum per-run read buffer. Each run reader gets max(this, budget/numRuns) bytes.
constexpr uint64_t PK_VALIDATOR_RUN_READ_BUFFER_MIN = 4 * 1024;
// Flush threshold for the string-run write buffer (bytes).
constexpr uint64_t PK_VALIDATOR_WRITE_FLUSH_THRESHOLD = 1u << 20;

// Produces a unique spill-file path next to the database file so that concurrent no-index COPYs
// into different tables do not collide.
std::string makePKValidatorSpillFilePath(const std::string& dbPath) {
    static std::atomic<uint64_t> counter{0};
    return std::format("{}.pk_validator.{}.tmp", dbPath, counter.fetch_add(1));
}

template<typename T>
class PKRunReader {
    static constexpr bool kIsStringPK = std::same_as<T, string_t>;

public:
    PKRunReader(FileInfo* file, uint64_t startOffset, uint64_t numValues, uint64_t runBytes,
        uint64_t bufferSize)
        : file{file}, filePos{startOffset}, valuesLeft{numValues}, runBytesLeft{runBytes},
          buffer(std::max<uint64_t>(bufferSize, PK_VALIDATOR_RUN_READ_BUFFER_MIN)) {}

    bool hasNext() const { return valuesLeft > 0; }

    StoredPKValue<T> next() {
        DASSERT(valuesLeft > 0);
        --valuesLeft;
        if constexpr (kIsStringPK) {
            uint32_t len = 0;
            readBytes(reinterpret_cast<uint8_t*>(&len), sizeof(uint32_t));
            std::string s;
            s.resize(len);
            if (len > 0) {
                readBytes(reinterpret_cast<uint8_t*>(s.data()), len);
            }
            return s;
        } else {
            StoredPKValue<T> value;
            readBytes(reinterpret_cast<uint8_t*>(&value), sizeof(StoredPKValue<T>));
            return value;
        }
    }

private:
    void readBytes(uint8_t* dst, size_t n) {
        size_t copied = 0;
        while (copied < n) {
            if (bufferPos == bufferFilled) {
                refill();
            }
            const size_t avail = bufferFilled - bufferPos;
            const size_t toCopy = std::min(avail, n - copied);
            std::memcpy(dst + copied, buffer.data() + bufferPos, toCopy);
            bufferPos += toCopy;
            copied += toCopy;
        }
    }

    void refill() {
        DASSERT(runBytesLeft > 0);
        const auto toRead = std::min<uint64_t>(buffer.size(), runBytesLeft);
        file->readFromFile(buffer.data(), toRead, filePos);
        filePos += toRead;
        runBytesLeft -= toRead;
        bufferPos = 0;
        bufferFilled = static_cast<size_t>(toRead);
    }

    FileInfo* file;
    uint64_t filePos;
    uint64_t valuesLeft;
    uint64_t runBytesLeft;
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;
    size_t bufferFilled = 0;
};

template<typename T>
struct NoIndexPKValidatorImpl final : NoIndexPKValidator {
    static constexpr bool kIsStringPK = std::same_as<T, string_t>;

    NoIndexPKValidatorImpl(uint64_t spillThresholdBytes, std::string spillFilePath,
        VirtualFileSystem* vfs)
        : spillThresholdBytes{spillThresholdBytes}, spillFilePath{std::move(spillFilePath)},
          vfs{vfs} {
        if (!this->spillFilePath.empty()) {
            DASSERT(vfs != nullptr);
            spillFile = vfs->openFile(this->spillFilePath,
                FileOpenFlags{FileFlags::WRITE | FileFlags::READ_ONLY |
                              FileFlags::CREATE_AND_TRUNCATE_IF_EXISTS});
        }
    }

    ~NoIndexPKValidatorImpl() override { cleanup(); }
    NoIndexPKValidatorImpl(const NoIndexPKValidatorImpl&) = delete;
    NoIndexPKValidatorImpl& operator=(const NoIndexPKValidatorImpl&) = delete;

    void validate(const ColumnChunkData& pkChunk, offset_t startOffset,
        length_t numValues) override {
        std::lock_guard lck{mtx};
        for (auto i = 0u; i < numValues; ++i) {
            const auto pos = startOffset + i;
            if (pkChunk.isNull(pos)) {
                throw RuntimeException(ExceptionMessage::nullPKException());
            }
            const auto value = readPKValue<T>(pkChunk, pos);
            bufferBytes += approxValueBytes(value);
            buffer.push_back(std::move(value));
        }
        if (canSpill() && bufferBytes >= spillThresholdBytes) {
            spillSortedRun();
        }
    }

    void finalize() override {
        std::lock_guard lck{mtx};
        if (runs.empty()) {
            // Everything stayed in memory (small COPY, spilling disabled, or in-memory DB):
            // a single sort + adjacent dedupe is enough and needs no disk.
            sortAndCheckDuplicates(buffer);
            return;
        }
        // Sort the residual buffer so it can participate as one more sorted source alongside the
        // spilled runs, then stream-merge everything to detect cross-run duplicates.
        std::sort(buffer.begin(), buffer.end());
        mergeAndCheckDuplicates();
    }

private:
    static uint64_t approxValueBytes(const StoredPKValue<T>& v) {
        if constexpr (kIsStringPK) {
            return sizeof(uint32_t) + v.size();
        } else {
            return sizeof(StoredPKValue<T>);
        }
    }

    bool canSpill() const { return spillFile != nullptr; }

    struct Run {
        uint64_t startOffset;
        uint64_t numValues;
        uint64_t numBytes;
    };

    void spillSortedRun() {
        sortAndCheckDuplicates(buffer);
        Run run{fileEndOffset, buffer.size(), 0};
        writeRun(run);
        runs.push_back(run);
        buffer.clear();
        bufferBytes = 0;
    }

    void writeRun(Run& run) {
        if constexpr (kIsStringPK) {
            std::vector<uint8_t> out;
            out.reserve(std::min<uint64_t>(bufferBytes, PK_VALIDATOR_WRITE_FLUSH_THRESHOLD));
            for (const auto& v : buffer) {
                const auto len = static_cast<uint32_t>(v.size());
                const auto* lenBytes = reinterpret_cast<const uint8_t*>(&len);
                out.insert(out.end(), lenBytes, lenBytes + sizeof(uint32_t));
                out.insert(out.end(), v.data(), v.data() + v.size());
                if (out.size() >= PK_VALIDATOR_WRITE_FLUSH_THRESHOLD) {
                    flushBytes(out);
                }
            }
            if (!out.empty()) {
                flushBytes(out);
            }
        } else {
            writeNumericRun();
        }
        run.numBytes = fileEndOffset - run.startOffset;
    }

    void flushBytes(std::vector<uint8_t>& out) {
        spillFile->writeFile(out.data(), out.size(), fileEndOffset);
        fileEndOffset += out.size();
        out.clear();
    }

    // Writes the numeric (non-string) run. std::vector<bool> has no contiguous storage, so it is
    // serialized element-by-element; all other numeric types are written as a contiguous block.
    void writeNumericRun() {
        if constexpr (std::same_as<StoredPKValue<T>, bool>) {
            std::vector<uint8_t> out;
            out.reserve(buffer.size());
            for (const auto v : buffer) {
                out.push_back(static_cast<uint8_t>(v ? 1 : 0));
            }
            if (!out.empty()) {
                spillFile->writeFile(out.data(), out.size(), fileEndOffset);
                fileEndOffset += out.size();
            }
        } else {
            const auto totalBytes = buffer.size() * sizeof(StoredPKValue<T>);
            spillFile->writeFile(reinterpret_cast<const uint8_t*>(buffer.data()), totalBytes,
                fileEndOffset);
            fileEndOffset += totalBytes;
        }
    }

    static void sortAndCheckDuplicates(std::vector<StoredPKValue<T>>& values) {
        std::sort(values.begin(), values.end());
        for (size_t i = 1; i < values.size(); ++i) {
            if (values[i] == values[i - 1]) {
                throw RuntimeException(
                    ExceptionMessage::duplicatePKException(pkValueToString<T>(values[i])));
            }
        }
    }

    // Streaming k-way merge over all spilled runs plus the (already sorted) residual buffer.
    // Memory is bounded by the per-run read buffers plus the heap of source indices; no merged
    // output is written because we only need to detect adjacent equal values.
    void mergeAndCheckDuplicates() {
        struct Source {
            std::unique_ptr<PKRunReader<T>> reader; // null when this source is the residual vector
            const std::vector<StoredPKValue<T>>* vec = nullptr;
            uint64_t vecIdx = 0;
            StoredPKValue<T> cur{};
            bool hasCur = false;
            bool advance() {
                if (reader) {
                    if (!reader->hasNext()) {
                        hasCur = false;
                        return false;
                    }
                    cur = reader->next();
                    hasCur = true;
                    return true;
                }
                DASSERT(vec != nullptr);
                if (vecIdx >= vec->size()) {
                    hasCur = false;
                    return false;
                }
                cur = (*vec)[vecIdx++];
                hasCur = true;
                return true;
            }
        };

        std::vector<Source> sources;
        sources.reserve(runs.size() + (buffer.empty() ? 0 : 1));
        const auto perRunBuffer = computeRunReadBufferSize(runs.size());
        for (const auto& run : runs) {
            Source s;
            s.reader = std::make_unique<PKRunReader<T>>(spillFile.get(), run.startOffset,
                run.numValues, run.numBytes, perRunBuffer);
            s.advance();
            sources.push_back(std::move(s));
        }
        if (!buffer.empty()) {
            Source s;
            s.vec = &buffer;
            s.advance();
            sources.push_back(std::move(s));
        }

        auto cmp = [&sources](size_t a, size_t b) { return sources[a].cur > sources[b].cur; };
        std::vector<size_t> heap;
        heap.reserve(sources.size());
        for (size_t i = 0; i < sources.size(); ++i) {
            if (sources[i].hasCur) {
                heap.push_back(i);
            }
        }
        std::make_heap(heap.begin(), heap.end(), cmp);

        std::optional<StoredPKValue<T>> last;
        while (!heap.empty()) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            const auto idx = heap.back();
            auto& src = sources[idx];
            DASSERT(src.hasCur);
            if (last.has_value() && src.cur == *last) {
                throw RuntimeException(
                    ExceptionMessage::duplicatePKException(pkValueToString<T>(src.cur)));
            }
            last = src.cur;
            if (src.advance()) {
                std::push_heap(heap.begin(), heap.end(), cmp);
            } else {
                heap.pop_back();
            }
        }
    }

    uint64_t computeRunReadBufferSize(size_t numRuns) const {
        if (numRuns == 0) {
            return PK_VALIDATOR_RUN_READ_BUFFER_MIN;
        }
        const auto perRun = std::max<uint64_t>(PK_VALIDATOR_RUN_READ_BUFFER_MIN,
            PK_VALIDATOR_MERGE_BUFFER_BUDGET / numRuns);
        return std::min<uint64_t>(perRun, PK_VALIDATOR_MERGE_BUFFER_BUDGET);
    }

    void cleanup() {
        // Drop in-memory state and best-effort remove the spill file. Idempotent so it is safe to
        // call from the destructor even after a throwing finalize.
        buffer.clear();
        bufferBytes = 0;
        runs.clear();
        spillFile.reset();
        if (vfs != nullptr && !spillFilePath.empty()) {
            try {
                vfs->removeFileIfExists(spillFilePath);
            } catch (...) {
                // Best-effort cleanup; ignore failures (e.g. file already removed).
            }
        }
    }

    std::mutex mtx;
    std::vector<StoredPKValue<T>> buffer; // unsorted accumulator; sorted before spill/merge
    uint64_t bufferBytes = 0;             // approximate serialized size of `buffer`
    std::vector<Run> runs;                // metadata for each spilled sorted run
    uint64_t fileEndOffset = 0;           // append cursor within the spill file

    const uint64_t spillThresholdBytes; // 0 => never spill (unbounded in-memory, legacy behaviour)
    std::string spillFilePath;          // empty when spilling is disabled (in-memory DB, etc.)
    VirtualFileSystem* vfs;
    std::unique_ptr<FileInfo> spillFile;
};

std::unique_ptr<NoIndexPKValidator> createNoIndexPKValidator(const LogicalType& pkType,
    main::ClientContext* clientContext) {
    const auto threshold = clientContext->getClientConfig()->pkValidatorSpillThreshold;
    std::string spillFilePath;
    VirtualFileSystem* vfs = nullptr;
    if (threshold > 0 && !clientContext->isInMemory() &&
        clientContext->getDBConfig()->enableSpillingToDisk) {
        vfs = VirtualFileSystem::GetUnsafe(*clientContext);
        spillFilePath = makePKValidatorSpillFilePath(clientContext->getDatabasePath());
    }
    return TypeUtils::visit(pkType, [=]<typename T>(T) -> std::unique_ptr<NoIndexPKValidator> {
        if constexpr (std::same_as<T, bool> || std::same_as<T, int8_t> ||
                      std::same_as<T, int16_t> || std::same_as<T, int32_t> ||
                      std::same_as<T, int64_t> || std::same_as<T, uint8_t> ||
                      std::same_as<T, uint16_t> || std::same_as<T, uint32_t> ||
                      std::same_as<T, uint64_t> || std::same_as<T, int128_t> ||
                      std::same_as<T, uint128_t> || std::same_as<T, float> ||
                      std::same_as<T, double> || std::same_as<T, string_t>) {
            return std::make_unique<NoIndexPKValidatorImpl<T>>(threshold, spillFilePath, vfs);
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
            if (skipDuplicatePK) {
                throw RuntimeException(
                    "IGNORE_ERRORS=true (DUPLICATE_PK_ONLY) is only supported for node tables "
                    "with a primary-key hash index.");
            }
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
        if (skipDuplicatePK) {
            throw RuntimeException(
                "IGNORE_ERRORS=true (DUPLICATE_PK_ONLY) is only supported for node tables with "
                "a primary-key hash index.");
        }
        globalIndexBuilder.reset();
        noIndexPKValidator = createNoIndexPKValidator(pkType, context->clientContext);
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
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
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
    if (nodeInfo->skipDuplicatePK) {
        std::lock_guard lck{nodeSharedState->duplicatePKSkipResult->mtx};
        nodeSharedState->duplicatePKSkipResult->skippedCount +=
            nodeLocalState->duplicatePKSkipResult.skippedCount;
        nodeSharedState->duplicatePKSkipResult->pks.insert(
            nodeSharedState->duplicatePKSkipResult->pks.end(),
            std::make_move_iterator(nodeLocalState->duplicatePKSkipResult.pks.begin()),
            std::make_move_iterator(nodeLocalState->duplicatePKSkipResult.pks.end()));
        nodeLocalState->duplicatePKSkipResult.pks.clear();
    }
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
    const auto* nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
    auto* duplicatePKSkipResult = nodeSharedState->duplicatePKSkipResult.get();
    if (localState != nullptr) {
        const auto nodeLocalState =
            dynamic_cast_checked<NodeBatchInsertLocalState*>(localState.get());
        duplicatePKSkipResult = &nodeLocalState->duplicatePKSkipResult;
    }
    return NodeBatchInsertErrorHandler{context, nodeSharedState->pkType.getLogicalTypeID(),
        nodeTable, WarningContext::Get(*context->clientContext)->getIgnoreErrorsOption(),
        sharedState->numErroredRows, &sharedState->erroredRowMutex, nodeInfo->skipDuplicatePK,
        duplicatePKSkipResult};
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
    if (nodeSharedState->noIndexPKValidator) {
        // Completes cross-chunk duplicate detection for the no-hash-index COPY path. Sorted runs
        // spilled to disk during validate() are stream-merged here, so duplicates that span
        // chunks are reported before the transaction commits.
        nodeSharedState->noIndexPKValidator->finalize();
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
    auto clientContext = context->clientContext;
    const auto* nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
    const auto* nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());

    int64_t skippedDuplicatePKCount = 0;
    std::vector<std::string> skippedDuplicatePKs;
    if (nodeInfo->skipDuplicatePK) {
        std::lock_guard lck{nodeSharedState->duplicatePKSkipResult->mtx};
        // Duplicate-PK rows are counted in getNumRows() because they are appended before index
        // validation, but they do not contribute to getNumErroredRows() because they bypass the
        // generic warning/error path. We subtract skippedCount here to convert the appended-row
        // count into the final copied-row count. This relies on duplicate-row deletion not
        // decrementing numRows and on the duplicate skip path not incrementing numErroredRows.
        DASSERT(
            sharedState->getNumRows() >= sharedState->getNumErroredRows() +
                                             nodeSharedState->duplicatePKSkipResult->skippedCount);
        skippedDuplicatePKCount = nodeSharedState->duplicatePKSkipResult->skippedCount;
        skippedDuplicatePKs = nodeSharedState->duplicatePKSkipResult->pks;
    }
    auto copiedCount =
        sharedState->getNumRows() - sharedState->getNumErroredRows() - skippedDuplicatePKCount;
    const auto warningCount =
        WarningContext::Get(*clientContext)->getWarningCount(context->queryID);
    std::string outputMsg =
        std::format("{} tuples have been copied to the {} table.", copiedCount, info->tableName);
    if (warningCount > 0) {
        // Fold the warning summary into the single result row so the user still sees how many
        // warnings were collected during the COPY. Individual warnings remain queryable via
        // `CALL show_warnings() RETURN *`.
        outputMsg = std::format(
            "{} tuples have been copied to the {} table. {} warnings encountered during copy. "
            "Use 'CALL show_warnings() RETURN *' to view the actual warnings. Query ID: {}",
            copiedCount, info->tableName, warningCount, context->queryID);
    }
    // Contract: a node COPY always returns exactly one row with three columns
    // (result, skipped_duplicate_pk_count, skipped_duplicate_pks).
    FactorizedTableUtils::appendNodeCopyResultToTable(sharedState->fTable.get(), outputMsg,
        skippedDuplicatePKCount, skippedDuplicatePKs, MemoryManager::Get(*clientContext));
}

} // namespace processor
} // namespace lbug
