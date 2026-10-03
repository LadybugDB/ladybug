#include "storage/wal/wal.h"

#include "common/exception/runtime.h"
#include "common/file_system/file_info.h"
#include "common/file_system/virtual_file_system.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/in_mem_file_writer.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/db_config.h"
#include "storage/file_db_id_utils.h"
#include "storage/storage_manager.h"
#include "storage/storage_utils.h"
#include "storage/wal/checksum_writer.h"
#include "storage/wal/local_wal.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

WAL::FrozenWALAdoptionGuard::FrozenWALAdoptionGuard(WAL& wal) : wal{wal} {
    std::unique_lock lck{wal.mtx};
    wal.adoptFrozenWAL = true;
}

WAL::FrozenWALAdoptionGuard::~FrozenWALAdoptionGuard() {
    std::unique_lock lck{wal.mtx};
    wal.adoptFrozenWAL = false;
}

WAL::WAL(const std::string& dbPath, bool readOnly, bool enableChecksums, VirtualFileSystem* vfs,
    StorageManager* storageManager)
    : walPath{StorageUtils::getWALFilePath(dbPath)},
      checkpointWalPath{StorageUtils::getCheckpointWALFilePath(dbPath)},
      inMemory{main::DBConfig::isDBPathInMemory(dbPath)}, readOnly{readOnly}, vfs{vfs},
      storageManager{storageManager}, enableChecksums(enableChecksums) {}

WAL::~WAL() {}

void WAL::logCommittedWAL(LocalWAL& localWAL, main::ClientContext* context,
    uint64_t& commitSequence) {
    DASSERT(!readOnly);
    commitSequence = 0;
    if (inMemory || localWAL.getSize() == 0) {
        return; // No need to log empty WAL.
    }
    std::unique_lock lck{mtx};
    throwIfPoisonedNoLock();
    initWriter(context);
    localWAL.inMemWriter->flush(*serializer->getWriter());
    commitSequence = ++appendedCommitSequence;
    waitForDurabilityNoLock(commitSequence, lck);
}

void WAL::logAndFlushCheckpoint(main::ClientContext* context) {
    std::unique_lock lck{mtx};
    throwIfPoisonedNoLock();
    initWriter(context);
    CheckpointRecord walRecord{CHECKPOINT_BUNDLE_FORMAT_VERSION};
    checkpointRecordMayExist = true;
    addNewWALRecordNoLock(walRecord);
    flushAndSyncNoLock();
}

bool WAL::rotateForCheckpoint(main::ClientContext* /*context*/) {
    std::unique_lock lck{mtx};
    throwIfPoisonedNoLock();
    if (inMemory) {
        return false;
    }
    if (adoptFrozenWAL) {
        // The frozen WAL on disk becomes this checkpoint's frozen WAL. The active WAL, if any,
        // holds later records that recovery replays after this checkpoint, so it is left alone.
        // It has no CHECKPOINT record yet, so any state tracking whether the frozen WAL holds
        // one must read "no" here, as it does after a fresh rotation.
        adoptFrozenWAL = false;
        return true;
    }
    if (vfs->fileOrPathExists(checkpointWalPath)) {
        throw RuntimeException(
            "Cannot checkpoint: the frozen WAL of an earlier checkpoint is still "
            "pending. Reopen the database to recover it.");
    }
    if (!serializer && !vfs->fileOrPathExists(walPath)) {
        return false;
    }
    if (serializer) {
        flushAndSyncNoLock();
        durableCommitSequence = appendedCommitSequence;
        groupCommitCV.notify_all();
        fileInfo.reset();
        serializer.reset();
    }
    bool renamed = false;
    try {
        vfs->renameFile(walPath, checkpointWalPath);
        renamed = true;
        activeWALDirectorySynced = false;
        frozenWALHasCheckpointRecord = false;
        checkpointRecordMayExist = false;
        vfs->syncParentDirectory(checkpointWalPath);
    } catch (const std::exception& e) {
        if (!renamed) {
            try {
                renamed =
                    vfs->fileOrPathExists(checkpointWalPath) || !vfs->fileOrPathExists(walPath);
            } catch (...) {
                renamed = true;
            }
        }
        if (renamed) {
            activeWALDirectorySynced = false;
            frozenWALHasCheckpointRecord = false;
            checkpointRecordMayExist = false;
            poisonNoLock(std::string{"WAL rotation failed: "} + e.what());
            throw RuntimeException(
                "WAL rotation may have reached storage; database is in a panic state and refuses "
                "further writes until restart. Original error: " +
                std::string{e.what()});
        }
        throw;
    } catch (...) {
        activeWALDirectorySynced = false;
        frozenWALHasCheckpointRecord = false;
        checkpointRecordMayExist = false;
        poisonNoLock("WAL rotation failed: unknown exception");
        throw RuntimeException(
            "WAL rotation may have reached storage; database is in a panic state and refuses "
            "further writes until restart. Original error: unknown exception");
    }
    return true;
}

void WAL::undoRotationForCheckpoint() noexcept {
    std::unique_lock lck{mtx};
    if (inMemory || frozenWALHasCheckpointRecord || serializer) {
        return;
    }
    // The checkpoint holds the write gate from rotation until rollback, so nothing can have
    // written a new active WAL in the meantime. If the rename is not possible, keep the frozen
    // WAL: recovery replays it, and rotateForCheckpoint() refuses to overwrite it.
    try {
        if (vfs->fileOrPathExists(walPath) || !vfs->fileOrPathExists(checkpointWalPath)) {
            return;
        }
        vfs->renameFile(checkpointWalPath, walPath);
        activeWALDirectorySynced = false;
        vfs->syncParentDirectory(walPath);
    } catch (const std::exception& e) {
        poisonNoLock(
            std::string{"WAL rotation rollback failed; frozen WAL left for recovery: "} + e.what());
    } catch (...) {
        poisonNoLock(
            "WAL rotation rollback failed; frozen WAL left for recovery: unknown exception");
    }
}

void WAL::logAndFlushCheckpointToFrozen(main::ClientContext* context) {
    {
        std::unique_lock lck{mtx};
        throwIfPoisonedNoLock();
    }
    auto frozenFileInfo = vfs->openFile(checkpointWalPath,
        FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE), context);
    {
        // From here on, the CHECKPOINT record may reach the frozen WAL.
        std::unique_lock lck{mtx};
        frozenWALHasCheckpointRecord = true;
        checkpointRecordMayExist = true;
    }

    std::shared_ptr<Writer> writer = std::make_shared<BufferedFileWriter>(*frozenFileInfo);
    auto& bufferedWriter = writer->cast<BufferedFileWriter>();
    if (enableChecksums) {
        writer = std::make_shared<ChecksumWriter>(std::move(writer), *MemoryManager::Get(*context));
    }
    auto frozenSerializer = std::make_unique<Serializer>(std::move(writer));
    bufferedWriter.setFileOffset(frozenFileInfo->getFileSize());

    CheckpointRecord walRecord{CHECKPOINT_BUNDLE_FORMAT_VERSION};
    frozenSerializer->getWriter()->onObjectBegin();
    WALRecord::serializeWithLength(*frozenSerializer, walRecord);
    frozenSerializer->getWriter()->onObjectEnd();
    try {
        frozenSerializer->getWriter()->flush();
        frozenSerializer->getWriter()->sync();
    } catch (const std::exception& e) {
        std::unique_lock lck{mtx};
        poisonNoLock(std::string{"WAL sync failed: "} + e.what());
        throw RuntimeException(
            "WAL sync failed; database is in a panic state and refuses further writes until "
            "restart. Original error: " +
            std::string{e.what()});
    } catch (...) {
        std::unique_lock lck{mtx};
        poisonNoLock("WAL sync failed: unknown exception");
        throw RuntimeException(
            "WAL sync failed; database is in a panic state and refuses further writes until "
            "restart. Original error: unknown exception");
    }
}

void WAL::clearFrozenWAL() {
    std::unique_lock lck{mtx};
    vfs->removeFileIfExists(checkpointWalPath);
    frozenWALHasCheckpointRecord = false;
    checkpointRecordMayExist = false;
}

void WAL::retireFrozenWAL() {
    std::unique_lock lck{mtx};
    throwIfPoisonedNoLock();
    retireFileNoLock(checkpointWalPath, "Frozen");
    frozenWALHasCheckpointRecord = false;
    checkpointRecordMayExist = false;
}

void WAL::retireActiveWAL() {
    std::unique_lock lck{mtx};
    throwIfPoisonedNoLock();
    fileInfo.reset();
    serializer.reset();
    retireFileNoLock(walPath, "Active");
    activeWALDirectorySynced = false;
    checkpointRecordMayExist = false;
    durableCommitSequence = appendedCommitSequence;
    syncInProgress = false;
    groupCommitCV.notify_all();
}

void WAL::retireFileNoLock(const std::string& path, const char* walName) {
    try {
        if (vfs->fileOrPathExists(path)) {
            auto retiredFileInfo =
                vfs->openFile(path, FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE));
            retiredFileInfo->truncate(0);
            retiredFileInfo->syncFile();
            retiredFileInfo.reset();
            vfs->removeFileIfExists(path);
            vfs->syncParentDirectory(path);
        }
    } catch (const std::exception& e) {
        poisonNoLock(std::string{walName} + " WAL retirement failed: " + e.what());
        throw RuntimeException(std::string{walName} +
                               " WAL retirement failed; database is in a panic state and refuses "
                               "further writes until restart. Correct persistent filesystem "
                               "errors before reopening. Original error: " +
                               e.what());
    } catch (...) {
        poisonNoLock(std::string{walName} + " WAL retirement failed: unknown exception");
        throw RuntimeException(std::string{walName} +
                               " WAL retirement failed; database is in a panic state and refuses "
                               "further writes until restart. Correct persistent filesystem "
                               "errors before reopening. Original error: unknown exception");
    }
}

void WAL::reset() {
    std::unique_lock lck{mtx};
    durableCommitSequence = appendedCommitSequence;
    syncInProgress = false;
    groupCommitCV.notify_all();
    fileInfo.reset();
    serializer.reset();
    vfs->removeFileIfExists(walPath);
    activeWALDirectorySynced = false;
    checkpointRecordMayExist = false;
}

void WAL::waitForDurabilityNoLock(uint64_t commitSequence, std::unique_lock<std::mutex>& lck) {
    while (durableCommitSequence < commitSequence) {
        throwIfPoisonedNoLock();
        if (syncInProgress) {
            groupCommitCV.wait(lck);
            continue;
        }
        syncInProgress = true;
        while (durableCommitSequence < appendedCommitSequence) {
            const auto targetSequence = appendedCommitSequence;
            const auto syncParentDirectory = !activeWALDirectorySynced;
            serializer->getWriter()->flush();
            auto* fileToSync = fileInfo.get();
            lck.unlock();
            try {
                fileToSync->syncFile();
                if (syncParentDirectory) {
                    vfs->syncParentDirectory(walPath);
                }
            } catch (const std::exception& e) {
                lck.lock();
                poisonNoLock(std::string{"WAL sync failed: "} + e.what());
                throw RuntimeException(
                    "WAL sync failed; database is in a panic state and refuses further writes "
                    "until restart. Original error: " +
                    std::string{e.what()});
            } catch (...) {
                lck.lock();
                poisonNoLock("WAL sync failed: unknown exception");
                throw RuntimeException(
                    "WAL sync failed; database is in a panic state and refuses further writes "
                    "until restart. Original error: unknown exception");
            }
            lck.lock();
            if (syncParentDirectory) {
                activeWALDirectorySynced = true;
            }
            durableCommitSequence = targetSequence;
            groupCommitCV.notify_all();
        }
        syncInProgress = false;
        groupCommitCV.notify_all();
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const): semantically non-const function.
void WAL::flushAndSyncNoLock() {
    serializer->getWriter()->flush();
    try {
        serializer->getWriter()->sync();
        if (!activeWALDirectorySynced) {
            vfs->syncParentDirectory(walPath);
            activeWALDirectorySynced = true;
        }
    } catch (const std::exception& e) {
        poisonNoLock(std::string{"WAL sync failed: "} + e.what());
        throw RuntimeException(
            "WAL sync failed; database is in a panic state and refuses further writes until "
            "restart. Original error: " +
            std::string{e.what()});
    } catch (...) {
        poisonNoLock("WAL sync failed: unknown exception");
        throw RuntimeException(
            "WAL sync failed; database is in a panic state and refuses further writes until "
            "restart. Original error: unknown exception");
    }
}

uint64_t WAL::getFileSize() {
    std::unique_lock lck{mtx};
    if (!serializer) {
        if (inMemory || !vfs->fileOrPathExists(walPath)) {
            return 0;
        }
        return vfs->openFile(walPath, FileOpenFlags(FileFlags::READ_ONLY))->getFileSize();
    }
    return serializer->getWriter()->getSize();
}

bool WAL::mayHaveCheckpointRecord() {
    std::unique_lock lck{mtx};
    return checkpointRecordMayExist;
}

void WAL::throwIfPoisoned() {
    std::unique_lock lck{mtx};
    throwIfPoisonedNoLock();
}

void WAL::poison(const std::string& reason) {
    std::unique_lock lck{mtx};
    poisonNoLock(reason);
}

void WAL::throwIfPoisonedNoLock() const {
    if (!poisoned) {
        return;
    }
    throw RuntimeException("WAL is in a panic state and refuses further writes until restart. "
                           "Original error: " +
                           poisonReason);
}

void WAL::poisonNoLock(const std::string& reason) {
    if (poisoned) {
        return;
    }
    poisoned = true;
    poisonReason = reason;
    syncInProgress = false;
    groupCommitCV.notify_all();
}

void WAL::writeHeader(main::ClientContext& context) {
    serializer->getWriter()->onObjectBegin();
    auto* owner =
        storageManager == nullptr ? context.getDatabase()->getStorageManager() : storageManager;
    FileDBIDUtils::writeDatabaseID(*serializer, owner->getOrInitDatabaseID(context));
    serializer->write(enableChecksums);
    serializer->getWriter()->onObjectEnd();
}

void WAL::initWriter(main::ClientContext* context) {
    if (serializer) {
        return;
    }
    fileInfo = vfs->openFile(walPath,
        FileOpenFlags(FileFlags::CREATE_IF_NOT_EXISTS | FileFlags::READ_ONLY | FileFlags::WRITE),
        context);

    std::shared_ptr<Writer> writer = std::make_shared<BufferedFileWriter>(*fileInfo);
    auto& bufferedWriter = writer->cast<BufferedFileWriter>();
    if (enableChecksums) {
        writer = std::make_shared<ChecksumWriter>(std::move(writer), *MemoryManager::Get(*context));
    }
    serializer = std::make_unique<Serializer>(std::move(writer));

    // Write the databaseID at the start of the WAL if needed
    // This is used to ensure that when replaying the WAL matches the database
    if (fileInfo->getFileSize() == 0) {
        writeHeader(*context);
    }

    // WAL should always be APPEND only. We don't want to overwrite the file as it may still
    // contain records not replayed. This can happen if checkpoint is not triggered before the
    // Database is closed last time.
    bufferedWriter.setFileOffset(fileInfo->getFileSize());
}

// NOLINTNEXTLINE(readability-make-member-function-const): semantically non-const function.
void WAL::addNewWALRecordNoLock(const WALRecord& walRecord) {
    DASSERT(walRecord.type != WALRecordType::INVALID_RECORD);
    DASSERT(!inMemory);
    DASSERT(serializer != nullptr);
    serializer->getWriter()->onObjectBegin();
    WALRecord::serializeWithLength(*serializer, walRecord);
    serializer->getWriter()->onObjectEnd();
}

WAL* WAL::Get(const main::ClientContext& context) {
    DASSERT(context.getDatabase() && context.getDatabase()->getStorageManager());
    return &context.getDatabase()->getStorageManager()->getWAL();
}

} // namespace storage
} // namespace lbug
