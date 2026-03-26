#include "storage/wal/wal_replayer.h"

#include "common/file_system/file_info.h"
#include "common/file_system/file_system.h"
#include "common/file_system/virtual_file_system.h"
#include "common/serializer/buffered_file.h"
#include "common/type_utils.h"
#include "common/types/types.h"
#include "main/client_context.h"
#include "storage/checkpointer.h"
#include "storage/file_db_id_utils.h"
#include "storage/local_storage/local_rel_table.h"
#include "storage/storage_manager.h"
#include "storage/wal/checksum_reader.h"
#include "storage/wal/wal_record.h"
#include "transaction/transaction_context.h"
#include <format>

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

static constexpr std::string_view checksumMismatchMessage =
    "Checksum verification failed, the WAL file is corrupted.";

WALReplayer::WALReplayer(main::ClientContext& clientContext) : clientContext{clientContext} {
    walPath = StorageUtils::getWALFilePath(clientContext.getDatabasePath());
    checkpointWalPath = StorageUtils::getCheckpointWALFilePath(clientContext.getDatabasePath());
    shadowFilePath = StorageUtils::getShadowFilePath(clientContext.getDatabasePath());
}

static WALHeader readWALHeader(Deserializer& deserializer) {
    WALHeader header{};
    deserializer.deserializeValue(header.databaseID);

    // It is possible to read a value other than 0/1 when deserializing the flag
    // This causes some weird behaviours with some toolchains so we manually do the conversion here
    uint8_t enableChecksumsBytes = 0;
    deserializer.deserializeValue(enableChecksumsBytes);
    header.enableChecksums = enableChecksumsBytes != 0;

    return header;
}

static Deserializer initDeserializer(FileInfo& fileInfo, main::ClientContext& clientContext,
    bool enableChecksums) {
    if (enableChecksums) {
        return Deserializer{std::make_unique<ChecksumReader>(fileInfo,
            *MemoryManager::Get(clientContext), checksumMismatchMessage)};
    } else {
        return Deserializer{std::make_unique<BufferedFileReader>(fileInfo)};
    }
}

static void checkWALHeader(const WALHeader& header, bool enableChecksums) {
    if (enableChecksums != header.enableChecksums) {
        throw RuntimeException(std::format(
            "The database you are trying to open was serialized with enableChecksums={} but you "
            "are trying to open it with enableChecksums={}. Please open your database using the "
            "correct enableChecksums config. If you wish to change this for your database, please "
            "use the export/import functionality.",
            TypeUtils::toString(header.enableChecksums), TypeUtils::toString(enableChecksums)));
    }
}

static uint64_t getReadOffset(Deserializer& deSer, bool enableChecksums) {
    if (enableChecksums) {
        return deSer.getReader()->cast<ChecksumReader>()->getReadOffset();
    } else {
        return deSer.getReader()->cast<BufferedFileReader>()->getReadOffset();
    }
}

void WALReplayer::replay(bool throwOnWalReplayFailure, bool enableChecksums) const {
    auto vfs = VirtualFileSystem::GetUnsafe(clientContext);
    Checkpointer checkpointer(clientContext);
    bool hasFrozenWAL = vfs->fileOrPathExists(checkpointWalPath, &clientContext);
    bool hasActiveWAL = vfs->fileOrPathExists(walPath, &clientContext);

    if (!hasFrozenWAL && !hasActiveWAL) {
        removeFileIfExists(shadowFilePath);
        checkpointer.readCheckpoint();
        return;
    }

    if (hasFrozenWAL) {
        replayFrozenWAL(checkpointer, throwOnWalReplayFailure, enableChecksums);
    } else {
        removeFileIfExists(shadowFilePath);
        checkpointer.readCheckpoint();
    }

    if (hasActiveWAL) {
        replayActiveWAL(checkpointer, throwOnWalReplayFailure, enableChecksums);
    }
}

void WALReplayer::replayFrozenWAL(Checkpointer& checkpointer, bool throwOnWalReplayFailure,
    bool enableChecksums) const {
    auto vfs = VirtualFileSystem::GetUnsafe(clientContext);
    auto fileInfo =
        vfs->openFile(checkpointWalPath, FileOpenFlags(FileFlags::READ_ONLY | FileFlags::WRITE));
    if (fileInfo->getFileSize() == 0) {
        removeFileIfExists(checkpointWalPath);
        removeFileIfExists(shadowFilePath);
        checkpointer.readCheckpoint();
        return;
    }
    syncWALFile(*fileInfo);

    try {
        auto [offsetDeserialized, isLastRecordCheckpoint] =
            dryReplay(*fileInfo, throwOnWalReplayFailure, enableChecksums);
        if (isLastRecordCheckpoint) {
            ShadowFile::replayShadowPageRecords(clientContext);
            removeFileIfExists(checkpointWalPath);
            removeFileIfExists(shadowFilePath);
            checkpointer.readCheckpoint();
        } else {
            removeFileIfExists(shadowFilePath);
            checkpointer.readCheckpoint();
            Deserializer deserializer = initDeserializer(*fileInfo, clientContext, enableChecksums);
            if (offsetDeserialized > 0) {
                deserializer.getReader()->onObjectBegin();
                const auto walHeader = readWALHeader(deserializer);
                FileDBIDUtils::verifyDatabaseID(*fileInfo,
                    StorageManager::Get(clientContext)->getOrInitDatabaseID(clientContext),
                    walHeader.databaseID);
                deserializer.getReader()->onObjectEnd();
            }
            while (getReadOffset(deserializer, enableChecksums) < offsetDeserialized) {
                DASSERT(!deserializer.finished());
                auto walRecord = WALRecord::deserialize(deserializer, clientContext);
                replayWALRecord(*walRecord);
            }
            removeFileIfExists(checkpointWalPath);
        }
    } catch (const std::exception&) {
        auto transactionContext = TransactionContext::Get(clientContext);
        if (transactionContext->hasActiveTransaction()) {
            transactionContext->rollback();
        }
        throw;
    }
}

void WALReplayer::replayActiveWAL(Checkpointer& checkpointer, bool throwOnWalReplayFailure,
    bool enableChecksums) const {
    auto fileInfo = openWALFile();
    if (fileInfo->getFileSize() == 0) {
        removeFileIfExists(walPath);
        return;
    }
    syncWALFile(*fileInfo);

    try {
        auto [offsetDeserialized, isLastRecordCheckpoint] =
            dryReplay(*fileInfo, throwOnWalReplayFailure, enableChecksums);
        if (isLastRecordCheckpoint) {
            ShadowFile::replayShadowPageRecords(clientContext);
            removeWALAndShadowFiles();
            checkpointer.readCheckpoint();
        } else {
            Deserializer deserializer = initDeserializer(*fileInfo, clientContext, enableChecksums);
            if (offsetDeserialized > 0) {
                deserializer.getReader()->onObjectBegin();
                const auto walHeader = readWALHeader(deserializer);
                FileDBIDUtils::verifyDatabaseID(*fileInfo,
                    StorageManager::Get(clientContext)->getOrInitDatabaseID(clientContext),
                    walHeader.databaseID);
                deserializer.getReader()->onObjectEnd();
            }
            while (getReadOffset(deserializer, enableChecksums) < offsetDeserialized) {
                DASSERT(!deserializer.finished());
                auto walRecord = WALRecord::deserialize(deserializer, clientContext);
                replayWALRecord(*walRecord);
            }
            truncateWALFile(*fileInfo, offsetDeserialized);
        }
    } catch (const std::exception&) {
        auto transactionContext = TransactionContext::Get(clientContext);
        if (transactionContext->hasActiveTransaction()) {
            transactionContext->rollback();
        }
        throw;
    }
}

WALReplayer::WALReplayInfo WALReplayer::dryReplay(FileInfo& fileInfo, bool throwOnWalReplayFailure,
    bool enableChecksums) const {
    uint64_t offsetDeserialized = 0;
    bool isLastRecordCheckpoint = false;
    try {
        Deserializer deserializer = initDeserializer(fileInfo, clientContext, enableChecksums);

        // Skip the databaseID here, we'll verify it when we actually replay
        deserializer.getReader()->onObjectBegin();
        const auto walHeader = readWALHeader(deserializer);
        checkWALHeader(walHeader, enableChecksums);
        deserializer.getReader()->onObjectEnd();

        bool finishedDeserializing = deserializer.finished();
        while (!finishedDeserializing) {
            auto walRecord = WALRecord::deserialize(deserializer, clientContext);
            finishedDeserializing = deserializer.finished();
            switch (walRecord->type) {
            case WALRecordType::CHECKPOINT_RECORD: {
                DASSERT(finishedDeserializing);
                // If we reach a checkpoint record, we can stop replaying.
                isLastRecordCheckpoint = true;
                finishedDeserializing = true;
                offsetDeserialized = getReadOffset(deserializer, enableChecksums);
            } break;
            case WALRecordType::COMMIT_RECORD: {
                // Update the offset to the end of the last commit record.
                offsetDeserialized = getReadOffset(deserializer, enableChecksums);
            } break;
            default: {
                // DO NOTHING.
            }
            }
        }
    } catch (...) {
        // If we hit an exception while deserializing, we assume that the WAL file is (partially)
        // corrupted. This should only happen for records of the last transaction recorded.
        if (throwOnWalReplayFailure) {
            throw;
        }
    }
    return {offsetDeserialized, isLastRecordCheckpoint};
}

void WALReplayer::replayWALRecord(WALRecord& walRecord) const {
    switch (walRecord.type) {
    case WALRecordType::BEGIN_TRANSACTION_RECORD: {
        TransactionContext::Get(clientContext)->beginRecoveryTransaction();
    } break;
    case WALRecordType::COMMIT_RECORD: {
        TransactionContext::Get(clientContext)->commit();
    } break;
    case WALRecordType::CREATE_CATALOG_ENTRY_RECORD: {
        replayCreateCatalogEntryRecord(walRecord);
    } break;
    case WALRecordType::DROP_CATALOG_ENTRY_RECORD: {
        replayDropCatalogEntryRecord(walRecord);
    } break;
    case WALRecordType::ALTER_TABLE_ENTRY_RECORD: {
        replayAlterTableEntryRecord(walRecord);
    } break;
    case WALRecordType::TABLE_INSERTION_RECORD: {
        replayTableInsertionRecord(walRecord);
    } break;
    case WALRecordType::NODE_DELETION_RECORD: {
        replayNodeDeletionRecord(walRecord);
    } break;
    case WALRecordType::NODE_UPDATE_RECORD: {
        replayNodeUpdateRecord(walRecord);
    } break;
    case WALRecordType::REL_DELETION_RECORD: {
        replayRelDeletionRecord(walRecord);
    } break;
    case WALRecordType::REL_DETACH_DELETE_RECORD: {
        replayRelDetachDeletionRecord(walRecord);
    } break;
    case WALRecordType::REL_UPDATE_RECORD: {
        replayRelUpdateRecord(walRecord);
    } break;
    case WALRecordType::COPY_TABLE_RECORD: {
        replayCopyTableRecord(walRecord);
    } break;
    case WALRecordType::UPDATE_SEQUENCE_RECORD: {
        replayUpdateSequenceRecord(walRecord);
    } break;
    case WALRecordType::LOAD_EXTENSION_RECORD: {
        replayLoadExtensionRecord(walRecord);
    } break;
    case WALRecordType::CHECKPOINT_RECORD: {
        // This record should not be replayed. It is only used to indicate that the previous records
        // had been replayed and shadow files are created.
        UNREACHABLE_CODE;
    }
    default:
        UNREACHABLE_CODE;
    }
}

void WALReplayer::removeWALAndShadowFiles() const {
    removeFileIfExists(shadowFilePath);
    removeFileIfExists(walPath);
}

void WALReplayer::removeFileIfExists(const std::string& path) const {
    if (StorageManager::Get(clientContext)->isReadOnly()) {
        return;
    }
    auto vfs = VirtualFileSystem::GetUnsafe(clientContext);
    if (vfs->fileOrPathExists(path, &clientContext)) {
        vfs->removeFileIfExists(path);
    }
}

std::unique_ptr<FileInfo> WALReplayer::openWALFile() const {
    auto flag = FileFlags::READ_ONLY;
    if (!StorageManager::Get(clientContext)->isReadOnly()) {
        flag |= FileFlags::WRITE; // The write flag here is to ensure the file is opened with O_RDWR
                                  // so that we can sync it.
    }
    return VirtualFileSystem::GetUnsafe(clientContext)->openFile(walPath, FileOpenFlags(flag));
}

void WALReplayer::syncWALFile(const FileInfo& fileInfo) const {
    if (StorageManager::Get(clientContext)->isReadOnly()) {
        return;
    }
    fileInfo.syncFile();
}

void WALReplayer::truncateWALFile(FileInfo& fileInfo, uint64_t size) const {
    if (StorageManager::Get(clientContext)->isReadOnly()) {
        return;
    }
    if (fileInfo.getFileSize() > size) {
        fileInfo.truncate(size);
        fileInfo.syncFile();
    }
}

} // namespace storage
} // namespace lbug
