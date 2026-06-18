#pragma once

#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "processor/execution_context.h"
#include "processor/operator/persistent/batch_insert_error_handler.h"

namespace lbug {
namespace storage {
class NodeTable;
}

namespace processor {
enum class NodeCopyErrorKind : uint8_t {
    DUPLICATE_PK,
    NULL_PK,
    OTHER,
};

struct DuplicatePKSkipResult {
    common::row_idx_t skippedCount = 0;
    std::vector<std::string> pks;
    std::mutex mtx;
};

template<typename T>
struct IndexBuilderError {
    std::string message;
    T key;
    common::nodeID_t nodeID;
    NodeCopyErrorKind kind = NodeCopyErrorKind::OTHER;

    // CSV Reader data
    std::optional<WarningSourceData> warningData;
};

class NodeBatchInsertErrorHandler {
public:
    NodeBatchInsertErrorHandler(ExecutionContext* context, common::LogicalTypeID pkType,
        storage::NodeTable* nodeTable, bool ignoreErrors,
        std::shared_ptr<common::row_idx_t> sharedErrorCounter, std::mutex* sharedErrorCounterMtx,
        bool skipDuplicatePK, DuplicatePKSkipResult* duplicatePKSkipResult);

    template<typename T>
    void handleError(IndexBuilderError<T> error) {
        if (error.kind == NodeCopyErrorKind::DUPLICATE_PK && skipDuplicatePK) {
            recordSkippedDuplicatePK(error.key);
            setCurrentErroneousRow(error.key, error.nodeID);
            deleteCurrentErroneousRow();
            return;
        }
        baseErrorHandler.handleError(std::move(error.message), std::move(error.warningData));

        setCurrentErroneousRow(error.key, error.nodeID);
        deleteCurrentErroneousRow();
    }

    void flushStoredErrors();

private:
    template<typename T>
    void recordSkippedDuplicatePK(const T& key) {
        duplicatePKSkipResult->skippedCount++;
        std::string keyStr;
        if constexpr (std::same_as<T, std::string>) {
            keyStr = key;
        } else if constexpr (std::same_as<T, common::string_t>) {
            keyStr = std::string{key.getAsString()};
        } else {
            keyStr = common::TypeUtils::toString(key);
        }
        duplicatePKSkipResult->pks.push_back(std::move(keyStr));
    }

    template<typename T>
    void setCurrentErroneousRow(const T& key, common::nodeID_t nodeID) {
        keyVector->setValue<T>(0, key);
        offsetVector->setValue(0, nodeID);
    }

    void deleteCurrentErroneousRow();

    static constexpr common::idx_t DELETE_VECTOR_SIZE = 1;

    storage::NodeTable* nodeTable;
    ExecutionContext* context;

    // vectors that are reused by each deletion
    std::shared_ptr<common::ValueVector> keyVector;
    std::shared_ptr<common::ValueVector> offsetVector;
    bool skipDuplicatePK;
    DuplicatePKSkipResult* duplicatePKSkipResult;

    BatchInsertErrorHandler baseErrorHandler;
};
} // namespace processor
} // namespace lbug
