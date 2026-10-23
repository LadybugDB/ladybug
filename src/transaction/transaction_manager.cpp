#include "transaction/transaction_manager.h"

#include <algorithm>
#include <iostream>
#include <thread>

#include "common/exception/checkpoint.h"
#include "common/exception/transaction_manager.h"
#include "common/task_system/progress_bar.h"
#include "main/attached_database.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/db_config.h"
#include "storage/checkpointer.h"
#include "storage/wal/local_wal.h"
#include "storage/wal/wal.h"
#include <format>

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace transaction {
namespace {

class QueryProgressScope {
public:
    QueryProgressScope(main::ClientContext& clientContext, double initialProgress) {
        queryID = clientContext.getActiveQueryID();
        if (!queryID.has_value()) {
            return;
        }
        progressBar = ProgressBar::Get(clientContext);
        progressBar->addPipeline();
        progressBar->startProgress(queryID.value());
        update(initialProgress);
    }

    ~QueryProgressScope() {
        if (progressBar != nullptr) {
            progressBar->endProgress(queryID.value());
        }
    }

    void update(double progress) const {
        if (progressBar != nullptr) {
            progressBar->updateProgress(queryID.value(), progress);
        }
    }

private:
    ProgressBar* progressBar = nullptr;
    std::optional<uint64_t> queryID;
};

void warnAutoCheckpointFailed(WAL& wal, const std::exception& e) {
    std::string walSize = "unknown";
    try {
        walSize = std::to_string(wal.getFileSize());
    } catch (...) { // NOLINT: The statement has committed, so reporting must not fail it.
    }
    std::cerr << "Warning: automatic checkpoint after a commit failed, WAL " << walSize
              << " bytes: " << e.what() << std::endl;
}

} // namespace

Transaction* TransactionManager::beginTransaction(main::ClientContext& clientContext,
    TransactionType type) {
    std::unique_lock publicFunctionLck{mtxForSerializingPublicFunctionCalls};
    // Only acquire the write gate for write/recovery transactions. Read-only transactions
    // can start freely during checkpoint since they use snapshot isolation.
    std::unique_lock newTransactionLck{mtxForStartingNewTransactions, std::defer_lock};
    if (type != TransactionType::READ_ONLY) {
        newTransactionLck.lock();
    }
    while (type != TransactionType::READ_ONLY && !clientContext.getDBConfig()->enableMultiWrites &&
           hasActiveWriteTransactionNoLock() &&
           activeWriteTransactionCount.load(std::memory_order_acquire) ==
               committingWriteTransactionCount.load(std::memory_order_acquire)) {
        newTransactionLck.unlock();
        cvForCommittingWriteTransaction.wait(publicFunctionLck, [&]() {
            return !hasActiveWriteTransactionNoLock() ||
                   activeWriteTransactionCount.load(std::memory_order_acquire) !=
                       committingWriteTransactionCount.load(std::memory_order_acquire);
        });
        newTransactionLck.lock();
    }
    switch (type) {
    case TransactionType::READ_ONLY: {
        auto transaction = std::make_unique<Transaction>(clientContext, type, ++lastTransactionID,
            lastTimestamp.load(std::memory_order_acquire));
        activeTransactions.push_back(std::move(transaction));
        auto* result = activeTransactions.back().get();
        registerTransactionNoLock(result->getID(), result->getStartTS());
        return result;
    }
    case TransactionType::RECOVERY:
    case TransactionType::WRITE: {
        wal.throwIfPoisoned();
        if (!clientContext.getDBConfig()->enableMultiWrites && hasActiveWriteTransactionNoLock()) {
            throw TransactionManagerException(
                "Cannot start a new write transaction in the system. "
                "Only one write transaction at a time is allowed in the system.");
        }
        auto transaction = std::make_unique<Transaction>(clientContext, type, ++lastTransactionID,
            lastTimestamp.load(std::memory_order_acquire));
        activeWriteTransactionCount.fetch_add(1, std::memory_order_release);
        activeTransactions.push_back(std::move(transaction));
        auto* result = activeTransactions.back().get();
        registerTransactionNoLock(result->getID(), result->getStartTS());
        return result;
    }
        // LCOV_EXCL_START
    default: {
        throw TransactionManagerException("Invalid transaction type to begin transaction.");
    }
        // LCOV_EXCL_STOP
    }
}

void TransactionManager::commit(main::ClientContext& clientContext, Transaction* transaction) {
    bool shouldForceCheckpoint = false;
    bool shouldAutoCheckpoint = false;
    bool markedAsCommitting = false;
    uint64_t walCommitSequence = 0;
    try {
        {
            std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
            clientContext.cleanUp();
            switch (transaction->getType()) {
            case TransactionType::READ_ONLY: {
                clearTransactionNoLock(transaction->getID());
            } break;
            case TransactionType::RECOVERY:
            case TransactionType::WRITE: {
                committingWriteTransactionCount.fetch_add(1, std::memory_order_release);
                markedAsCommitting = true;
                lck.unlock();
                transaction->writeCommitToWAL(&wal, walCommitSequence);
                lck.lock();
                if (walCommitSequence != 0) {
                    cvForPublishingCommit.wait(lck,
                        [&]() { return walCommitSequence == nextWALCommitSequenceToPublish; });
                }
                lastTimestamp.fetch_add(1, std::memory_order_acq_rel);
                transaction->commitTS = lastTimestamp.load(std::memory_order_acquire);
                transaction->publishCommit();
                if (walCommitSequence != 0) {
                    nextWALCommitSequenceToPublish++;
                    cvForPublishingCommit.notify_all();
                }
                shouldForceCheckpoint = transaction->shouldForceCheckpoint();
                shouldAutoCheckpoint = Checkpointer::canAutoCheckpoint(clientContext, *transaction);
                clearTransactionNoLock(transaction->getID());
                activeWriteTransactionCount.fetch_sub(1, std::memory_order_release);
                committingWriteTransactionCount.fetch_sub(1, std::memory_order_release);
                cvForCommittingWriteTransaction.notify_all();
                markedAsCommitting = false;
            } break;
                // LCOV_EXCL_START
            default: {
                throw TransactionManagerException("Invalid transaction type to commit.");
            }
                // LCOV_EXCL_STOP
            }
        }
    } catch (...) {
        if (walCommitSequence != 0) {
            std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
            cvForPublishingCommit.wait(lck,
                [&]() { return walCommitSequence == nextWALCommitSequenceToPublish; });
            nextWALCommitSequenceToPublish++;
            cvForPublishingCommit.notify_all();
        }
        if (markedAsCommitting) {
            std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
            // Keep the transaction active so the caller's rollback path can undo any partial
            // in-memory publish work. The rollback path clears activeWriteTransactionCount.
            committingWriteTransactionCount.fetch_sub(1, std::memory_order_release);
            cvForCommittingWriteTransaction.notify_all();
        }
        throw;
    }
    // Checkpoint outside the public function lock so active writers can finish
    // (commit/rollback) during the drain phase instead of deadlocking. The transaction has
    // already been removed from activeTransactions at this point, so any later failure is a
    // checkpoint failure, not a transaction failure that can be rolled back.
    if (shouldForceCheckpoint) {
        try {
            checkpoint(clientContext);
        } catch (const std::exception& e) {
            // COPY FROM and CREATE INDEX on a large ART rely on this checkpoint instead of the WAL.
            throw CheckpointException{std::format(
                "Transaction committed successfully, but the checkpoint it requires failed. Its "
                "changes are not in the WAL and may be lost on restart unless a later CHECKPOINT "
                "succeeds: {}",
                e.what())};
        }
    } else if (shouldAutoCheckpoint) {
        try {
            tryCheckpoint(clientContext);
        } catch (const std::exception& e) {
            // The commit is in the WAL whatever the checkpoint did, so failing the statement would
            // only invite a retry that applies it twice. A rolled-back checkpoint is retried by a
            // later write commit; a poisoned WAL fails the next write instead.
            warnAutoCheckpointFailed(wal, e);
        }
    }
}

void TransactionManager::rollback(main::ClientContext& clientContext, transaction_t transactionID) {
    std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
    clientContext.cleanUp();
    const auto transactionIt =
        std::ranges::find_if(activeTransactions, [transactionID](const auto& activeTransaction) {
            return activeTransaction->getID() == transactionID;
        });
    if (transactionIt == activeTransactions.end()) {
        return;
    }
    auto* transaction = transactionIt->get();
    switch (transaction->getType()) {
    case TransactionType::READ_ONLY: {
        clearTransactionNoLock(transaction->getID());
    } break;
    case TransactionType::RECOVERY:
    case TransactionType::WRITE: {
        transaction->rollback(&wal);
        clearTransactionNoLock(transaction->getID());
        activeWriteTransactionCount.fetch_sub(1, std::memory_order_release);
    } break;
    default: {
        throw TransactionManagerException("Invalid transaction type to rollback.");
    }
    }
}

void TransactionManager::checkpoint(main::ClientContext& clientContext) {
    if (clientContext.isInMemory()) {
        return;
    }
    // Use the dedicated checkpoint mutex so active writers can still commit/rollback
    // during the drain phase.
    std::unique_lock checkpointLck{mtxForCheckpoint};
    checkpointNoLock(clientContext);
}

TransactionManager* TransactionManager::Get(const main::ClientContext& context) {
    if (context.getAttachedDatabase() != nullptr) {
        context.getAttachedDatabase()->getTransactionManager();
    }
    return context.getDatabase()->getTransactionManager();
}

UniqLock TransactionManager::stopNewTransactionsAndWaitUntilAllTransactionsLeave() {
    UniqLock startTransactionLock{mtxForStartingNewTransactions};
    uint64_t numTimesWaited = 0;
    while (true) {
        if (hasNoActiveTransactions()) {
            break;
        }
        numTimesWaited++;
        if (numTimesWaited * THREAD_SLEEP_TIME_WHEN_WAITING_IN_MICROS >
            checkpointWaitTimeoutInMicros) {
            throw TransactionManagerException(
                "Timeout waiting for active transactions to leave the system before "
                "checkpointing. If you have an open transaction, please close it and try "
                "again.");
        }
        std::this_thread::sleep_for(
            std::chrono::microseconds(THREAD_SLEEP_TIME_WHEN_WAITING_IN_MICROS));
    }
    return startTransactionLock;
}

UniqLock TransactionManager::stopNewWriteTransactionsAndWaitUntilAllWriteTransactionsLeave() {
    UniqLock startTransactionLock{mtxForStartingNewTransactions};
    uint64_t numTimesWaited = 0;
    while (true) {
        if (!hasActiveWriteTransactionNoLock()) {
            break;
        }
        numTimesWaited++;
        if (numTimesWaited * THREAD_SLEEP_TIME_WHEN_WAITING_IN_MICROS >
            checkpointWaitTimeoutInMicros) {
            throw TransactionManagerException(
                "Timeout waiting for active write transactions to leave the system before "
                "checkpointing. If you have an open write transaction, please close it and "
                "try again.");
        }
        std::this_thread::sleep_for(
            std::chrono::microseconds(THREAD_SLEEP_TIME_WHEN_WAITING_IN_MICROS));
    }
    return startTransactionLock;
}

bool TransactionManager::hasNoActiveTransactions() const {
    return activeTransactions.empty();
}

void TransactionManager::clearTransactionNoLock(transaction_t transactionID) {
    DASSERT(std::ranges::any_of(activeTransactions.begin(), activeTransactions.end(),
        [transactionID](const auto& activeTransaction) {
            return activeTransaction->getID() == transactionID;
        }));
    std::erase_if(activeTransactions, [transactionID](const auto& activeTransaction) {
        return activeTransaction->getID() == transactionID;
    });
    openTxnStartTS.erase(transactionID);
    recomputeHorizonNoLock();
}

void TransactionManager::registerTransactionNoLock(transaction_t transactionID,
    transaction_t startTS) {
    openTxnStartTS[transactionID] = startTS;
    if (cachedHorizon == INVALID_TRANSACTION || startTS < cachedHorizon) {
        cachedHorizon = startTS;
    }
}

void TransactionManager::recomputeHorizonNoLock() {
    cachedHorizon = INVALID_TRANSACTION;
    for (const auto& entry : openTxnStartTS) {
        const auto startTS = entry.second;
        if (cachedHorizon == INVALID_TRANSACTION || startTS < cachedHorizon) {
            cachedHorizon = startTS;
        }
    }
}

transaction_t TransactionManager::snapshotHorizon() {
    std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
    return cachedHorizon;
}

TransactionManager::HorizonPinInfo TransactionManager::snapshotHorizonInfo() {
    std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
    HorizonPinInfo pin;
    pin.openTxnCount = openTxnStartTS.size();
    for (const auto& entry : openTxnStartTS) {
        if (pin.horizon == INVALID_TRANSACTION || entry.second < pin.horizon) {
            pin.horizon = entry.second;
            pin.pinningTxnID = entry.first;
        }
    }
    return pin;
}

std::vector<transaction_t> TransactionManager::snapshotOpenStartTSs() {
    std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
    std::vector<transaction_t> startTSs;
    startTSs.reserve(openTxnStartTS.size());
    for (const auto& entry : openTxnStartTS) {
        startTSs.push_back(entry.second);
    }
    return startTSs;
}

std::unique_ptr<Checkpointer> TransactionManager::initCheckpointer(
    main::ClientContext& clientContext) {
    return std::make_unique<Checkpointer>(clientContext);
}

void TransactionManager::tryCheckpoint(main::ClientContext& clientContext) {
    if (clientContext.isInMemory()) {
        return;
    }
    std::unique_lock checkpointLck{mtxForCheckpoint, std::try_to_lock};
    if (!checkpointLck.owns_lock()) {
        return;
    }
    checkpointNoLock(clientContext);
}

void TransactionManager::checkpointNoLock(main::ClientContext& clientContext) {
    QueryProgressScope progress{clientContext, 0.01};
    // Capture the retention horizon before acquiring the write gate. Taking the
    // public-function lock here is safe because the gate is not held yet; the value is
    // threaded through the checkpointer so no later stage re-acquires that lock under
    // the gate.
    const auto preHorizonPin = snapshotHorizonInfo();
    auto retentionHorizon = preHorizonPin.horizon;
    auto openStartTSs = snapshotOpenStartTSs();
    auto pinningTxnID = preHorizonPin.pinningTxnID;
    // We only need to wait for active write transactions to leave the system before
    // checkpointing. Read transactions can continue because they use MVCC snapshot isolation.
    // Column chunk metadata of in-place checkpointed data is published before the shadow pages
    // are applied, so column reads resolve such in-flight pages through the shadow file (see
    // ColumnReadWriter::readFromPage); applying shadow pages uses per-page locking.
    UniqLock writeGate;
    try {
        writeGate = stopNewWriteTransactionsAndWaitUntilAllWriteTransactionsLeave();
    } catch (std::exception& e) {
        throw CheckpointException{e};
    }
    try {
        clientContext.getDatabase()->getStorageManager()->getWAL().throwIfPoisoned();
    } catch (std::exception& e) {
        throw CheckpointException{e};
    }
    auto checkpointer = initCheckpointerFunc(clientContext);
    try {
        // lastTimestamp is atomic, so we can snapshot it without taking
        // mtxForSerializingPublicFunctionCalls. Grabbing that mutex here would invert the
        // lock order against beginTransaction() (which takes public -> start) while the
        // checkpoint holds the write gate (start), deadlocking concurrent writers during an
        // auto-checkpoint triggered from commit().
        //
        // A reader that begins between the pre-gate capture above and the write gate (or
        // during the storage phase) would be missing from the pre-gate horizon and lose its
        // snapshot. Isolate newcomers by bumping lastTimestamp: the checkpoint snapshot is
        // the pre-bump value, and every transaction that begins afterwards observes the
        // collapsed post-checkpoint base with a larger start timestamp, so it needs no
        // retention. Readers that began before the bump keep startTS <= snapshotTimestamp
        // and must be covered by retention below.
        transaction_t snapshotTimestamp = lastTimestamp.load(std::memory_order_acquire);
        lastTimestamp.fetch_add(1, std::memory_order_acq_rel);
        // Re-sample the registry to include gap readers without deadlocking: a waiting
        // writer holds the public lock while blocked on the gate, so only try_lock here.
        // Keep only start timestamps at or below the checkpoint snapshot; anything larger
        // began after the bump and observes the post-checkpoint base.
        {
            std::unique_lock tryLck{mtxForSerializingPublicFunctionCalls, std::try_to_lock};
            if (tryLck.owns_lock()) {
                transaction_t postHorizon = INVALID_TRANSACTION;
                transaction_t postPin = INVALID_TRANSACTION;
                std::vector<transaction_t> postOpen;
                postOpen.reserve(openTxnStartTS.size());
                for (const auto& entry : openTxnStartTS) {
                    if (entry.second > snapshotTimestamp) {
                        continue;
                    }
                    postOpen.push_back(entry.second);
                    if (postHorizon == INVALID_TRANSACTION || entry.second < postHorizon) {
                        postHorizon = entry.second;
                        postPin = entry.first;
                    }
                }
                retentionHorizon = postHorizon;
                openStartTSs = std::move(postOpen);
                pinningTxnID = postPin;
            } else if (retentionHorizon == INVALID_TRANSACTION) {
                // A writer is waiting on the gate and holds the public lock. Fall back to a
                // conservative horizon: any gap reader holds startTS == snapshotTimestamp.
                // This over-retains only when a writer races the checkpoint with no actual
                // gap reader, which is rare and still correct.
                retentionHorizon = snapshotTimestamp;
                openStartTSs.push_back(snapshotTimestamp);
            } else if (std::ranges::find(openStartTSs, snapshotTimestamp) == openStartTSs.end()) {
                // Pre-gate horizon already covers gap readers (their startTS >= horizon),
                // but CSR coverage needs their timestamp present to take a snapshot.
                openStartTSs.push_back(snapshotTimestamp);
            }
        }
        checkpointer->beginCheckpoint(snapshotTimestamp, retentionHorizon, openStartTSs,
            pinningTxnID);
        progress.update(0.15);
    } catch (std::exception& e) {
        checkpointer->rollback();
        throw CheckpointException{e};
    }
    // NOTE: The write gate must be held until the checkpoint completes. Releasing it early
    // (e.g. after WAL rotation) allows new write transactions to update persistent chunks'
    // version/update info while checkpointStoragePhase is concurrently scanning and resetting
    // those structures, which corrupts the version chains and crashes committing writers.
    try {
        checkpointer->checkpointStoragePhase();
        progress.update(0.75);
    } catch (std::exception& e) {
        checkpointer->rollback();
        throw CheckpointException{e};
    }
    try {
        checkpointer->finishCheckpoint();
        progress.update(0.95);
    } catch (std::exception& e) {
        auto* wal = &clientContext.getDatabase()->getStorageManager()->getWAL();
        if (wal->mayHaveCheckpointRecord()) {
            checkpointer->releaseCheckpointLocks();
            wal->poison(std::format(
                "Checkpoint failed after its commit record may have reached storage: {}",
                e.what()));
            throw CheckpointException(std::format(
                "Checkpoint failed after its commit record may have reached storage; the database "
                "refuses further writes until restart. Original error: {}",
                e.what()));
        }
        checkpointer->rollback();
        throw CheckpointException{e};
    }
    bool canResetPageManagerToCurrent = true;
    if (!writeGate.isLocked()) {
        try {
            writeGate = stopNewWriteTransactionsAndWaitUntilAllWriteTransactionsLeave();
        } catch (std::exception&) {
            canResetPageManagerToCurrent = false;
        }
    }
    checkpointer->postCheckpointCleanup(canResetPageManagerToCurrent);
    progress.update(1.0);
    writeGate = {};
}

} // namespace transaction
} // namespace lbug
