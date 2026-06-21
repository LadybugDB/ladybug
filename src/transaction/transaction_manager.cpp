#include "transaction/transaction_manager.h"

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
        return activeTransactions.back().get();
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
        return activeTransactions.back().get();
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
    // (commit/rollback) during the drain phase instead of deadlocking.
    if (shouldForceCheckpoint) {
        checkpoint(clientContext);
    } else if (shouldAutoCheckpoint) {
        tryCheckpoint(clientContext);
    }
}

// Note: We take in additional `transaction` here is due to that `transactionContext` might be
// destructed when a transaction throws an exception, while we need to roll back the active
// transaction still.
void TransactionManager::rollback(main::ClientContext& clientContext, Transaction* transaction) {
    std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
    clientContext.cleanUp();
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
    // We only need to wait for active write transactions to leave the system before
    // checkpointing. Read transactions can continue safely because they use MVCC snapshot
    // isolation and shadow pages are applied with per-page locking.
    UniqLock writeGate;
    try {
        writeGate = stopNewWriteTransactionsAndWaitUntilAllWriteTransactionsLeave();
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
        transaction_t snapshotTimestamp = lastTimestamp.load(std::memory_order_acquire);
        checkpointer->beginCheckpoint(snapshotTimestamp);
        progress.update(0.15);
    } catch (std::exception& e) {
        checkpointer->rollback();
        throw CheckpointException{e};
    }
    // Release the write gate early when WAL was rotated. New writers create a fresh active WAL
    // isolated from the frozen checkpoint WAL, so node-data reads during checkpointStoragePhase
    // remain bounded to snapshotTS.
    // NOTE: HashIndexLocalStorage has no per-entry timestamps, so post-snapshotTS inserts that
    // arrive after the gate is released may appear in the on-disk hash index while the
    // corresponding node data was not included in this checkpoint.  This is a pre-existing
    // limitation of the Vela design; fixing it requires adding timestamp-aware snapshotting
    // to HashIndexLocalStorage (tracked as a follow-up).
    if (checkpointer->wasWalRotated()) {
        writeGate = {};
    }
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
