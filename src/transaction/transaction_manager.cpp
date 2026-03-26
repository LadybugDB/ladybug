#include "transaction/transaction_manager.h"

#include <thread>

#include "common/exception/checkpoint.h"
#include "common/exception/transaction_manager.h"
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

Transaction* TransactionManager::beginTransaction(main::ClientContext& clientContext,
    TransactionType type) {
    std::unique_lock publicFunctionLck{mtxForSerializingPublicFunctionCalls};
    // Only acquire the write gate for write/recovery transactions. Read-only transactions
    // can start freely during checkpoint since they use snapshot isolation.
    std::unique_lock newTransactionLck{mtxForStartingNewTransactions, std::defer_lock};
    if (type != TransactionType::READ_ONLY) {
        newTransactionLck.lock();
    }
    switch (type) {
    case TransactionType::READ_ONLY: {
        auto transaction =
            std::make_unique<Transaction>(clientContext, type, ++lastTransactionID, lastTimestamp);
        activeTransactions.push_back(std::move(transaction));
        return activeTransactions.back().get();
    }
    case TransactionType::RECOVERY:
    case TransactionType::WRITE: {
        if (!clientContext.getDBConfig()->enableMultiWrites && hasActiveWriteTransactionNoLock()) {
            throw TransactionManagerException(
                "Cannot start a new write transaction in the system. "
                "Only one write transaction at a time is allowed in the system.");
        }
        auto transaction =
            std::make_unique<Transaction>(clientContext, type, ++lastTransactionID, lastTimestamp);
        if (transaction->shouldLogToWAL()) {
            transaction->getLocalWAL().logBeginTransaction();
        }
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
    bool shouldCheckpoint = false;
    {
        std::unique_lock lck{mtxForSerializingPublicFunctionCalls};
        clientContext.cleanUp();
        switch (transaction->getType()) {
        case TransactionType::READ_ONLY: {
            clearTransactionNoLock(transaction->getID());
        } break;
        case TransactionType::RECOVERY:
        case TransactionType::WRITE: {
            lastTimestamp++;
            transaction->commitTS = lastTimestamp;
            transaction->commit(&wal);
            shouldCheckpoint = transaction->shouldForceCheckpoint() ||
                               Checkpointer::canAutoCheckpoint(clientContext, *transaction);
            clearTransactionNoLock(transaction->getID());
            activeWriteTransactionCount.fetch_sub(1, std::memory_order_release);
        } break;
            // LCOV_EXCL_START
        default: {
            throw TransactionManagerException("Invalid transaction type to commit.");
        }
            // LCOV_EXCL_STOP
        }
    }
    // Checkpoint outside the public function lock so active writers can finish
    // (commit/rollback) during the drain phase instead of deadlocking.
    if (shouldCheckpoint) {
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
        checkpointer->beginCheckpoint(lastTimestamp);
    } catch (std::exception& e) {
        checkpointer->rollback();
        throw CheckpointException{e};
    }
    // Release the write gate early when WAL was rotated. New writers create a fresh active WAL,
    // isolated from the frozen checkpoint WAL.
    if (checkpointer->wasWalRotated()) {
        writeGate = {};
    }
    try {
        checkpointer->checkpointStoragePhase();
    } catch (std::exception& e) {
        checkpointer->rollback();
        throw CheckpointException{e};
    }
    try {
        checkpointer->finishCheckpoint();
    } catch (std::exception& e) {
        checkpointer->rollback();
        throw CheckpointException{e};
    }
    writeGate = {};
    checkpointer->postCheckpointCleanup();
}

} // namespace transaction
} // namespace lbug
