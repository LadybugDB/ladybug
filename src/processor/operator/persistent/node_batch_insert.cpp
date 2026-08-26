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
#include "processor/partition_routing.h"
#include "processor/result/factorized_table_util.h"
#include "processor/warning_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/local_storage/local_storage.h"
#include "storage/partition_storage_registry.h"
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

void NodeBatchInsertSharedState::initTargetPKIndex(const ExecutionContext* context,
    NodeBatchInsertTarget& target) {
    auto* nodeTable = target.table;
    auto* pkIndex = nodeTable->tryGetPKIndex();
    if (!pkIndex) {
        if (nodeTable->tryGetPrimaryKeyIndex() != nullptr) {
            if (skipDuplicatePK) {
                throw RuntimeException(
                    "IGNORE_ERRORS=true (DUPLICATE_PK_ONLY) is only supported for node tables "
                    "with a primary-key hash index.");
            }
            target.globalIndexBuilder.reset();
            target.noIndexPKValidator.reset();
            target.usePrimaryKeyIndexCommitInsert = true;
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
        target.globalIndexBuilder.reset();
        target.noIndexPKValidator = createNoIndexPKValidator(pkType, context->clientContext);
        target.usePrimaryKeyIndexCommitInsert = false;
        if (!target.noIndexPKValidator) {
            throw RuntimeException(ExceptionMessage::invalidPKType(pkType.toString()));
        }
        return;
    }
    target.noIndexPKValidator.reset();
    target.usePrimaryKeyIndexCommitInsert = false;
    target.globalIndexBuilder = IndexBuilder(std::make_shared<IndexBuilderSharedState>(
        Transaction::Get(*context->clientContext), nodeTable));
}

void NodeBatchInsert::initGlobalStateInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    auto catalog = Catalog::Get(*clientContext);
    auto transaction = Transaction::Get(*clientContext);
    auto nodeTableEntry = catalog->getTableCatalogEntry(transaction, info->tableName)
                              ->ptrCast<NodeTableCatalogEntry>();
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
    nodeSharedState->pkColumnID = pkColumnID;
    nodeSharedState->pkType = pkDefinition.getType().copy();
    nodeSharedState->skipDuplicatePK = info->ptrCast<NodeBatchInsertInfo>()->skipDuplicatePK;

    auto* storageManager = StorageManager::Get(*clientContext);
    const auto nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
    if (nodeInfo->partitionInfo.has_value()) {
        const auto& partitionInfo = *nodeInfo->partitionInfo;
        nodeSharedState->targets.reserve(partitionInfo.numPartitions);
        const auto* hooks = common::getPartitionRoutingHooks();
        for (auto i = 0u; i < partitionInfo.partitionTableIDs.size(); ++i) {
            const auto tableID = partitionInfo.partitionTableIDs[i];
            NodeBatchInsertTarget target;
            const auto ref = common::PartitionRef{partitionInfo.parentTableID, i};
            common::PartitionHandle handle = nullptr;
            const bool claimed = hooks != nullptr && hooks->locate != nullptr &&
                                 hooks->locate(hooks->context, ref, &handle);
            // Remotely routed partitions own no local table; rows are shipped through the
            // routing hooks in copyToNodeGroup.
            target.table =
                claimed ?
                    nullptr :
                    storage::PartitionStorageRegistry::resolveNodeTableByID(clientContext, tableID);
            // Allocate pages from the file that owns the target table (a partition child
            // flushes into its own data file, not the main database file).
            if (target.table != nullptr) {
                target.optimisticAllocator = transaction->getLocalStorage()->addOptimisticAllocator(
                    target.table->getStorageManager());
            }
            nodeSharedState->targets.push_back(std::move(target));
            nodeSharedState->partitionRefs.push_back(ref);
            nodeSharedState->partitionHandles.push_back(claimed ? handle : nullptr);
        }
        // The partition-key column id is the catalog property id. Property columns are evaluated
        // in schema order, so its index among the column evaluators equals its position in
        // insertColumnIDs (warning columns are appended after property columns).
        const auto keyColumnID = partitionInfo.partitionKeyColumnID;
        const auto it =
            std::find(info->insertColumnIDs.begin(), info->insertColumnIDs.end(), keyColumnID);
        DASSERT(it != info->insertColumnIDs.end());
        nodeSharedState->partitionKeyColumnIdx = std::distance(info->insertColumnIDs.begin(), it);
        if (partitionInfo.method == common::PartitionMethod::LIST) {
            // The parent entry is reachable through any partition child. Its partition set grows
            // as workers encounter new key values; the router serializes discovery.
            const auto* firstChild =
                catalog->getTableCatalogEntry(transaction, partitionInfo.partitionTableIDs[0])
                    ->ptrCast<NodeTableCatalogEntry>();
            auto* parent =
                catalog->getTableCatalogEntry(transaction, firstChild->getParentTableID())
                    ->ptrCast<NodeTableCatalogEntry>();
            nodeSharedState->listRouter =
                std::make_unique<ListPartitionRouter>(clientContext, parent);
        }
    } else {
        NodeBatchInsertTarget target;
        target.table = storageManager->getTable(nodeTableEntry->getTableID())->ptrCast<NodeTable>();
        target.optimisticAllocator = transaction->getLocalStorage()->addOptimisticAllocator(
            target.table->getStorageManager());
        nodeSharedState->targets.push_back(std::move(target));
    }

    for (auto& target : nodeSharedState->targets) {
        if (target.table == nullptr) {
            continue; // remotely routed: PK handling is the wrapper's job
        }
        nodeSharedState->initTargetPKIndex(context, target);
    }
}

void NodeBatchInsert::initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) {
    const auto nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
    const auto numColumns = nodeInfo->columnEvaluators.size();

    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    localState = std::make_unique<NodeBatchInsertLocalState>();
    const auto nodeLocalState = localState->ptrCast<NodeBatchInsertLocalState>();
    nodeLocalState->stats.emplace(
        std::span{nodeInfo->columnTypes.begin(), nodeInfo->outputDataColumns.size()});
    nodeLocalState->targets.reserve(nodeSharedState->targets.size());
    for (auto i = 0u; i < nodeSharedState->targets.size(); ++i) {
        auto& sharedTarget = nodeSharedState->targets[i];
        NodeBatchInsertLocalTarget localTarget;
        if (sharedTarget.globalIndexBuilder) {
            localTarget.localIndexBuilder = sharedTarget.globalIndexBuilder->clone();
        }
        // Remotely routed targets never receive local appends, so they carry no error handler.
        if (sharedTarget.table == nullptr) {
            localTarget.errorHandler = std::nullopt;
        } else {
            localTarget.errorHandler = createErrorHandler(context, sharedTarget.table,
                &nodeLocalState->duplicatePKSkipResult);
        }
        nodeLocalState->targets.push_back(std::move(localTarget));
    }

    nodeLocalState->columnVectors.resize(numColumns);

    for (auto i = 0u; i < numColumns; ++i) {
        auto& evaluator = nodeInfo->columnEvaluators[i];
        evaluator->init(*resultSet, context->clientContext);
        nodeLocalState->columnVectors[i] = evaluator->resultVector.get();
    }
    DASSERT(resultSet->dataChunks[0]);
    nodeLocalState->columnState = resultSet->dataChunks[0]->state;
}

void NodeBatchInsert::executeInternal(ExecutionContext* context) {
    const auto clientContext = context->clientContext;
    auto nodeLocalState = localState->ptrCast<NodeBatchInsertLocalState>();
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    // LIST-partitioned COPYs grow `targets` (and these tokens) mid-copy; pre-existing
    // partitions register their producers here.
    nodeLocalState->indexProducerTokens.resize(nodeLocalState->targets.size());
    for (auto i = 0u; i < nodeLocalState->targets.size(); ++i) {
        if (nodeLocalState->targets[i].localIndexBuilder) {
            nodeLocalState->indexProducerTokens[i] =
                nodeLocalState->targets[i].localIndexBuilder->getProducerToken();
        }
    }
    auto transaction = Transaction::Get(*clientContext);
    while (children[0]->getNextTuple(context)) {
        const auto originalSelVector = nodeLocalState->columnState->getSelVectorShared();
        // Evaluate expressions if needed.
        const auto numTuples = nodeLocalState->columnState->getSelVector().getSelSize();
        evaluateExpressions(numTuples);
        copyToNodeGroup(transaction, MemoryManager::Get(*clientContext), context);
        nodeLocalState->columnState->setSelVector(originalSelVector);
    }
    for (auto i = 0u; i < nodeLocalState->targets.size(); ++i) {
        auto& localTarget = nodeLocalState->targets[i];
        if (localTarget.chunkedGroup && localTarget.chunkedGroup->getNumRows() > 0) {
            appendIncompleteNodeGroup(transaction, i, std::move(localTarget.chunkedGroup),
                localTarget.localIndexBuilder, MemoryManager::Get(*context->clientContext));
        }
        if (localTarget.localIndexBuilder) {
            DASSERT(i < nodeLocalState->indexProducerTokens.size() &&
                    nodeLocalState->indexProducerTokens[i].has_value());
            nodeLocalState->indexProducerTokens[i]->quit();

            DASSERT(localTarget.errorHandler.has_value());
            localTarget.localIndexBuilder->finishedProducing(localTarget.errorHandler.value());
            localTarget.errorHandler->flushStoredErrors();
        }
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
    if (nodeSharedState->targets.size() == 1 && nodeLocalState->stats.has_value()) {
        nodeSharedState->targets[0].table->mergeStats(nodeInfo->insertColumnIDs,
            *nodeLocalState->stats);
    }
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
    storage::MemoryManager* mm, ExecutionContext* context) const {
    const auto nodeLocalState = dynamic_cast_checked<NodeBatchInsertLocalState*>(localState.get());
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    const auto nodeInfo = info->ptrCast<NodeBatchInsertInfo>();

    const auto numTuples = nodeLocalState->columnState->getSelVector().getSelSize();
    if (nodeSharedState->targets.size() <= 1 && nodeSharedState->listRouter == nullptr) {
        // Non-partitioned fast path: append the contiguous selection vector directly.
        auto numAppendedTuples = 0ul;
        auto& target = nodeLocalState->targets[0];
        while (numAppendedTuples < numTuples) {
            if (!target.chunkedGroup) {
                // Allocate on the first append rather than in initLocalStateInternal so that a
                // worker that receives no rows allocates nothing. Eager allocation reserves
                // NODE_GROUP_SIZE rows of every column for every worker before the scan produces
                // a row, which sets the COPY memory floor to columns x workers regardless of the
                // input size.
                target.chunkedGroup =
                    std::make_unique<InMemChunkedNodeGroup>(*mm, nodeInfo->columnTypes,
                        info->compressionEnabled, StorageConfig::NODE_GROUP_SIZE, 0);
            }
            const auto numAppendedInGroup = target.chunkedGroup->append(
                nodeLocalState->columnVectors, numAppendedTuples, numTuples - numAppendedTuples);
            numAppendedTuples += numAppendedInGroup;
            if (target.chunkedGroup->isFull()) {
                writeAndResetNodeGroup(transaction, 0, target.chunkedGroup,
                    target.localIndexBuilder, mm,
                    *sharedState->ptrCast<NodeBatchInsertSharedState>()
                         ->targets[0]
                         .optimisticAllocator);
            }
        }
        if (nodeLocalState->stats.has_value()) {
            nodeLocalState->stats->update(nodeLocalState->columnVectors,
                nodeInfo->outputDataColumns.size());
        }
        sharedState->incrementNumRows(numAppendedTuples);
        return;
    }

    // Partitioned path: compute a partition index per (logical) row, then append runs of
    // consecutive rows that belong to the same partition. Rows are addressed by their logical
    // index (i.e. an offset into each vector's own selection vector) so columns backed by
    // reference/cast/default evaluators all follow the same row order. Each partition subgraph is
    // an ordinary NodeTable, so its own WAL/MVCC machinery (appendToLastNodeGroup + commit/undo
    // records) applies the routed write.
    const auto& keyVector = *nodeLocalState->columnVectors[nodeSharedState->partitionKeyColumnIdx];
    if (nodeSharedState->listRouter != nullptr) {
        // LIST: resolve every row's key to its (possibly newly created) partition under the
        // router lock -- growing the shared and per-worker target arrays atomically with
        // discovery -- then fall through to the generic run-append loop.
        nodeLocalState->partitionIdxes.resize(numTuples);
        const auto& selVector = keyVector.state->getSelVector();
        nodeSharedState->listRouter->withLock([&]() {
            std::unordered_map<uint64_t, storage::NodeTable*> freshTables;
            for (auto i = 0u; i < numTuples; ++i) {
                const auto pos = selVector[i];
                if (keyVector.isNull(pos)) {
                    throw RuntimeException("Cannot COPY a NULL partition-key value into a "
                                           "LIST-partitioned table.");
                }
                const auto route = nodeSharedState->listRouter->getOrCreatePartitionLocked(
                    encodeListPartitionKey(keyVector, pos));
                nodeLocalState->partitionIdxes[i] = route.ordinal;
                freshTables.emplace(route.ordinal, route.table);
            }
            growListTargets(context, nodeSharedState, nodeLocalState, freshTables);
        });
    } else {
        computePartitionIndexes(keyVector, nodeSharedState->targets.size(),
            nodeLocalState->partitionIdxes);
    }

    for (auto i = 0u; i < numTuples;) {
        const auto partitionIdx = nodeLocalState->partitionIdxes[i];
        auto runEnd = i + 1;
        while (runEnd < numTuples && nodeLocalState->partitionIdxes[runEnd] == partitionIdx) {
            ++runEnd;
        }

        auto& target = nodeLocalState->targets[partitionIdx];
        if (nodeSharedState->targets[partitionIdx].table == nullptr) {
            // Remotely routed partition: ship the run through the routing hooks. Rows are
            // addressed by their logical index in the evaluated row space (the same convention
            // InMemChunkedNodeGroup::append uses).
            const auto* hooks = common::getPartitionRoutingHooks();
            if (hooks == nullptr || hooks->insertChunk == nullptr) {
                throw RuntimeException("Partition is routed remotely but no routing hooks with "
                                       "insertChunk are installed.");
            }
            hooks->insertChunk(hooks->context, nodeSharedState->partitionRefs[partitionIdx],
                nodeSharedState->partitionHandles[partitionIdx], transaction, &keyVector,
                nodeLocalState->columnVectors, i, runEnd - i);
            i = runEnd;
            continue;
        }
        auto numAppendedTuples = 0ul;
        while (numAppendedTuples < runEnd - i) {
            if (!target.chunkedGroup) {
                target.chunkedGroup =
                    std::make_unique<InMemChunkedNodeGroup>(*mm, nodeInfo->columnTypes,
                        info->compressionEnabled, StorageConfig::NODE_GROUP_SIZE, 0);
            }
            const auto numAppendedInGroup =
                target.chunkedGroup->append(nodeLocalState->columnVectors, i + numAppendedTuples,
                    runEnd - i - numAppendedTuples);
            numAppendedTuples += numAppendedInGroup;
            if (target.chunkedGroup->isFull()) {
                writeAndResetNodeGroup(transaction, partitionIdx, target.chunkedGroup,
                    target.localIndexBuilder, mm,
                    *nodeSharedState->targets[partitionIdx].optimisticAllocator);
            }
        }
        i = runEnd;
    }
    // Per-partition table stats are advisory (used by the optimizer); skip them for partitioned
    // writes rather than over-counting each partition.
    sharedState->incrementNumRows(numTuples);
}

void NodeBatchInsert::growListTargets(ExecutionContext* context,
    NodeBatchInsertSharedState* nodeSharedState, NodeBatchInsertLocalState* nodeLocalState,
    const std::unordered_map<uint64_t, storage::NodeTable*>& freshTables) const {
    // Caller holds the router lock: discovery and array growth are atomic across workers.
    auto maxOrdinal = uint64_t{0};
    for (const auto& [ordinal, table] : freshTables) {
        maxOrdinal = std::max(maxOrdinal, ordinal);
    }
    const auto prevSize = nodeLocalState->targets.size();
    if (maxOrdinal + 1 <= prevSize) {
        return;
    }
    for (auto k = nodeSharedState->targets.size(); k <= maxOrdinal; ++k) {
        NodeBatchInsertTarget target;
        target.table = freshTables.at(k);
        // LIST partitions discover targets mid-copy; scope their allocations to the file
        // owning the newly created partition child, same as pre-declared HASH targets.
        target.optimisticAllocator =
            Transaction::Get(*context->clientContext)
                ->getLocalStorage()
                ->addOptimisticAllocator(target.table->getStorageManager());
        nodeSharedState->initTargetPKIndex(context, target);
        nodeSharedState->targets.push_back(std::move(target));
    }
    for (auto k = prevSize; k <= maxOrdinal; ++k) {
        auto& sharedTarget = nodeSharedState->targets[k];
        NodeBatchInsertLocalTarget localTarget;
        if (localTarget.localIndexBuilder) {
            localTarget.localIndexBuilder = sharedTarget.globalIndexBuilder->clone();
            nodeLocalState->indexProducerTokens.push_back(
                localTarget.localIndexBuilder->getProducerToken());
        } else {
            nodeLocalState->indexProducerTokens.push_back(std::nullopt);
        }
        localTarget.errorHandler =
            createErrorHandler(context, sharedTarget.table, &nodeLocalState->duplicatePKSkipResult);
        nodeLocalState->targets.push_back(std::move(localTarget));
    }
}

NodeBatchInsertErrorHandler NodeBatchInsert::createErrorHandler(ExecutionContext* context,
    storage::NodeTable* nodeTable, DuplicatePKSkipResult* duplicatePKSkipResult) const {
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    const auto* nodeInfo = info->ptrCast<NodeBatchInsertInfo>();
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
    common::idx_t targetIdx, std::unique_ptr<InMemChunkedNodeGroup>& nodeGroup,
    std::optional<IndexBuilder>& indexBuilder, MemoryManager* mm,
    PageAllocator& pageAllocator) const {
    const auto nodeLocalState = localState->ptrCast<NodeBatchInsertLocalState>();
    DASSERT(nodeLocalState->targets[targetIdx].errorHandler.has_value());
    writeAndResetNodeGroup(transaction, targetIdx, nodeGroup, indexBuilder, mm,
        nodeLocalState->targets[targetIdx].errorHandler.value(), pageAllocator);
}

void NodeBatchInsert::writeAndResetNodeGroup(transaction::Transaction* transaction,
    common::idx_t targetIdx, std::unique_ptr<InMemChunkedNodeGroup>& nodeGroup,
    std::optional<IndexBuilder>& indexBuilder, MemoryManager* mm,
    NodeBatchInsertErrorHandler& errorHandler, PageAllocator& pageAllocator) const {
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    const auto& sharedTarget = nodeSharedState->targets[targetIdx];
    auto* nodeTable = sharedTarget.table;

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
    } else if (sharedTarget.usePrimaryKeyIndexCommitInsert) {
        auto* index = nodeTable->tryGetPrimaryKeyIndex();
        DASSERT(index != nullptr);
        commitPrimaryKeyIndexInsertions(transaction, *nodeTable, *index,
            nodeGroup->getColumnChunk(nodeSharedState->pkColumnID), nodeOffset, numRowsWritten,
            transaction->getClientContext());
    } else if (sharedTarget.noIndexPKValidator) {
        sharedTarget.noIndexPKValidator->validate(
            nodeGroup->getColumnChunk(nodeSharedState->pkColumnID), 0, numRowsWritten);
    }
    if (numRowsWritten == nodeGroup->getNumRows()) {
        nodeGroup->resetToEmpty();
    } else {
        clearToIndex(mm, nodeGroup, numRowsWritten);
    }
}

void NodeBatchInsert::appendIncompleteNodeGroup(transaction::Transaction* transaction,
    common::idx_t targetIdx, std::unique_ptr<InMemChunkedNodeGroup> localNodeGroup,
    std::optional<IndexBuilder>& indexBuilder, MemoryManager* mm) const {
    std::unique_lock xLck{sharedState->mtx};
    auto* nodeSharedState = dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    auto& sharedTarget = nodeSharedState->targets[targetIdx];
    if (!sharedTarget.sharedNodeGroup) {
        sharedTarget.sharedNodeGroup = std::move(localNodeGroup);
        return;
    }
    uint64_t numNodesAppended = 0;
    while (numNodesAppended < localNodeGroup->getNumRows()) {
        if (sharedTarget.sharedNodeGroup->isFull()) {
            writeAndResetNodeGroup(transaction, targetIdx, sharedTarget.sharedNodeGroup,
                indexBuilder, mm, *nodeSharedState->targets[targetIdx].optimisticAllocator);
        }
        numNodesAppended += sharedTarget.sharedNodeGroup->append(*localNodeGroup,
            numNodesAppended /* offsetInNodeGroup */,
            localNodeGroup->getNumRows() - numNodesAppended);
    }
    DASSERT(numNodesAppended == localNodeGroup->getNumRows());
}

void NodeBatchInsert::finalize(ExecutionContext* context) {
    DASSERT(localState == nullptr);
    const auto nodeSharedState =
        dynamic_cast_checked<NodeBatchInsertSharedState*>(sharedState.get());
    auto clientContext = context->clientContext;
    auto transaction = Transaction::Get(*clientContext);
    auto& pageAllocator = *transaction->getLocalStorage()->addOptimisticAllocator();
    for (auto targetIdx = 0u; targetIdx < nodeSharedState->targets.size(); ++targetIdx) {
        auto& sharedTarget = nodeSharedState->targets[targetIdx];
        if (sharedTarget.table == nullptr) {
            // Remotely routed partition: the wrapper owns index finalization.
            continue;
        }
        auto errorHandler = createErrorHandler(context, sharedTarget.table,
            nodeSharedState->duplicatePKSkipResult.get());
        if (sharedTarget.sharedNodeGroup) {
            while (sharedTarget.sharedNodeGroup->getNumRows() > 0) {
                writeAndResetNodeGroup(transaction, targetIdx, sharedTarget.sharedNodeGroup,
                    sharedTarget.globalIndexBuilder, MemoryManager::Get(*clientContext),
                    errorHandler, pageAllocator);
            }
        }
        if (sharedTarget.globalIndexBuilder) {
            sharedTarget.globalIndexBuilder->finalize(context, errorHandler);
            errorHandler.flushStoredErrors();
        }
        if (sharedTarget.noIndexPKValidator) {
            // Completes cross-chunk duplicate detection for the no-hash-index COPY path. Sorted
            // runs spilled to disk during validate() are stream-merged here, so duplicates that
            // span chunks are reported before the transaction commits.
            sharedTarget.noIndexPKValidator->finalize();
        }
        for (auto& index : sharedTarget.table->getIndexes()) {
            index.finalize(clientContext);
        }
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
