#include "transaction/transaction_manager.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <thread>

#include "common/exception/checkpoint.h"
#include "common/exception/transaction_manager.h"
#include "common/task_system/progress_bar.h"
#include "main/attached_database.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/database_manager.h"
#include "main/db_config.h"
#include "storage/checkpointer.h"
#include "storage/partition_storage_registry.h"
#include "storage/storage_manager.h"
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
    // Only acquire the write gate for write/recovery transactions. Read-only transactions
    // can start freely during checkpoint since they use snapshot isolation.
    std::unique_lock newTransactionLck{mtxForStartingNewTransactions, std::defer_lock};
    if (type != TransactionType::READ_ONLY) {
        newTransactionLck.lock();
    }
    std::unique_lock publicFunctionLck{mtxForSerializingPublicFunctionCalls};
    const auto mustWait = [&]() {
        if (type == TransactionType::READ_ONLY) {
            return false;
        }
        if (forcedCheckpointPending) {
            return true;
        }
        return !clientContext.getDBConfig()->enableMultiWrites &&
               hasActiveWriteTransactionNoLock() &&
               activeWriteTransactionCount.load(std::memory_order_acquire) ==
                   committingWriteTransactionCount.load(std::memory_order_acquire);
    };
    while (mustWait()) {
        newTransactionLck.unlock();
        cvForCommittingWriteTransaction.wait(publicFunctionLck, [&]() { return !mustWait(); });
        // Nothing may block on the start mutex while holding the public one.
        publicFunctionLck.unlock();
        newTransactionLck.lock();
        publicFunctionLck.lock();
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

// The transaction's COMMIT record is durable, but its changes were not applied and the caller
// rolls them back: without an ABORT record recovery would apply them. If even that cannot be
// logged, refusing writes keeps a retry from applying them twice before a reopen does.
void TransactionManager::logAbortOfDurableCommit(uint64_t walBlockStart,
    const std::string& publishError) {
    try {
        wal.logAbort(walBlockStart);
    } catch (const std::exception& e) {
        // A failed sync has poisoned the WAL already; one that failed before it has not.
        wal.poison(std::format("A transaction failed after its COMMIT record was written, and "
                               "logging its abort failed: {}",
            e.what()));
        throw TransactionManagerException(std::format(
            "The transaction failed after its COMMIT record was written, and logging its abort "
            "failed, so it is applied when the database reopens; until then the database refuses "
            "further writes. Original error: {}. Abort error: {}",
            publishError, e.what()));
    }
}

void TransactionManager::commit(main::ClientContext& clientContext, Transaction* transaction) {
    if (!transaction->shouldForceCheckpoint()) {
        commitAndCheckpoint(clientContext, transaction);
        return;
    }
    waitForOtherWritersBeforeForcedCheckpoint();
    try {
        commitAndCheckpoint(clientContext, transaction);
    } catch (...) {
        endForcedCheckpoint();
        throw;
    }
    endForcedCheckpoint();
}

void TransactionManager::waitForOtherWritersBeforeForcedCheckpoint() {
    std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
    if (forcedCheckpointPending) {
        // The pending one waits for this transaction to leave, so waiting here would only stall
        // both until the timeout.
        throw TransactionManagerException(
            "Another transaction that runs COPY FROM or CREATE INDEX is committing, and this "
            "one, which also checkpoints as part of its commit, cannot commit beside it. The "
            "transaction was rolled back; run it again once the other one finishes.");
    }
    forcedCheckpointPending = true;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::microseconds(checkpointWaitTimeoutInMicros);
    // The committing transaction is itself one of the active write transactions.
    if (!cvForCommittingWriteTransaction.wait_until(lck, deadline,
            [&]() { return activeWriteTransactionCount.load(std::memory_order_acquire) == 1; })) {
        forcedCheckpointPending = false;
        cvForCommittingWriteTransaction.notify_all();
        throw TransactionManagerException(
            "Timeout waiting for other write transactions to finish before committing a "
            "transaction that runs COPY FROM or CREATE INDEX, which checkpoints as part of its "
            "commit. The transaction was rolled back; run it again once the other write "
            "transactions finish.");
    }
}

void TransactionManager::endForcedCheckpoint() {
    std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
    forcedCheckpointPending = false;
    cvForCommittingWriteTransaction.notify_all();
}

void TransactionManager::commitAndCheckpoint(main::ClientContext& clientContext,
    Transaction* transaction) {
    bool shouldForceCheckpoint = false;
    bool shouldAutoCheckpoint = false;
    bool markedAsCommitting = false;
    uint64_t walCommitSequence = 0;
    uint64_t walBlockStart = 0;
    std::optional<std::string> publishError;
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
                hasCommittedWriteTransaction_.store(true, std::memory_order_release);
                committingWriteTransactionCount.fetch_add(1, std::memory_order_release);
                markedAsCommitting = true;
                lck.unlock();
                transaction->writeCommitToWAL(&wal, walCommitSequence, walBlockStart);
                lck.lock();
                if (walCommitSequence != 0) {
                    cvForPublishingCommit.wait(lck,
                        [&]() { return walCommitSequence == nextWALCommitSequenceToPublish; });
                }
                lastTimestamp.fetch_add(1, std::memory_order_acq_rel);
                transaction->commitTS = lastTimestamp.load(std::memory_order_acquire);
                try {
                    transaction->publishCommit();
                } catch (const std::exception& e) {
                    publishError = e.what();
                    throw;
                } catch (...) {
                    publishError = "unknown exception";
                    throw;
                }
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
        // activeWriteTransactionCount still counts this transaction until the caller rolls it back,
        // so no checkpoint can rotate the WAL between its COMMIT and this ABORT. A failed WAL sync
        // logs no ABORT: it has poisoned the WAL, and a reopen applies the COMMIT if it is durable.
        if (publishError && walCommitSequence != 0) {
            logAbortOfDurableCommit(walBlockStart, *publishError);
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
            if (wal.isPoisoned()) {
                // No later CHECKPOINT can run before the restart, whose recovery applies this one
                // only if its record reached storage.
                throw CheckpointException{std::format(
                    "Transaction committed successfully, but the checkpoint it requires failed. "
                    "The database refuses further writes until restart, and the changes survive "
                    "it only if the checkpoint's record reached storage: {}",
                    e.what())};
            }
            throw CheckpointException{std::format(
                "Transaction committed successfully, but the checkpoint it requires failed. Its "
                "changes are not in the WAL and may be lost on restart unless a later CHECKPOINT "
                "succeeds; run CHECKPOINT rather than the statement again: {}",
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
    if (clientContext.isInMemory()) {
        // In-memory databases never checkpoint, so version chains would grow without
        // bound (see #1566). Fold superseded versions into the bases now that the
        // committing transaction is gone; live readers (if any) keep their horizon.
        // Best-effort: a vacuum failure must not fail the already-committed statement.
        try {
            std::unique_lock checkpointLck{mtxForCheckpoint, std::try_to_lock};
            if (checkpointLck.owns_lock()) {
                vacuumInMemoryUpdates(clientContext);
            }
        } catch (const std::exception& e) {
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
        cvForCommittingWriteTransaction.notify_all();
    } break;
    default: {
        throw TransactionManagerException("Invalid transaction type to rollback.");
    }
    }
}

void TransactionManager::vacuumInMemoryUpdates(main::ClientContext& clientContext) {
    // Oldest active startTS, or INVALID_TRANSACTION when no transaction is open. Only
    // committed versions below it may be folded; newer versions stay for live readers.
    const auto horizon = snapshotHorizon();
    // Main storage manager plus any graph and partition-child managers (mirrors
    // Checkpointer::collectCheckpointTargets plus per-partition files).
    std::vector<StorageManager*> managers;
    managers.push_back(clientContext.getDatabase()->getStorageManager());
    if (auto* dbManager = main::DatabaseManager::Get(clientContext)) {
        for (auto* graphCatalog : dbManager->getGraphs()) {
            if (auto* sm = graphCatalog->getStorageManager()) {
                if (sm != managers.front()) {
                    managers.push_back(sm);
                }
            }
        }
        for (auto* sm : dbManager->getPartitionStorageRegistry()->getAllManagers()) {
            managers.push_back(sm);
        }
    }
    for (auto* sm : managers) {
        sm->vacuumInMemoryUpdates(horizon);
    }
}

void TransactionManager::checkpoint(main::ClientContext& clientContext) {
    if (clientContext.isInMemory()) {
        // In-memory databases have no WAL/shadow files to checkpoint, but explicit
        // CHECKPOINT should still reclaim superseded MVCC versions.
        std::unique_lock checkpointLck{mtxForCheckpoint};
        vacuumInMemoryUpdates(clientContext);
        return;
    }
    // Use the dedicated checkpoint mutex so active writers can still commit/rollback
    // during the drain phase.
    std::unique_lock checkpointLck{mtxForCheckpoint};
    checkpointNoLock(clientContext);
}

void TransactionManager::addLocalWriter(const std::string& ownerCatalogName,
    common::table_id_t tableID, const storage::LocalStorage* writer,
    const std::function<void()>& checkTableUnchanged) {
    std::lock_guard lck{localWritersMtx};
    checkTableUnchanged();
    localWriters[{ownerCatalogName, tableID}].insert(writer);
}

void TransactionManager::removeLocalWriter(const std::string& ownerCatalogName,
    common::table_id_t tableID, const storage::LocalStorage* writer) {
    std::lock_guard lck{localWritersMtx};
    const auto it = localWriters.find({ownerCatalogName, tableID});
    if (it == localWriters.end()) {
        return;
    }
    it->second.erase(writer);
    if (it->second.empty()) {
        localWriters.erase(it);
    }
}

bool TransactionManager::hasLocalWriterOtherThan(const std::string& ownerCatalogName,
    common::table_id_t tableID, const storage::LocalStorage* writer) {
    std::lock_guard lck{localWritersMtx};
    const auto it = localWriters.find({ownerCatalogName, tableID});
    return it != localWriters.end() &&
           std::ranges::any_of(it->second, [&](const auto* other) { return other != writer; });
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
        std::unique_lock checkpointLck{mtxForCheckpoint, std::try_to_lock};
        if (!checkpointLck.owns_lock()) {
            return;
        }
        vacuumInMemoryUpdates(clientContext);
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
        // A reader that begins during the storage phase must not lose its snapshot. Isolate
        // newcomers by bumping lastTimestamp: the checkpoint snapshot is the pre-bump value, and a
        // transaction that reads the bumped value observes the collapsed post-checkpoint base, so
        // it needs no retention. One that read it before the bump keeps startTS <=
        // snapshotTimestamp and must be covered by retention.
        transaction_t snapshotTimestamp = lastTimestamp.load(std::memory_order_acquire);
        lastTimestamp.fetch_add(1, std::memory_order_acq_rel);
        transaction_t retentionHorizon = INVALID_TRANSACTION;
        transaction_t pinningTxnID = INVALID_TRANSACTION;
        std::vector<transaction_t> openStartTSs;
        {
            std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
            openStartTSs.reserve(openTxnStartTS.size());
            for (const auto& entry : openTxnStartTS) {
                if (entry.second > snapshotTimestamp) {
                    continue;
                }
                openStartTSs.push_back(entry.second);
                if (retentionHorizon == INVALID_TRANSACTION || entry.second < retentionHorizon) {
                    retentionHorizon = entry.second;
                    pinningTxnID = entry.first;
                }
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
    hasCommittedWriteTransaction_.store(false, std::memory_order_release);
}

} // namespace transaction
} // namespace lbug
