#pragma once

#include <array>
#include <bitset>
#include <shared_mutex>
#include <vector>

#include "column_chunk_data.h"
#include "common/types/types.h"

namespace lbug {
namespace common {
class ValueVector;
} // namespace common

namespace transaction {
class Transaction;
} // namespace transaction

namespace storage {
class MemoryManager;

class ColumnChunkData;
struct VectorUpdateInfo {
    common::transaction_t version;
    std::array<common::sel_t, common::DEFAULT_VECTOR_CAPACITY> rowsInVector;
    common::sel_t numRowsUpdated;
    // Older versions.
    std::unique_ptr<VectorUpdateInfo> prev;
    // Newer versions.
    VectorUpdateInfo* next;

    std::unique_ptr<ColumnChunkData> data;

    VectorUpdateInfo()
        : version{common::INVALID_TRANSACTION}, rowsInVector{}, numRowsUpdated(0), prev(nullptr),
          next{nullptr}, data{nullptr} {}
    VectorUpdateInfo(MemoryManager& memoryManager, const common::transaction_t transactionID,
        common::LogicalType dataType)
        : version{transactionID}, rowsInVector{}, numRowsUpdated{0}, prev{nullptr}, next{nullptr} {
        data = ColumnChunkFactory::createColumnChunkData(memoryManager, std::move(dataType), false,
            common::DEFAULT_VECTOR_CAPACITY, ResidencyState::IN_MEMORY);
    }

    std::unique_ptr<VectorUpdateInfo> movePrev() { return std::move(prev); }
    void setPrev(std::unique_ptr<VectorUpdateInfo> prev_) { this->prev = std::move(prev_); }
    VectorUpdateInfo* getPrev() const { return prev.get(); }
    void setNext(VectorUpdateInfo* next_) { this->next = next_; }
    VectorUpdateInfo* getNext() const { return next; }
};

struct UpdateNode {
    mutable std::shared_mutex mtx;
    std::unique_ptr<VectorUpdateInfo> info;

    UpdateNode() : info{nullptr} {}
    UpdateNode(UpdateNode&& other) noexcept : info{std::move(other.info)} {}
    UpdateNode(const UpdateNode& other) = delete;

    bool isEmpty() const {
        std::shared_lock lock{mtx};
        return info != nullptr;
    }
    void clear() {
        std::unique_lock lock{mtx};
        info = nullptr;
    }
};

// Retention plan for one update vector at a checkpoint horizon: the chain nodes a
// parked reader (startTS >= horizon) may still observe, plus the row sets that decide
// synthetic horizon base nodes. Raw node pointers are stable across the checkpoint merge:
// writers are drained by the checkpoint gate and readers only take shared locks.
struct VectorRetentionPlan {
    common::idx_t vectorIdx = 0;
    // Chain nodes with version >= horizon (newest-first). Uncommitted entries qualify by
    // comparison (writer IDs sort above any horizon); none are expected post-drain.
    std::vector<VectorUpdateInfo*> kept;
    // Newest chain node with version < horizon, or nullptr when no older node exists.
    VectorUpdateInfo* newestBelow = nullptr;
    // Chunk-row coordinates (within the vector) touched by kept / below-horizon nodes.
    std::bitset<common::DEFAULT_VECTOR_CAPACITY> keptRows;
    std::bitset<common::DEFAULT_VECTOR_CAPACITY> belowRows;
    bool hasKept() const { return !kept.empty(); }
};

class UpdateInfo {
public:
    using iterate_read_from_row_func_t =
        std::function<void(const VectorUpdateInfo&, uint64_t, uint64_t)>;

    UpdateInfo() {}

    VectorUpdateInfo& update(MemoryManager& memoryManager,
        const transaction::Transaction* transaction, common::idx_t vectorIdx,
        common::sel_t rowIdxInVector, const common::ValueVector& values);

    void clearVectorInfo(common::idx_t vectorIdx) {
        std::unique_lock lock{mtx};
        updates[vectorIdx]->clear();
    }

    common::idx_t getNumVectors() const {
        std::shared_lock lock{mtx};
        return updates.size();
    }

    void scan(const transaction::Transaction* transaction, common::ValueVector& output,
        common::offset_t offsetInChunk, common::length_t length) const;
    void lookup(const transaction::Transaction* transaction, common::offset_t rowInChunk,
        common::ValueVector& output, common::sel_t posInOutputVector) const;

    void scanCommitted(const transaction::Transaction* transaction, ColumnChunkData& output,
        common::offset_t startOffsetInOutput, common::row_idx_t startRowScanned,
        common::row_idx_t numRows) const;

    void iterateVectorInfo(const transaction::Transaction* transaction, common::idx_t idx,
        const std::function<void(const VectorUpdateInfo&)>& func) const;

    void commit(common::idx_t vectorIdx, VectorUpdateInfo* info, common::transaction_t commitTS);
    void rollback(common::idx_t vectorIdx, common::transaction_t version);

    common::row_idx_t getNumUpdatedRows(const transaction::Transaction* transaction) const;

    bool hasUpdates(const transaction::Transaction* transaction, common::row_idx_t startRow,
        common::length_t numRows) const;

    bool isSet() const {
        std::shared_lock lock{mtx};
        return !updates.empty();
    }
    // Moves all pending updates from `other` into this, leaving `other` empty. The caller
    // must guarantee `other` holds this object's pre-checkpoint updates (checkpoint reads
    // update info but only resets it on success). Used to preserve uncheckpointed updates
    // when restoring a node group's persistent chunks after a failed checkpoint (see #1051).
    void adoptUpdates(UpdateInfo& other) {
        std::unique_lock lock{mtx};
        std::unique_lock otherLock{other.mtx};
        DASSERT(updates.empty());
        updates = std::move(other.updates);
    }
    void reset() {
        std::unique_lock lock{mtx};
        updates.clear();
    }

    void iterateScan(const transaction::Transaction* transaction, uint64_t startOffsetToScan,
        uint64_t numRowsToScan, uint64_t startPosInOutput,
        const iterate_read_from_row_func_t& readFromRowFunc) const;

    // Classifies every vector chain against the horizon (shared locks only). The checkpoint
    // computes plans before its page-rewrite phase and applies them after; the gate keeps
    // writers out in between, so the captured node pointers stay valid.
    std::vector<VectorRetentionPlan> planRetention(common::transaction_t horizon) const;
    static bool plansHaveKeptNodes(const std::vector<VectorRetentionPlan>& plans);
    // Drops every chain node that is neither kept nor the retained newest-below node of a
    // vector with kept nodes. Prunes empty vector shells only when `exclusive` (no scan
    // shares this object); shared owners keep their shells so in-flight scans never observe
    // structural churn. Content removal is safe either way: every dropped node is older
    // than the horizon, hence invisible to every live scan (startTS >= horizon).
    void dropUnretainedNodes(const std::vector<VectorRetentionPlan>& plans, bool exclusive);
    // Stages one retained row into the chain node stamped with `version`, creating the
    // node when absent. The find-or-create, the duplicate check, and the value copy all
    // run under the chain lock, so in-flight scans never observe a half-staged row.
    // Returns false when the row is already staged (stable across re-checkpoints).
    bool stageRetainedRow(MemoryManager& memoryManager, common::LogicalType dataType,
        common::idx_t vectorIdx, common::transaction_t version, common::sel_t rowInVector,
        const ColumnChunkData& srcData, common::row_idx_t srcPos);
    // Creates the synthetic horizon base node (version == horizon) spliced directly above
    // the newest-below node (or at the tail when none exists), so newer kept nodes keep
    // shadowing it. Returns the existing node when one is already stamped with horizon.
    VectorUpdateInfo& insertRetainedBaseNode(MemoryManager& memoryManager,
        common::LogicalType dataType, common::idx_t vectorIdx, common::transaction_t horizon);

private:
    UpdateNode& getUpdateNode(common::idx_t vectorIdx);
    UpdateNode& getOrCreateUpdateNode(common::idx_t vectorIdx);

private:
    mutable std::shared_mutex mtx;
    std::vector<std::unique_ptr<UpdateNode>> updates;
};

} // namespace storage
} // namespace lbug
