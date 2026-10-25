#include "storage/table/update_info.h"

#include <bitset>
#include <unordered_set>

#include "common/exception/runtime.h"
#include "common/vector/value_vector.h"
#include "storage/storage_utils.h"
#include "storage/table/column_chunk_data.h"
#include "transaction/transaction.h"

using namespace lbug::transaction;
using namespace lbug::common;

namespace lbug {
namespace storage {

VectorUpdateInfo& UpdateInfo::update(MemoryManager& memoryManager, const Transaction* transaction,
    const idx_t vectorIdx, const sel_t rowIdxInVector, const ValueVector& values) {
    UpdateNode& header = getOrCreateUpdateNode(vectorIdx);
    // We always lock the head of the chain of vectorUpdateInfo to ensure that we can safely
    // read/write to any part of the chain.
    std::unique_lock chainLock{header.mtx};
    // Traverse the chain of vectorUpdateInfo to find the one that matches the transaction. Also
    // detect if there is any write-write conflicts.
    auto current = header.info.get();
    VectorUpdateInfo* vecUpdateInfo = nullptr;
    while (current) {
        if (current->version == transaction->getID()) {
            // Same transaction, we can update the existing vector info.
            DASSERT(current->version >= Transaction::START_TRANSACTION_ID);
            vecUpdateInfo = current;
        } else if (current->version > transaction->getStartTS()) {
            // Potentially there can be conflicts. `current` can be uncommitted transaction (version
            // is transaction ID) or committed transaction started after this transaction.
            for (auto i = 0u; i < current->numRowsUpdated; i++) {
                if (current->rowsInVector[i] == rowIdxInVector) {
                    throw RuntimeException("Write-write conflict of updating the same row.");
                }
            }
        }
        current = current->prev.get();
    }
    if (!vecUpdateInfo) {
        // Create a new version here if not found in the chain.
        auto newInfo = std::make_unique<VectorUpdateInfo>(memoryManager, transaction->getID(),
            values.dataType.copy());
        vecUpdateInfo = newInfo.get();
        auto currentInfo = std::move(header.info);
        if (currentInfo) {
            currentInfo->next = newInfo.get();
        }
        newInfo->prev = std::move(currentInfo);
        header.info = std::move(newInfo);
    }
    DASSERT(vecUpdateInfo);
    // Check if the row is already updated in this transaction.
    idx_t idxInUpdateData = INVALID_IDX;
    for (auto i = 0u; i < vecUpdateInfo->numRowsUpdated; i++) {
        if (vecUpdateInfo->rowsInVector[i] == rowIdxInVector) {
            idxInUpdateData = i;
            break;
        }
    }
    if (idxInUpdateData != INVALID_IDX) {
        // Overwrite existing update value.
        vecUpdateInfo->data->write(&values, values.state->getSelVector()[0], idxInUpdateData);
    } else {
        // Append new value and update `rowsInVector`.
        vecUpdateInfo->rowsInVector[vecUpdateInfo->numRowsUpdated] = rowIdxInVector;
        vecUpdateInfo->data->write(&values, values.state->getSelVector()[0],
            vecUpdateInfo->numRowsUpdated++);
    }
    return *vecUpdateInfo;
}

void UpdateInfo::scan(const Transaction* transaction, ValueVector& output, offset_t offsetInChunk,
    length_t length) const {
    iterateScan(transaction, offsetInChunk, length, 0 /* startPosInOutput */,
        [&](const VectorUpdateInfo& vecUpdateInfo, uint64_t i, uint64_t posInOutput) -> void {
            vecUpdateInfo.data->lookup(i, output, posInOutput);
        });
}

void UpdateInfo::lookup(const Transaction* transaction, offset_t rowInChunk, ValueVector& output,
    sel_t posInOutputVector) const {
    if (!isSet()) {
        return;
    }
    auto [vectorIdx, rowInVector] =
        StorageUtils::getQuotientRemainder(rowInChunk, DEFAULT_VECTOR_CAPACITY);
    bool updated = false;
    iterateVectorInfo(transaction, vectorIdx, [&](const VectorUpdateInfo& vectorInfo) {
        if (updated) {
            return;
        }
        for (auto i = 0u; i < vectorInfo.numRowsUpdated; i++) {
            if (vectorInfo.rowsInVector[i] == rowInVector) {
                vectorInfo.data->lookup(i, output, posInOutputVector);
                updated = true;
                return;
            }
        }
    });
}

void UpdateInfo::scanCommitted(const Transaction* transaction, ColumnChunkData& output,
    offset_t startOffsetInOutput, row_idx_t startRowScanned, row_idx_t numRows) const {
    iterateScan(transaction, startRowScanned, numRows, startOffsetInOutput,
        [&](const VectorUpdateInfo& vecUpdateInfo, uint64_t i, uint64_t posInOutput) -> void {
            output.write(vecUpdateInfo.data.get(), i, posInOutput, 1);
        });
}

void UpdateInfo::iterateVectorInfo(const Transaction* transaction, idx_t idx,
    const std::function<void(const VectorUpdateInfo&)>& func) const {
    const UpdateNode* head = nullptr;
    {
        std::shared_lock lock{mtx};
        if (idx >= updates.size() || !updates[idx]->isEmpty()) {
            return;
        }
        head = updates[idx].get();
    }
    // We lock the head of the chain to ensure that we can safely read from any part of the
    // chain.
    DASSERT(head);
    std::shared_lock chainLock{head->mtx};
    auto current = head->info.get();
    if (current == nullptr) {
        // The chain was emptied after the prefix check (retention dropped every node
        // below the horizon, or a rollback removed its entries). Either way the base
        // page now holds the newest value, so there is nothing to overlay.
        return;
    }
    while (current) {
        if (current->version == transaction->getID() ||
            current->version <= transaction->getStartTS()) {
            DASSERT((current->version == transaction->getID() &&
                        current->version >= Transaction::START_TRANSACTION_ID) ||
                    (current->version <= transaction->getStartTS() &&
                        current->version < Transaction::START_TRANSACTION_ID));
            func(*current);
        }
        current = current->getPrev();
    }
}

#if defined(RUNTIME_CHECKS) || !defined(NDEBUG)
// Assert that info is in the updatedNode version chain.
static bool validateUpdateChain(const UpdateNode& updatedNode, const VectorUpdateInfo* info) {
    auto current = updatedNode.info.get();
    while (current) {
        if (current == info) {
            return true;
        }
        current = current->getPrev();
    }
    return false;
}
#endif

void UpdateInfo::commit(idx_t vectorIdx, VectorUpdateInfo* info, transaction_t commitTS) {
    auto& updateNode = getUpdateNode(vectorIdx);
    std::unique_lock chainLock{updateNode.mtx};
    DASSERT(validateUpdateChain(updateNode, info));
    info->version = commitTS;
}

void UpdateInfo::rollback(idx_t vectorIdx, transaction_t version) {
    UpdateNode* header = nullptr;
    // Note that we lock the entire UpdateInfo structure here because we might modify the
    // head of the version chain. This is just a simplification and should be optimized later.
    {
        std::unique_lock lock{mtx};
        DASSERT(updates.size() > vectorIdx);
        header = updates[vectorIdx].get();
    }
    DASSERT(header);
    std::unique_lock chainLock{header->mtx};
    // First check if this version is still in the chain. It might have been removed by
    // a previous rollback entry of the same transaction.
    // TODO(Guodong): This will be optimized by moving VectorUpdateInfo into UndoBuffer.
    auto current = header->info.get();
    while (current) {
        if (current->version == version) {
            auto prevVersion = current->movePrev();
            if (current->next) {
                // Has newer version. Remove this from the version chain.
                const auto newerVersion = current->next;
                if (prevVersion) {
                    prevVersion->next = newerVersion;
                }
                newerVersion->setPrev(std::move(prevVersion));
            } else {
                DASSERT(header->info.get() == current);
                // This is the beginning of the version chain.
                if (prevVersion) {
                    prevVersion->next = nullptr;
                }
                header->info = std::move(prevVersion);
            }
            break;
        }
        current = current->getPrev();
    }
}

row_idx_t UpdateInfo::getNumUpdatedRows(const Transaction* transaction) const {
    std::unordered_set<row_idx_t> updatedRows;
    for (auto vectorIdx = 0u; vectorIdx < updates.size(); vectorIdx++) {
        iterateVectorInfo(transaction, vectorIdx, [&](const VectorUpdateInfo& info) {
            for (auto i = 0u; i < info.numRowsUpdated; i++) {
                updatedRows.insert(info.rowsInVector[i]);
            }
        });
    }
    return updatedRows.size();
}

bool UpdateInfo::hasUpdates(const Transaction* transaction, row_idx_t startRow,
    length_t numRows) const {
    bool hasUpdates = false;
    iterateScan(transaction, startRow, numRows, 0 /* startPosInOutput */,
        [&](const VectorUpdateInfo&, uint64_t, uint64_t) -> void { hasUpdates = true; });
    return hasUpdates;
}

UpdateNode& UpdateInfo::getUpdateNode(idx_t vectorIdx) {
    std::shared_lock lock{mtx};
    if (vectorIdx >= updates.size()) {
        throw InternalException(
            "UpdateInfo does not have update node for vector index: " + std::to_string(vectorIdx));
    }
    return *updates[vectorIdx];
}

UpdateNode& UpdateInfo::getOrCreateUpdateNode(idx_t vectorIdx) {
    std::unique_lock lock{mtx};
    if (vectorIdx >= updates.size()) {
        updates.resize(vectorIdx + 1);
        for (auto i = 0u; i < updates.size(); i++) {
            if (!updates[i]) {
                updates[i] = std::make_unique<UpdateNode>();
            }
        }
    }
    return *updates[vectorIdx];
}

void UpdateInfo::iterateScan(const Transaction* transaction, uint64_t startOffsetToScan,
    uint64_t numRowsToScan, uint64_t startPosInOutput,
    const iterate_read_from_row_func_t& readFromRowFunc) const {
    if (!isSet()) {
        return;
    }
    auto [startVectorIdx, startOffsetInVector] =
        StorageUtils::getQuotientRemainder(startOffsetToScan, DEFAULT_VECTOR_CAPACITY);
    auto [endVectorIdx, endOffsetInVector] = StorageUtils::getQuotientRemainder(
        startOffsetToScan + numRowsToScan, DEFAULT_VECTOR_CAPACITY);
    idx_t idx = startVectorIdx;
    sel_t posInVector = startPosInOutput;
    while (idx <= endVectorIdx) {
        const auto startOffsetInclusively = idx == startVectorIdx ? startOffsetInVector : 0;
        const auto endOffsetExclusively =
            idx == endVectorIdx ? endOffsetInVector : DEFAULT_VECTOR_CAPACITY;
        const auto numRowsInVector = endOffsetExclusively - startOffsetInclusively;
        // We keep track of the rows that have been applied with updates from updateInfo. The update
        // version chain is maintained with the newest version at the head and the oldest version at
        // the tail. For each tuple, we iterate through the chain to merge the updates from latest
        // visible version. If a row has been updated in the current vectorInfo, we should skip it
        // in older versions.
        std::bitset<DEFAULT_VECTOR_CAPACITY> rowsUpdated;
        iterateVectorInfo(transaction, idx, [&](const VectorUpdateInfo& vecUpdateInfo) -> void {
            if (vecUpdateInfo.numRowsUpdated == 0) {
                return;
            }
            if (rowsUpdated.count() == numRowsInVector) {
                // All rows in this vector have been updated with a newer visible version already.
                return;
            }
            // TODO(Guodong): Ideally we should make sure vecUpdateInfo.rowsInVector is sorted to
            // simplify the checks here.
            for (auto i = 0u; i < vecUpdateInfo.numRowsUpdated; i++) {
                if (vecUpdateInfo.rowsInVector[i] < startOffsetInclusively ||
                    vecUpdateInfo.rowsInVector[i] >= endOffsetExclusively) {
                    // Continue if the row is out of the current scan range.
                    continue;
                }
                auto updatedRowIdx = vecUpdateInfo.rowsInVector[i] - startOffsetInclusively;
                if (rowsUpdated[updatedRowIdx]) {
                    // Skip the rows that have been updated with a newer visible version already.
                    continue;
                }
                readFromRowFunc(vecUpdateInfo, i, posInVector + updatedRowIdx);
                rowsUpdated[updatedRowIdx] = true;
            }
        });
        posInVector += numRowsInVector;
        idx++;
    }
}

std::vector<VectorRetentionPlan> UpdateInfo::planRetention(transaction_t horizon) const {
    std::vector<VectorRetentionPlan> plans;
    std::shared_lock lock{mtx};
    for (auto vectorIdx = 0u; vectorIdx < updates.size(); vectorIdx++) {
        const auto& node = updates[vectorIdx];
        if (!node) {
            continue;
        }
        VectorRetentionPlan plan;
        plan.vectorIdx = vectorIdx;
        std::shared_lock chainLock{node->mtx};
        for (auto* current = node->info.get(); current != nullptr; current = current->getPrev()) {
            if (current->version >= horizon) {
                plan.kept.push_back(current);
                for (auto i = 0u; i < current->numRowsUpdated; i++) {
                    plan.keptRows.set(current->rowsInVector[i]);
                }
            } else {
                if (plan.newestBelow == nullptr) {
                    plan.newestBelow = current;
                }
                for (auto i = 0u; i < current->numRowsUpdated; i++) {
                    plan.belowRows.set(current->rowsInVector[i]);
                }
            }
        }
        if (plan.hasKept() || plan.newestBelow != nullptr) {
            plans.push_back(std::move(plan));
        }
    }
    return plans;
}

bool UpdateInfo::plansHaveKeptNodes(const std::vector<VectorRetentionPlan>& plans) {
    for (const auto& plan : plans) {
        if (plan.hasKept()) {
            return true;
        }
    }
    return false;
}

void UpdateInfo::dropUnretainedNodes(const std::vector<VectorRetentionPlan>& plans,
    bool exclusive) {
    std::unique_lock lock{mtx};
    for (const auto& plan : plans) {
        if (plan.vectorIdx >= updates.size() || !updates[plan.vectorIdx]) {
            continue;
        }
        auto& node = updates[plan.vectorIdx];
        std::unordered_set<VectorUpdateInfo*> keepSet(plan.kept.begin(), plan.kept.end());
        if (plan.hasKept() && plan.newestBelow != nullptr) {
            keepSet.insert(plan.newestBelow);
        }
        // Dismantle newest-first, then relink the survivors preserving chain order.
        std::unique_lock chainLock{node->mtx};
        std::vector<std::unique_ptr<VectorUpdateInfo>> survivors;
        auto head = std::move(node->info);
        while (head) {
            auto prev = head->movePrev();
            if (keepSet.contains(head.get())) {
                survivors.push_back(std::move(head));
            }
            head = std::move(prev);
        }
        // survivors[] is newest-first; relink head=newest with prev/next pairing
        // (chainLock from above is still held).
        if (survivors.empty()) {
            node->info = nullptr;
            continue;
        }
        node->info = std::move(survivors[0]);
        auto* cursor = node->info.get();
        cursor->setNext(nullptr);
        for (auto i = 1u; i < survivors.size(); i++) {
            cursor->setPrev(std::move(survivors[i]));
            cursor->getPrev()->setNext(cursor);
            cursor = cursor->getPrev();
        }
        cursor->setPrev(nullptr);
    }
    if (exclusive) {
        // Chains are found by vector index, so only empty slots at the end can go.
        // Empty means info == nullptr; UpdateNode::isEmpty() returns the opposite.
        while (!updates.empty() && (!updates.back() || updates.back()->info == nullptr)) {
            updates.pop_back();
        }
    }
}

namespace {
// Relinks dismantled chain nodes (newest-first) as head=front with prev/next pairing.
void relinkChainNewestFirst(UpdateNode& header,
    std::vector<std::unique_ptr<VectorUpdateInfo>>& nodes) {
    if (nodes.empty()) {
        header.info = nullptr;
        return;
    }
    header.info = std::move(nodes[0]);
    auto* cursor = header.info.get();
    cursor->setNext(nullptr);
    for (auto i = 1u; i < nodes.size(); i++) {
        cursor->setPrev(std::move(nodes[i]));
        cursor->getPrev()->setNext(cursor);
        cursor = cursor->getPrev();
    }
    cursor->setPrev(nullptr);
}
} // namespace

VectorUpdateInfo& UpdateInfo::insertRetainedBaseNode(MemoryManager& memoryManager,
    LogicalType dataType, idx_t vectorIdx, transaction_t horizon) {
    auto& header = getOrCreateUpdateNode(vectorIdx);
    std::unique_lock chainLock{header.mtx};
    // Dismantle newest-first.
    std::vector<std::unique_ptr<VectorUpdateInfo>> nodes;
    auto head = std::move(header.info);
    while (head) {
        auto prev = head->movePrev();
        nodes.push_back(std::move(head));
        head = std::move(prev);
    }
    for (const auto& node : nodes) {
        if (node->version == horizon) {
            relinkChainNewestFirst(header, nodes);
            auto* cursor = header.info.get();
            while (cursor->version != horizon) {
                cursor = cursor->getPrev();
            }
            return *cursor;
        }
    }
    // Splice the new node directly above the newest node older than the horizon (chain
    // order is newest-first, so everything at/after the horizon precedes it).
    auto inserted = std::make_unique<VectorUpdateInfo>(memoryManager, horizon, std::move(dataType));
    auto* result = inserted.get();
    std::vector<std::unique_ptr<VectorUpdateInfo>> relinked;
    bool placed = false;
    for (auto& node : nodes) {
        if (!placed && node->version < horizon) {
            relinked.push_back(std::move(inserted));
            placed = true;
        }
        relinked.push_back(std::move(node));
    }
    if (!placed) {
        relinked.push_back(std::move(inserted));
    }
    relinkChainNewestFirst(header, relinked);
    return *result;
}

bool UpdateInfo::stageRetainedRow(MemoryManager& memoryManager, LogicalType dataType,
    idx_t vectorIdx, transaction_t version, sel_t rowInVector, const ColumnChunkData& srcData,
    row_idx_t srcPos) {
    auto& header = getOrCreateUpdateNode(vectorIdx);
    std::unique_lock chainLock{header.mtx};
    VectorUpdateInfo* node = nullptr;
    for (auto* current = header.info.get(); current != nullptr; current = current->getPrev()) {
        if (current->version == version) {
            node = current;
            break;
        }
    }
    if (node == nullptr) {
        // Prepended as newest: callers stage oldest-first (below-horizon node, then the
        // horizon base, then kept nodes oldest-first), so chain order is preserved.
        auto newInfo =
            std::make_unique<VectorUpdateInfo>(memoryManager, version, std::move(dataType));
        node = newInfo.get();
        auto currentInfo = std::move(header.info);
        if (currentInfo) {
            currentInfo->setNext(node);
        }
        newInfo->setPrev(std::move(currentInfo));
        header.info = std::move(newInfo);
    }
    for (auto i = 0u; i < node->numRowsUpdated; i++) {
        if (node->rowsInVector[i] == rowInVector) {
            return false;
        }
    }
    const auto pos = node->numRowsUpdated++;
    node->rowsInVector[pos] = rowInVector;
    node->data->write(&srcData, srcPos, pos, 1);
    return true;
}

} // namespace storage
} // namespace lbug