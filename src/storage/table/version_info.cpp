#include "storage/table/version_info.h"

#include <atomic>

#include "common/exception/runtime.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "storage/storage_utils.h"
#include "transaction/transaction.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

struct VectorVersionInfo {
    enum class InsertionStatus : uint8_t { NO_INSERTED, CHECK_VERSION, ALWAYS_INSERTED };
    // TODO(Guodong): ALWAYS_INSERTED is not added for now, but it may be useful as an optimization
    // to mark the vector data after checkpoint is all deleted.
    enum class DeletionStatus : uint8_t { NO_DELETED, CHECK_VERSION };

    // TODO: Keep an additional same insertion/deletion field as an optimization to avoid the need
    // of `array` if all are inserted/deleted in the same transaction.
    // Also, avoid allocate `array` when status are NO_INSERTED and NO_DELETED.
    // We can even consider separating the insertion and deletion into two separate Vectors.
    std::unique_ptr<std::array<transaction_t, DEFAULT_VECTOR_CAPACITY>> insertedVersions;
    std::unique_ptr<std::array<transaction_t, DEFAULT_VECTOR_CAPACITY>> deletedVersions;
    // If all values in the Vector are inserted/deleted in the same transaction, we can use this to
    // avoid the allocation of `array`.
    transaction_t sameInsertionVersion;
    transaction_t sameDeletionVersion;
    // Rows of `insertedVersions` holding a commit timestamp, and the largest such timestamp; kept
    // in step by setInsertedVersion and fillInsertedVersions. Never serialized: on disk insertions
    // are never versioned. Read without the node group lock: the writer publishes the timestamp
    // before the count.
    std::atomic<row_idx_t> numCommittedInsertions;
    std::atomic<transaction_t> maxCommittedInsertion;
    InsertionStatus insertionStatus;
    DeletionStatus deletionStatus;

    VectorVersionInfo()
        : sameInsertionVersion{INVALID_TRANSACTION}, sameDeletionVersion{INVALID_TRANSACTION},
          numCommittedInsertions{0}, maxCommittedInsertion{0},
          insertionStatus{InsertionStatus::NO_INSERTED},
          deletionStatus{DeletionStatus::NO_DELETED} {}
    DELETE_COPY_AND_MOVE(VectorVersionInfo);

    void append(transaction_t transactionID, row_idx_t startRow, row_idx_t numRows);
    bool delete_(transaction_t transactionID, row_idx_t rowIdx);
    transaction_t getInsertCommitTS(row_idx_t rowIdxInVector) const;
    transaction_t getDeleteCommitTS(row_idx_t rowIdxInVector) const;
    void stampInsertCommitTS(row_idx_t rowIdxInVector, transaction_t commitTS);
    void stampDeleteCommitTS(row_idx_t rowIdxInVector, transaction_t commitTS);
    bool hasRetainedEntries(transaction_t horizon) const;
    void setInsertCommitTS(transaction_t commitTS, row_idx_t startRow, row_idx_t numRows);
    void setDeleteCommitTS(transaction_t commitTS, row_idx_t startRow, row_idx_t numRows);

    bool isSelected(transaction_t startTS, transaction_t transactionID, row_idx_t rowIdx) const;
    void getSelVectorForScan(transaction_t startTS, transaction_t transactionID,
        SelectionVector& selVector, row_idx_t startRow, row_idx_t numRows,
        sel_t startOutputPos) const;

    void rollbackInsertions(row_idx_t startRowInVector, row_idx_t numRows);
    void rollbackDeletions(row_idx_t startRowInVector, row_idx_t numRows);

    bool hasDeletions(const transaction::Transaction* transaction) const;

    // Given startTS and transactionID, if the row is deleted to the transaction, return true.
    bool isDeleted(transaction_t startTS, transaction_t transactionID, row_idx_t rowIdx) const;
    // Given startTS and transactionID, if the row is readable to the transaction, return true.
    bool isInserted(transaction_t startTS, transaction_t transactionID, row_idx_t rowIdx) const;

    row_idx_t getNumDeletions(transaction_t startTS, transaction_t transactionID,
        row_idx_t startRow, length_t numRows) const;
    row_idx_t getNumInsertedRows(transaction_t startTS, transaction_t transactionID,
        row_idx_t startRow, length_t numRows) const;
    row_idx_t getNumVisibleRows(transaction_t startTS, transaction_t transactionID,
        row_idx_t startRow, length_t numRows) const;

    void serialize(Serializer& serializer) const;
    static std::unique_ptr<VectorVersionInfo> deSerialize(Deserializer& deSer);

private:
    void initInsertionVersionArray();
    void initDeletionVersionArray();
    void setInsertedVersion(row_idx_t rowIdx, transaction_t version);
    void fillInsertedVersions(transaction_t version);

    bool isSameInsertionVersion() const;
    bool isSameDeletionVersion() const;
};

void VectorVersionInfo::append(const transaction_t transactionID, const row_idx_t startRow,
    const row_idx_t numRows) {
    insertionStatus = InsertionStatus::CHECK_VERSION;
    if (transactionID == sameInsertionVersion) {
        return;
    }
    if (!isSameInsertionVersion() && !insertedVersions) {
        // No insertions before, and no need to allocate array.
        sameInsertionVersion = transactionID;
        return;
    }
    if (!insertedVersions) {
        initInsertionVersionArray();
        for (auto i = 0u; i < startRow; i++) {
            setInsertedVersion(i, sameInsertionVersion);
        }
        sameInsertionVersion = INVALID_TRANSACTION;
    }
    for (auto i = 0u; i < numRows; i++) {
        DASSERT(insertedVersions->operator[](startRow + i) == INVALID_TRANSACTION);
        setInsertedVersion(startRow + i, transactionID);
    }
}

bool VectorVersionInfo::delete_(const transaction_t transactionID, const row_idx_t rowIdx) {
    deletionStatus = DeletionStatus::CHECK_VERSION;
    if (transactionID == sameDeletionVersion) {
        // All are deleted in the same transaction.
        return false;
    }
    if (isSameDeletionVersion()) {
        // All are deleted in a different transaction.
        throw RuntimeException(
            "Write-write conflict: deleting a row that is already deleted by another transaction.");
    }
    if (!deletedVersions) {
        // No deletions before.
        initDeletionVersionArray();
    }
    if (deletedVersions->operator[](rowIdx) == transactionID) {
        return false;
    }
    if (deletedVersions->operator[](rowIdx) != INVALID_TRANSACTION) {
        throw RuntimeException(
            "Write-write conflict: deleting a row that is already deleted by another transaction.");
    }
    deletedVersions->operator[](rowIdx) = transactionID;
    return true;
}

void VectorVersionInfo::setInsertCommitTS(transaction_t commitTS, row_idx_t startRow,
    row_idx_t numRows) {
    if (isSameInsertionVersion()) {
        sameInsertionVersion = commitTS;
        return;
    }
    DASSERT(insertedVersions);
    for (auto rowIdx = startRow; rowIdx < startRow + numRows; rowIdx++) {
        setInsertedVersion(rowIdx, commitTS);
    }
}

void VectorVersionInfo::setDeleteCommitTS(transaction_t commitTS, row_idx_t startRow,
    row_idx_t numRows) {
    if (isSameDeletionVersion()) {
        sameDeletionVersion = commitTS;
        return;
    }
    DASSERT(deletedVersions);
    for (auto rowIdx = startRow; rowIdx < startRow + numRows; rowIdx++) {
        deletedVersions->operator[](rowIdx) = commitTS;
    }
}

transaction_t VectorVersionInfo::getInsertCommitTS(row_idx_t rowIdxInVector) const {
    if (isSameInsertionVersion()) {
        return sameInsertionVersion;
    }
    if (!insertedVersions) {
        return INVALID_TRANSACTION;
    }
    return insertedVersions->operator[](rowIdxInVector);
}

transaction_t VectorVersionInfo::getDeleteCommitTS(row_idx_t rowIdxInVector) const {
    if (isSameDeletionVersion()) {
        return sameDeletionVersion;
    }
    if (!deletedVersions) {
        return INVALID_TRANSACTION;
    }
    return deletedVersions->operator[](rowIdxInVector);
}

void VectorVersionInfo::stampInsertCommitTS(row_idx_t rowIdxInVector, transaction_t commitTS) {
    insertionStatus = InsertionStatus::CHECK_VERSION;
    if (isSameInsertionVersion()) {
        if (sameInsertionVersion == commitTS) {
            return;
        }
        initInsertionVersionArray();
        fillInsertedVersions(sameInsertionVersion);
        sameInsertionVersion = INVALID_TRANSACTION;
    }
    if (!insertedVersions) {
        // Fresh checkpoint-built vector: unstamped rows are baked-visible, so default the
        // array to visible-to-all rather than absent.
        initInsertionVersionArray();
        fillInsertedVersions(0);
    }
    setInsertedVersion(rowIdxInVector, commitTS);
}

void VectorVersionInfo::stampDeleteCommitTS(row_idx_t rowIdxInVector, transaction_t commitTS) {
    deletionStatus = DeletionStatus::CHECK_VERSION;
    if (isSameDeletionVersion()) {
        if (sameDeletionVersion == commitTS) {
            return;
        }
        initDeletionVersionArray();
        deletedVersions->fill(sameDeletionVersion);
        sameDeletionVersion = INVALID_TRANSACTION;
    }
    if (!deletedVersions) {
        // Fresh checkpoint-built vector: unstamped rows carry no delete.
        initDeletionVersionArray();
    }
    deletedVersions->operator[](rowIdxInVector) = commitTS;
}

bool VectorVersionInfo::hasRetainedEntries(transaction_t horizon) const {
    if (sameInsertionVersion != INVALID_TRANSACTION && sameInsertionVersion >= horizon) {
        return true;
    }
    if (sameDeletionVersion != INVALID_TRANSACTION && sameDeletionVersion >= horizon) {
        return true;
    }
    if (insertedVersions) {
        for (const auto version : *insertedVersions) {
            if (version != INVALID_TRANSACTION && version >= horizon) {
                return true;
            }
        }
    }
    if (deletedVersions) {
        for (const auto version : *deletedVersions) {
            if (version != INVALID_TRANSACTION && version >= horizon) {
                return true;
            }
        }
    }
    return false;
}

bool VectorVersionInfo::isSelected(const transaction_t startTS, const transaction_t transactionID,
    const row_idx_t rowIdx) const {
    if (deletionStatus == DeletionStatus::NO_DELETED &&
        insertionStatus == InsertionStatus::ALWAYS_INSERTED) {
        return true;
    }
    if (insertionStatus == InsertionStatus::NO_INSERTED) {
        return false;
    }
    if (isInserted(startTS, transactionID, rowIdx)) {
        return !isDeleted(startTS, transactionID, rowIdx);
    }
    return false;
}

void VectorVersionInfo::getSelVectorForScan(const transaction_t startTS,
    const transaction_t transactionID, SelectionVector& selVector, const row_idx_t startRow,
    const row_idx_t numRows, sel_t startOutputPos) const {
    auto numSelected = selVector.getSelSize();
    if (deletionStatus == DeletionStatus::NO_DELETED &&
        insertionStatus == InsertionStatus::ALWAYS_INSERTED) {
        if (selVector.isUnfiltered()) {
            selVector.setSelSize(selVector.getSelSize() + numRows);
        } else {
            for (auto i = 0u; i < numRows; i++) {
                selVector.getMutableBuffer()[numSelected++] = startOutputPos + i;
            }
            selVector.setToFiltered(numSelected);
        }
    } else if (insertionStatus != InsertionStatus::NO_INSERTED) {
        // If there were no deleted values up to this point the selVector may be unfiltered but have
        // non-zero size, and the mutable buffer may have arbitrary contents
        if (selVector.isUnfiltered()) {
            selVector.makeDynamic();
        }
        for (auto i = 0u; i < numRows; i++) {
            if (const auto rowIdx = startRow + i; isInserted(startTS, transactionID, rowIdx) &&
                                                  !isDeleted(startTS, transactionID, rowIdx)) {
                selVector.getMutableBuffer()[numSelected++] = startOutputPos + i;
            }
        }
        selVector.setToFiltered(numSelected);
    }
}

bool VectorVersionInfo::isDeleted(const transaction_t startTS, const transaction_t transactionID,
    const row_idx_t rowIdx) const {
    switch (deletionStatus) {
    case DeletionStatus::NO_DELETED: {
        return false;
    }
    case DeletionStatus::CHECK_VERSION: {
        transaction_t deletion = INVALID_TRANSACTION;
        if (isSameDeletionVersion()) {
            deletion = sameDeletionVersion;
        } else {
            DASSERT(deletedVersions);
            deletion = deletedVersions->operator[](rowIdx);
        }
        const auto isDeletedWithinSameTransaction = deletion == transactionID;
        const auto isDeletedByPrevCommittedTransaction = deletion <= startTS;
        return isDeletedWithinSameTransaction || isDeletedByPrevCommittedTransaction;
    }
    default: {
        UNREACHABLE_CODE;
    }
    }
}

bool VectorVersionInfo::isInserted(const transaction_t startTS, const transaction_t transactionID,
    const row_idx_t rowIdx) const {
    switch (insertionStatus) {
    case InsertionStatus::ALWAYS_INSERTED: {
        return true;
    }
    case InsertionStatus::NO_INSERTED: {
        return false;
    }
    case InsertionStatus::CHECK_VERSION: {
        transaction_t insertion = INVALID_TRANSACTION;
        if (isSameInsertionVersion()) {
            insertion = sameInsertionVersion;
        } else {
            DASSERT(insertedVersions);
            insertion = insertedVersions->operator[](rowIdx);
        }
        const auto isInsertedWithinSameTransaction = insertion == transactionID;
        const auto isInsertedByPrevCommittedTransaction = insertion <= startTS;
        return isInsertedWithinSameTransaction || isInsertedByPrevCommittedTransaction;
    }
    default: {
        UNREACHABLE_CODE;
    }
    }
}

row_idx_t VectorVersionInfo::getNumDeletions(transaction_t startTS, transaction_t transactionID,
    row_idx_t startRow, length_t numRows) const {
    if (deletionStatus == DeletionStatus::NO_DELETED) {
        return 0;
    }
    row_idx_t numDeletions = 0u;
    for (auto i = 0u; i < numRows; i++) {
        numDeletions += isDeleted(startTS, transactionID, startRow + i);
    }
    return numDeletions;
}

row_idx_t VectorVersionInfo::getNumInsertedRows(transaction_t startTS, transaction_t transactionID,
    row_idx_t startRow, length_t numRows) const {
    if (insertionStatus == InsertionStatus::NO_INSERTED) {
        return 0;
    }
    if (insertionStatus == InsertionStatus::ALWAYS_INSERTED || isSameInsertionVersion()) {
        return isInserted(startTS, transactionID, startRow) ? numRows : 0;
    }
    // Rows are appended in commit order by one writer at a time and rolled back from the end,
    // so committed rows form a prefix: if they number numRows, they are [0, numRows). The count
    // is read first, so the timestamp read after it covers every row it counts.
    if (startRow == 0 && numCommittedInsertions.load(std::memory_order_acquire) == numRows &&
        maxCommittedInsertion.load(std::memory_order_relaxed) <= startTS) {
        return numRows;
    }
    row_idx_t numInserted = 0u;
    for (auto i = 0u; i < numRows; i++) {
        const auto insertion = insertedVersions->operator[](startRow + i);
        numInserted += insertion == transactionID || insertion <= startTS;
    }
    return numInserted;
}

row_idx_t VectorVersionInfo::getNumVisibleRows(transaction_t startTS, transaction_t transactionID,
    row_idx_t startRow, length_t numRows) const {
    if (insertionStatus == InsertionStatus::NO_INSERTED) {
        return 0;
    }
    if (deletionStatus == DeletionStatus::NO_DELETED) {
        return getNumInsertedRows(startTS, transactionID, startRow, numRows);
    }
    row_idx_t numVisible = 0u;
    for (auto i = 0u; i < numRows; i++) {
        numVisible += isSelected(startTS, transactionID, startRow + i);
    }
    return numVisible;
}

void VectorVersionInfo::rollbackInsertions(row_idx_t startRowInVector, row_idx_t numRows) {
    if (isSameInsertionVersion()) {
        // This implicitly assumes that all rows are inserted in the same transaction, so regardless
        // which rows to be rolled back, we just reset the sameInsertionVersion.
        sameInsertionVersion = INVALID_TRANSACTION;
    } else {
        if (insertedVersions) {
            for (auto row = startRowInVector; row < startRowInVector + numRows; row++) {
                setInsertedVersion(row, INVALID_TRANSACTION);
            }
            bool hasAnyInsertions = false;
            for (const auto& version : *insertedVersions) {
                if (version != INVALID_TRANSACTION) {
                    hasAnyInsertions = true;
                    break;
                }
            }
            if (!hasAnyInsertions) {
                insertedVersions.reset();
            }
        }
    }
    if (!insertedVersions) {
        insertionStatus = InsertionStatus::NO_INSERTED;
        deletionStatus = DeletionStatus::NO_DELETED;
    }
}

void VectorVersionInfo::rollbackDeletions(row_idx_t startRowInVector, row_idx_t numRows) {
    if (isSameDeletionVersion()) {
        // This implicitly assumes that all rows are deleted in the same transaction, so regardless
        // which rows to be rollbacked, we just reset the sameInsertionVersion.
        sameDeletionVersion = INVALID_TRANSACTION;
    } else {
        if (deletedVersions) {
            for (auto row = startRowInVector; row < startRowInVector + numRows; row++) {
                deletedVersions->operator[](row) = INVALID_TRANSACTION;
            }
            bool hasAnyDeletions = false;
            for (const auto& version : *deletedVersions) {
                if (version != INVALID_TRANSACTION) {
                    hasAnyDeletions = true;
                    break;
                }
            }
            if (!hasAnyDeletions) {
                deletedVersions.reset();
            }
        }
    }
    if (!deletedVersions) {
        deletionStatus = DeletionStatus::NO_DELETED;
    }
}

void VectorVersionInfo::initInsertionVersionArray() {
    insertedVersions = std::make_unique<std::array<transaction_t, DEFAULT_VECTOR_CAPACITY>>();
    insertedVersions->fill(INVALID_TRANSACTION);
    numCommittedInsertions.store(0, std::memory_order_relaxed);
    maxCommittedInsertion.store(0, std::memory_order_relaxed);
}

static bool isCommittedVersion(transaction_t version) {
    return version < transaction::Transaction::START_TRANSACTION_ID;
}

// Called by the single writer only.
void VectorVersionInfo::setInsertedVersion(row_idx_t rowIdx, transaction_t version) {
    auto& slot = insertedVersions->operator[](rowIdx);
    if (isCommittedVersion(slot)) {
        numCommittedInsertions.fetch_sub(1, std::memory_order_relaxed);
    }
    if (isCommittedVersion(version)) {
        if (version > maxCommittedInsertion.load(std::memory_order_relaxed)) {
            maxCommittedInsertion.store(version, std::memory_order_relaxed);
        }
        numCommittedInsertions.fetch_add(1, std::memory_order_release);
    }
    slot = version;
}

void VectorVersionInfo::fillInsertedVersions(transaction_t version) {
    insertedVersions->fill(version);
    const auto committed = isCommittedVersion(version);
    maxCommittedInsertion.store(committed ? version : 0, std::memory_order_relaxed);
    numCommittedInsertions.store(committed ? DEFAULT_VECTOR_CAPACITY : 0,
        std::memory_order_release);
}

void VectorVersionInfo::initDeletionVersionArray() {
    deletedVersions = std::make_unique<std::array<transaction_t, DEFAULT_VECTOR_CAPACITY>>();
    deletedVersions->fill(INVALID_TRANSACTION);
}

bool VectorVersionInfo::isSameInsertionVersion() const {
    return sameInsertionVersion != INVALID_TRANSACTION;
}

bool VectorVersionInfo::isSameDeletionVersion() const {
    return sameDeletionVersion != INVALID_TRANSACTION;
}

namespace {
// Collapse a committed deletion timestamp to the always-deleted wire marker (0): no
// pre-restart reader survives recovery and post-restart timestamps restart at 1, so the
// stored commit timestamp could never satisfy (deletion <= startTS) again and the row would
// wrongly reappear. INVALID (live) and uncommitted entries (never expected on the wire;
// checkpoints drain writers first) pass through unchanged.
transaction_t collapseCommittedDelete(transaction_t version) {
    if (version != INVALID_TRANSACTION &&
        version < transaction::Transaction::START_TRANSACTION_ID) {
        return 0;
    }
    return version;
}
} // namespace

void VectorVersionInfo::serialize(Serializer& serializer) const {
    if (deletedVersions) {
        for (const auto deleted : *deletedVersions) {
            // Versions should be either INVALID_TRANSACTION or committed timestamps.
            DASSERT(deleted == INVALID_TRANSACTION ||
                    deleted < transaction::Transaction::START_TRANSACTION_ID);
            UNUSED(deleted);
        }
    }
    // Retained insert stamps are memory-only: no pre-restart reader survives recovery and
    // post-restart timestamps start fresh, so collapsing them to baked-visible on the wire
    // resolves identically after a restart while keeping the durable encoding unchanged.
    const auto wireInsertionStatus = insertionStatus == InsertionStatus::CHECK_VERSION ?
                                         InsertionStatus::ALWAYS_INSERTED :
                                         insertionStatus;
    serializer.writeDebuggingInfo("insertion_status");
    serializer.serializeValue<InsertionStatus>(wireInsertionStatus);
    serializer.writeDebuggingInfo("deletion_status");
    serializer.serializeValue<DeletionStatus>(deletionStatus);
    switch (deletionStatus) {
    case DeletionStatus::NO_DELETED: {
        // Nothing to serialize.
    } break;
    case DeletionStatus::CHECK_VERSION: {
        // Retained delete stamps are memory-only, like inserts above: post-restart
        // timestamps start fresh, so a stored commit timestamp would never satisfy
        // (deletion <= startTS) and the row would wrongly reappear. Committed deletes
        // collapse to the always-deleted marker 0; anything else (INVALID, or an
        // unexpected uncommitted entry) passes through unchanged.
        serializer.writeDebuggingInfo("same_deletion_version");
        serializer.serializeValue<transaction_t>(collapseCommittedDelete(sameDeletionVersion));
        if (sameDeletionVersion == INVALID_TRANSACTION) {
            DASSERT(deletedVersions);
            serializer.writeDebuggingInfo("deleted_versions");
            std::array<transaction_t, DEFAULT_VECTOR_CAPACITY> wireVersions;
            for (auto i = 0u; i < DEFAULT_VECTOR_CAPACITY; i++) {
                wireVersions[i] = collapseCommittedDelete((*deletedVersions)[i]);
            }
            serializer.serializeArray<transaction_t, DEFAULT_VECTOR_CAPACITY>(wireVersions);
        }
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
}

std::unique_ptr<VectorVersionInfo> VectorVersionInfo::deSerialize(Deserializer& deSer) {
    std::string key;
    auto vectorVersionInfo = std::make_unique<VectorVersionInfo>();
    deSer.validateDebuggingInfo(key, "insertion_status");
    deSer.deserializeValue<InsertionStatus>(vectorVersionInfo->insertionStatus);
    DASSERT(vectorVersionInfo->insertionStatus == InsertionStatus::NO_INSERTED ||
            vectorVersionInfo->insertionStatus == InsertionStatus::ALWAYS_INSERTED);
    deSer.validateDebuggingInfo(key, "deletion_status");
    deSer.deserializeValue<DeletionStatus>(vectorVersionInfo->deletionStatus);
    switch (vectorVersionInfo->deletionStatus) {
    case DeletionStatus::NO_DELETED: {
        // Nothing to deserialize.
    } break;
    case DeletionStatus::CHECK_VERSION: {
        deSer.validateDebuggingInfo(key, "same_deletion_version");
        deSer.deserializeValue<transaction_t>(vectorVersionInfo->sameDeletionVersion);
        if (vectorVersionInfo->sameDeletionVersion == INVALID_TRANSACTION) {
            deSer.validateDebuggingInfo(key, "deleted_versions");
            vectorVersionInfo->initDeletionVersionArray();
            deSer.deserializeArray<transaction_t, DEFAULT_VECTOR_CAPACITY>(
                *vectorVersionInfo->deletedVersions);
        }
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
    if (vectorVersionInfo->deletedVersions) {
        for (const auto deleted : *vectorVersionInfo->deletedVersions) {
            // Versions should be either INVALID_TRANSACTION or committed timestamps.
            DASSERT(deleted == INVALID_TRANSACTION ||
                    deleted < transaction::Transaction::START_TRANSACTION_ID);
            UNUSED(deleted);
        }
    }
    return vectorVersionInfo;
}

bool VectorVersionInfo::hasDeletions(const transaction::Transaction* transaction) const {
    if (deletionStatus == DeletionStatus::NO_DELETED) {
        return false;
    }
    if (isSameDeletionVersion()) {
        return sameDeletionVersion <= transaction->getStartTS() ||
               sameDeletionVersion == transaction->getID();
    }
    for (auto i = 0u; i < DEFAULT_VECTOR_CAPACITY; i++) {
        if (isDeleted(transaction->getStartTS(), transaction->getID(), i)) {
            return true;
        }
    }
    return false;
}

VectorVersionInfo& VersionInfo::getOrCreateVersionInfo(idx_t vectorIdx) {
    if (vectorsInfo.size() <= vectorIdx) {
        vectorsInfo.resize(vectorIdx + 1);
    }
    if (!vectorsInfo[vectorIdx]) {
        vectorsInfo[vectorIdx] = std::make_unique<VectorVersionInfo>();
    }
    return *vectorsInfo[vectorIdx];
}

VectorVersionInfo* VersionInfo::getVectorVersionInfo(idx_t vectorIdx) const {
    if (vectorIdx >= vectorsInfo.size()) {
        return nullptr;
    }
    return vectorsInfo[vectorIdx].get();
}

VersionInfo::VersionInfo() = default;
VersionInfo::~VersionInfo() = default;

void VersionInfo::append(transaction_t transactionID, const row_idx_t startRow,
    const row_idx_t numRows) {
    if (numRows == 0) {
        return;
    }
    auto [startVectorIdx, startRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    for (auto vectorIdx = startVectorIdx; vectorIdx <= endVectorIdx; vectorIdx++) {
        auto& vectorVersionInfo = getOrCreateVersionInfo(vectorIdx);
        const auto startRowIdx = vectorIdx == startVectorIdx ? startRowIdxInVector : 0;
        const auto endRowIdx =
            vectorIdx == endVectorIdx ? endRowIdxInVector : DEFAULT_VECTOR_CAPACITY - 1;
        const auto numRowsInVector = endRowIdx - startRowIdx + 1;
        vectorVersionInfo.append(transactionID, startRowIdx, numRowsInVector);
    }
}

bool VersionInfo::delete_(transaction_t transactionID, const row_idx_t rowIdx) {
    auto [vectorIdx, rowIdxInVector] =
        StorageUtils::getQuotientRemainder(rowIdx, DEFAULT_VECTOR_CAPACITY);
    auto& vectorVersionInfo = getOrCreateVersionInfo(vectorIdx);
    if (vectorVersionInfo.insertionStatus == VectorVersionInfo::InsertionStatus::NO_INSERTED) {
        // Note: The version info is newly created due to `delete_`. There is no newly inserted rows
        // in this vector, thus all are rows checkpointed. We set the insertion status to
        // ALWAYS_INSERTED to avoid checking the version in the future.
        vectorVersionInfo.insertionStatus = VectorVersionInfo::InsertionStatus::ALWAYS_INSERTED;
    }
    return vectorVersionInfo.delete_(transactionID, rowIdxInVector);
}

bool VersionInfo::isSelected(transaction_t startTS, transaction_t transactionID,
    row_idx_t rowIdx) const {
    auto [vectorIdx, rowIdxInVector] =
        StorageUtils::getQuotientRemainder(rowIdx, DEFAULT_VECTOR_CAPACITY);
    if (const auto vectorVersion = getVectorVersionInfo(vectorIdx)) {
        return vectorVersion->isSelected(startTS, transactionID, rowIdxInVector);
    }
    return true;
}

void VersionInfo::getSelVectorToScan(const transaction_t startTS, const transaction_t transactionID,
    SelectionVector& selVector, const row_idx_t startRow, const row_idx_t numRows) const {
    if (numRows == 0) {
        return;
    }
    auto [startVectorIdx, startRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    auto vectorIdx = startVectorIdx;
    selVector.setToUnfiltered(0);
    sel_t outputPos = 0u;
    while (vectorIdx <= endVectorIdx) {
        const auto startRowIdx = vectorIdx == startVectorIdx ? startRowIdxInVector : 0;
        const auto endRowIdx =
            vectorIdx == endVectorIdx ? endRowIdxInVector : DEFAULT_VECTOR_CAPACITY - 1;
        const auto numRowsInVector = endRowIdx - startRowIdx + 1;
        const auto vectorVersion = getVectorVersionInfo(vectorIdx);
        if (!vectorVersion) {
            auto numSelected = selVector.getSelSize();
            if (selVector.isUnfiltered()) {
                selVector.setSelSize(numSelected + numRowsInVector);
            } else {
                for (auto i = 0u; i < numRowsInVector; i++) {
                    selVector.getMutableBuffer()[numSelected++] = outputPos + i;
                }
                selVector.setToFiltered(numSelected);
            }
        } else {
            vectorVersion->getSelVectorForScan(startTS, transactionID, selVector, startRowIdx,
                numRowsInVector, outputPos);
        }
        outputPos += numRowsInVector;
        vectorIdx++;
    }
    DASSERT(outputPos <= DEFAULT_VECTOR_CAPACITY);
}

void VersionInfo::clearVectorInfo(const idx_t vectorIdx) {
    DASSERT(vectorIdx < vectorsInfo.size());
    vectorsInfo[vectorIdx] = nullptr;
}

bool VersionInfo::hasDeletions() const {
    for (auto& vectorInfo : vectorsInfo) {
        if (vectorInfo &&
            vectorInfo->deletionStatus == VectorVersionInfo::DeletionStatus::CHECK_VERSION) {
            return true;
        }
    }
    return false;
}

row_idx_t VersionInfo::getNumDeletions(const transaction::Transaction* transaction,
    row_idx_t startRow, length_t numRows) const {
    if (numRows == 0) {
        return 0;
    }
    auto [startVector, startRowInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    idx_t vectorIdx = startVector;
    row_idx_t numDeletions = 0u;
    while (vectorIdx <= endVectorIdx) {
        const auto rowInVector = vectorIdx == startVector ? startRowInVector : 0;
        const auto numRowsInVector = vectorIdx == endVectorIdx ?
                                         endRowInVector - rowInVector + 1 :
                                         DEFAULT_VECTOR_CAPACITY - rowInVector;
        const auto vectorVersion = getVectorVersionInfo(vectorIdx);
        if (vectorVersion) {
            numDeletions += vectorVersion->getNumDeletions(transaction->getStartTS(),
                transaction->getID(), rowInVector, numRowsInVector);
        }
        vectorIdx++;
    }
    return numDeletions;
}

row_idx_t VersionInfo::getNumVisibleRows(const transaction::Transaction* transaction,
    row_idx_t startRow, length_t numRows) const {
    if (numRows == 0) {
        return 0;
    }
    auto [startVector, startRowInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    row_idx_t numVisible = 0u;
    for (auto vectorIdx = startVector; vectorIdx <= endVectorIdx; vectorIdx++) {
        const auto rowInVector = vectorIdx == startVector ? startRowInVector : 0;
        const auto numRowsInVector = vectorIdx == endVectorIdx ?
                                         endRowInVector - rowInVector + 1 :
                                         DEFAULT_VECTOR_CAPACITY - rowInVector;
        const auto vectorVersion = getVectorVersionInfo(vectorIdx);
        numVisible += vectorVersion ? vectorVersion->getNumVisibleRows(transaction->getStartTS(),
                                          transaction->getID(), rowInVector, numRowsInVector) :
                                      numRowsInVector;
    }
    return numVisible;
}

row_idx_t VersionInfo::getNumInsertedRows(const transaction::Transaction* transaction,
    row_idx_t startRow, length_t numRows) const {
    if (numRows == 0) {
        return 0;
    }
    auto [startVector, startRowInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    row_idx_t numInserted = 0u;
    for (auto vectorIdx = startVector; vectorIdx <= endVectorIdx; vectorIdx++) {
        const auto rowInVector = vectorIdx == startVector ? startRowInVector : 0;
        const auto numRowsInVector = vectorIdx == endVectorIdx ?
                                         endRowInVector - rowInVector + 1 :
                                         DEFAULT_VECTOR_CAPACITY - rowInVector;
        const auto vectorVersion = getVectorVersionInfo(vectorIdx);
        numInserted += vectorVersion ? vectorVersion->getNumInsertedRows(transaction->getStartTS(),
                                           transaction->getID(), rowInVector, numRowsInVector) :
                                       numRowsInVector;
    }
    return numInserted;
}

void VersionInfo::getDeletedRows(const transaction::Transaction* transaction, row_idx_t startRow,
    length_t numRows, std::vector<row_idx_t>& rows) const {
    if (numRows == 0) {
        return;
    }
    auto [startVector, startRowInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    for (auto vectorIdx = startVector; vectorIdx <= endVectorIdx; vectorIdx++) {
        const auto vectorVersion = getVectorVersionInfo(vectorIdx);
        if (!vectorVersion ||
            vectorVersion->deletionStatus == VectorVersionInfo::DeletionStatus::NO_DELETED) {
            continue;
        }
        const auto rowInVector = vectorIdx == startVector ? startRowInVector : 0;
        const auto endRow =
            vectorIdx == endVectorIdx ? endRowInVector + 1 : DEFAULT_VECTOR_CAPACITY;
        for (auto row = rowInVector; row < endRow; row++) {
            if (vectorVersion->isDeleted(transaction->getStartTS(), transaction->getID(), row)) {
                rows.push_back(vectorIdx * DEFAULT_VECTOR_CAPACITY + row);
            }
        }
    }
}

bool VersionInfo::hasInsertions() const {
    for (auto& vectorInfo : vectorsInfo) {
        if (vectorInfo &&
            vectorInfo->insertionStatus == VectorVersionInfo::InsertionStatus::CHECK_VERSION) {
            return true;
        }
    }
    return false;
}

bool VersionInfo::isDeleted(const transaction::Transaction* transaction,
    row_idx_t rowInChunk) const {
    auto [vectorIdx, rowInVector] =
        StorageUtils::getQuotientRemainder(rowInChunk, DEFAULT_VECTOR_CAPACITY);
    const auto vectorVersion = getVectorVersionInfo(vectorIdx);
    if (vectorVersion) {
        return vectorVersion->isDeleted(transaction->getStartTS(), transaction->getID(),
            rowInVector);
    }
    return false;
}

bool VersionInfo::isInserted(const transaction::Transaction* transaction,
    row_idx_t rowInChunk) const {
    auto [vectorIdx, rowInVector] =
        StorageUtils::getQuotientRemainder(rowInChunk, DEFAULT_VECTOR_CAPACITY);
    const auto vectorVersion = getVectorVersionInfo(vectorIdx);
    if (vectorVersion) {
        return vectorVersion->isInserted(transaction->getStartTS(), transaction->getID(),
            rowInVector);
    }
    return true;
}

bool VersionInfo::hasDeletions(const transaction::Transaction* transaction) const {
    for (auto& vectorInfo : vectorsInfo) {
        if (vectorInfo && vectorInfo->hasDeletions(transaction) > 0) {
            return true;
        }
    }
    return false;
}

transaction_t VersionInfo::getInsertCommitTS(row_idx_t rowInChunk) const {
    auto [vectorIdx, rowInVector] =
        StorageUtils::getQuotientRemainder(rowInChunk, DEFAULT_VECTOR_CAPACITY);
    const auto vectorVersion = getVectorVersionInfo(vectorIdx);
    if (!vectorVersion) {
        return INVALID_TRANSACTION;
    }
    return vectorVersion->getInsertCommitTS(rowInVector);
}

transaction_t VersionInfo::getDeleteCommitTS(row_idx_t rowInChunk) const {
    auto [vectorIdx, rowInVector] =
        StorageUtils::getQuotientRemainder(rowInChunk, DEFAULT_VECTOR_CAPACITY);
    const auto vectorVersion = getVectorVersionInfo(vectorIdx);
    if (!vectorVersion) {
        return INVALID_TRANSACTION;
    }
    return vectorVersion->getDeleteCommitTS(rowInVector);
}

void VersionInfo::stampDeleteCommitTS(row_idx_t rowInChunk, transaction_t commitTS) {
    auto [vectorIdx, rowInVector] =
        StorageUtils::getQuotientRemainder(rowInChunk, DEFAULT_VECTOR_CAPACITY);
    getOrCreateVersionInfo(vectorIdx).stampDeleteCommitTS(rowInVector, commitTS);
}

void VersionInfo::stampInsertCommitTS(row_idx_t rowInChunk, transaction_t commitTS) {
    auto [vectorIdx, rowInVector] =
        StorageUtils::getQuotientRemainder(rowInChunk, DEFAULT_VECTOR_CAPACITY);
    getOrCreateVersionInfo(vectorIdx).stampInsertCommitTS(rowInVector, commitTS);
}

bool VersionInfo::hasRetainedEntries(transaction_t horizon) const {
    for (const auto& vectorVersion : vectorsInfo) {
        if (vectorVersion && vectorVersion->hasRetainedEntries(horizon)) {
            return true;
        }
    }
    return false;
}

void VersionInfo::commitInsert(row_idx_t startRow, row_idx_t numRows, transaction_t commitTS) {
    if (numRows == 0) {
        return;
    }
    auto [startVectorIdx, startRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    for (auto vectorIdx = startVectorIdx; vectorIdx <= endVectorIdx; vectorIdx++) {
        const auto startRowIdx = vectorIdx == startVectorIdx ? startRowIdxInVector : 0;
        const auto endRowIdx =
            vectorIdx == endVectorIdx ? endRowIdxInVector : DEFAULT_VECTOR_CAPACITY - 1;
        auto& vectorVersionInfo = getOrCreateVersionInfo(vectorIdx);
        vectorVersionInfo.setInsertCommitTS(commitTS, startRowIdx, endRowIdx - startRowIdx + 1);
    }
}

void VersionInfo::rollbackInsert(row_idx_t startRow, row_idx_t numRows) {
    if (numRows == 0) {
        return;
    }
    auto [startVectorIdx, startRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    for (auto vectorIdx = startVectorIdx; vectorIdx <= endVectorIdx; vectorIdx++) {
        const auto startRowIdx = vectorIdx == startVectorIdx ? startRowIdxInVector : 0;
        const auto endRowIdx =
            vectorIdx == endVectorIdx ? endRowIdxInVector : DEFAULT_VECTOR_CAPACITY - 1;
        auto& vectorVersionInfo = getOrCreateVersionInfo(vectorIdx);
        vectorVersionInfo.rollbackInsertions(startRowIdx, endRowIdx - startRowIdx + 1);
    }
}

void VersionInfo::commitDelete(row_idx_t startRow, row_idx_t numRows, transaction_t commitTS) {
    if (numRows == 0) {
        return;
    }
    auto [startVectorIdx, startRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    for (auto vectorIdx = startVectorIdx; vectorIdx <= endVectorIdx; vectorIdx++) {
        const auto startRowIdx = vectorIdx == startVectorIdx ? startRowIdxInVector : 0;
        const auto endRowIdx =
            vectorIdx == endVectorIdx ? endRowIdxInVector : DEFAULT_VECTOR_CAPACITY - 1;
        auto& vectorVersionInfo = getOrCreateVersionInfo(vectorIdx);
        vectorVersionInfo.setDeleteCommitTS(commitTS, startRowIdx, endRowIdx - startRowIdx + 1);
    }
}

void VersionInfo::rollbackDelete(row_idx_t startRow, row_idx_t numRows) {
    if (numRows == 0) {
        return;
    }
    auto [startVectorIdx, startRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endRowIdxInVector] =
        StorageUtils::getQuotientRemainder(startRow + numRows - 1, DEFAULT_VECTOR_CAPACITY);
    for (auto vectorIdx = startVectorIdx; vectorIdx <= endVectorIdx; vectorIdx++) {
        auto& vectorVersionInfo = getOrCreateVersionInfo(vectorIdx);
        const auto startRowIdx = vectorIdx == startVectorIdx ? startRowIdxInVector : 0;
        const auto endRowIdx =
            vectorIdx == endVectorIdx ? endRowIdxInVector : DEFAULT_VECTOR_CAPACITY - 1;
        vectorVersionInfo.rollbackDeletions(startRowIdx, endRowIdx - startRowIdx + 1);
    }
}

void VersionInfo::serialize(Serializer& serializer) const {
    serializer.writeDebuggingInfo("vectors_info_size");
    serializer.write<uint64_t>(vectorsInfo.size());
    for (auto i = 0u; i < vectorsInfo.size(); i++) {
        auto hasVectorVersion = vectorsInfo[i] != nullptr;
        serializer.writeDebuggingInfo("has_vector_info");
        serializer.write<bool>(hasVectorVersion);
        if (hasVectorVersion) {
            serializer.writeDebuggingInfo("vector_info");
            vectorsInfo[i]->serialize(serializer);
        }
    }
}

std::unique_ptr<VersionInfo> VersionInfo::deserialize(Deserializer& deSer) {
    std::string key;
    uint64_t vectorSize = 0;
    deSer.validateDebuggingInfo(key, "vectors_info_size");
    deSer.deserializeValue<uint64_t>(vectorSize);
    auto versionInfo = std::make_unique<VersionInfo>();
    for (auto i = 0u; i < vectorSize; i++) {
        bool hasVectorVersion = false;
        deSer.validateDebuggingInfo(key, "has_vector_info");
        deSer.deserializeValue<bool>(hasVectorVersion);
        if (hasVectorVersion) {
            deSer.validateDebuggingInfo(key, "vector_info");
            auto vectorVersionInfo = VectorVersionInfo::deSerialize(deSer);
            versionInfo->vectorsInfo.push_back(std::move(vectorVersionInfo));
        } else {
            versionInfo->vectorsInfo.push_back(nullptr);
        }
    }
    return versionInfo;
}

} // namespace storage
} // namespace lbug
