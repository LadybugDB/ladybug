#pragma once

#include "common/data_chunk/sel_vector.h"
#include "common/types/types.h"

namespace lbug {
namespace transaction {
class Transaction;
} // namespace transaction

namespace storage {

class ChunkedNodeGroup;
struct VectorVersionInfo;

class LBUG_API VersionInfo {
public:
    VersionInfo();
    ~VersionInfo();
    DELETE_BOTH_COPY(VersionInfo);

    void append(common::transaction_t transactionID, common::row_idx_t startRow,
        common::row_idx_t numRows);
    bool delete_(common::transaction_t transactionID, common::row_idx_t rowIdx);

    bool isSelected(common::transaction_t startTS, common::transaction_t transactionID,
        common::row_idx_t rowIdx) const;
    void getSelVectorToScan(common::transaction_t startTS, common::transaction_t transactionID,
        common::SelectionVector& selVector, common::row_idx_t startRow,
        common::row_idx_t numRows) const;

    void clearVectorInfo(common::idx_t vectorIdx);

    bool hasDeletions() const;
    common::row_idx_t getNumDeletions(const transaction::Transaction* transaction,
        common::row_idx_t startRow, common::length_t numRows) const;
    bool hasInsertions() const;
    bool isDeleted(const transaction::Transaction* transaction, common::row_idx_t rowInChunk) const;
    bool isInserted(const transaction::Transaction* transaction,
        common::row_idx_t rowInChunk) const;

    bool hasDeletions(const transaction::Transaction* transaction) const;

    common::idx_t getNumVectors() const { return vectorsInfo.size(); }

    // Insert/delete commit timestamp of a single row, or INVALID_TRANSACTION when the row
    // carries no entry (baked pre-history). Uncommitted entries surface as their writer ID.
    common::transaction_t getInsertCommitTS(common::row_idx_t rowInChunk) const;
    common::transaction_t getDeleteCommitTS(common::row_idx_t rowInChunk) const;
    // Records a retained insert stamp on a checkpoint-built VersionInfo. Rows never stamped
    // stay baked-visible (fresh vectors default to visible); only stamped rows filter.
    void stampInsertCommitTS(common::row_idx_t rowInChunk, common::transaction_t commitTS);
    void stampDeleteCommitTS(common::row_idx_t rowInChunk, common::transaction_t commitTS);
    // True when any entry still filters readers at the horizon (committed at/after it, or
    // uncommitted). Baked entries (0) and absent entries do not count.
    bool hasRetainedEntries(common::transaction_t horizon) const;

    void commitInsert(common::row_idx_t startRow, common::row_idx_t numRows,
        common::transaction_t commitTS);
    void rollbackInsert(common::row_idx_t startRow, common::row_idx_t numRows);
    void commitDelete(common::row_idx_t startRow, common::row_idx_t numRows,
        common::transaction_t commitTS);
    void rollbackDelete(common::row_idx_t startRow, common::row_idx_t numRows);

    void serialize(common::Serializer& serializer) const;
    static std::unique_ptr<VersionInfo> deserialize(common::Deserializer& deSer);

private:
    // Return nullptr when vectorIdx is out of range or when the vector is not created.
    VectorVersionInfo* getVectorVersionInfo(common::idx_t vectorIdx) const;
    VectorVersionInfo& getOrCreateVersionInfo(common::idx_t vectorIdx);

    std::vector<std::unique_ptr<VectorVersionInfo>> vectorsInfo;
};

} // namespace storage
} // namespace lbug
