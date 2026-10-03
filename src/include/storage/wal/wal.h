#pragma once

#include <condition_variable>
#include <mutex>
#include <string>

#include "storage/wal/wal_record.h"

namespace lbug {
namespace common {
class BufferedFileWriter;
class VirtualFileSystem;
} // namespace common

namespace storage {
class LocalWAL;
class StorageManager;
class WAL {
public:
    static constexpr uint64_t CHECKPOINT_BUNDLE_FORMAT_VERSION = 2;

    // Recovery only: adopt the frozen WAL for one checkpoint, clearing the request on exit even
    // if the checkpoint fails before rotation.
    class FrozenWALAdoptionGuard {
    public:
        explicit FrozenWALAdoptionGuard(WAL& wal);
        ~FrozenWALAdoptionGuard();

        FrozenWALAdoptionGuard(const FrozenWALAdoptionGuard&) = delete;
        FrozenWALAdoptionGuard& operator=(const FrozenWALAdoptionGuard&) = delete;

    private:
        WAL& wal;
    };

    WAL(const std::string& dbPath, bool readOnly, bool enableChecksums,
        common::VirtualFileSystem* vfs, StorageManager* storageManager = nullptr);
    ~WAL();

    void logCommittedWAL(LocalWAL& localWAL, main::ClientContext* context,
        uint64_t& commitSequence);
    void logAndFlushCheckpoint(main::ClientContext* context);

    // Renames the active WAL to the frozen checkpoint WAL. Returns false if there is nothing to
    // rotate. Throws if a frozen WAL from an earlier checkpoint is still on disk, since
    // overwriting it would drop records that recovery still needs.
    bool rotateForCheckpoint(main::ClientContext* context);
    void logAndFlushCheckpointToFrozen(main::ClientContext* context);
    // Undoes rotateForCheckpoint() for a checkpoint that failed before its CHECKPOINT record was
    // written, so the records it froze become part of the active WAL again. Never throws; on
    // failure the frozen WAL is left for recovery and the WAL is poisoned.
    void undoRotationForCheckpoint() noexcept;
    void clearFrozenWAL();
    void retireFrozenWAL();
    void retireActiveWAL();

    // Reset the WAL writer to nullptr, and remove the WAL file if it exists.
    void reset();

    uint64_t getFileSize();
    bool mayHaveCheckpointRecord();
    void throwIfPoisoned();
    void poison(const std::string& reason);

    static WAL* Get(const main::ClientContext& context);

private:
    void initWriter(main::ClientContext* context);
    void addNewWALRecordNoLock(const WALRecord& walRecord);
    void retireFileNoLock(const std::string& path, const char* walName);
    void throwIfPoisonedNoLock() const;
    void poisonNoLock(const std::string& reason);
    void waitForDurabilityNoLock(uint64_t commitSequence, std::unique_lock<std::mutex>& lck);
    void flushAndSyncNoLock();
    void writeHeader(main::ClientContext& context);

private:
    std::mutex mtx;
    std::string walPath;
    std::string checkpointWalPath;
    bool inMemory;
    [[maybe_unused]] bool readOnly;
    common::VirtualFileSystem* vfs;
    StorageManager* storageManager;
    std::unique_ptr<common::FileInfo> fileInfo;
    std::condition_variable groupCommitCV;
    uint64_t appendedCommitSequence = 0;
    uint64_t durableCommitSequence = 0;
    bool adoptFrozenWAL = false;
    bool syncInProgress = false;
    bool activeWALDirectorySynced = false;
    bool poisoned = false;
    std::string poisonReason;
    // Set before serializing a CHECKPOINT record because a failed sync has an ambiguous outcome.
    bool frozenWALHasCheckpointRecord = false;
    bool checkpointRecordMayExist = false;

    // Since most writes to the shared WAL will be flushing local WAL (which has its own checksums),
    // these writes can go through the normal writer. We do still need a checksum writer though for
    // writing COMMIT/CHECKPOINT records
    std::unique_ptr<common::Serializer> serializer;
    bool enableChecksums;
};

} // namespace storage
} // namespace lbug
