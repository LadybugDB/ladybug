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
class WAL {
public:
    WAL(const std::string& dbPath, bool readOnly, bool enableChecksums,
        common::VirtualFileSystem* vfs);
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
    // failure the frozen WAL is left for recovery.
    void undoRotationForCheckpoint() noexcept;
    void clearFrozenWAL();

    // Clear any buffer in the WAL writer. Also truncate the WAL file to 0 bytes.
    void clear();
    // Reset the WAL writer to nullptr, and remove the WAL file if it exists.
    void reset();

    uint64_t getFileSize();
    void throwIfPoisoned();
    // Recovery only: while set, the next checkpoint commits the frozen WAL of an interrupted
    // checkpoint, whose records recovery has replayed, instead of rotating the active WAL.
    void setAdoptFrozenWALForCheckpoint(bool adopt);

    static WAL* Get(const main::ClientContext& context);

private:
    void initWriter(main::ClientContext* context);
    void addNewWALRecordNoLock(const WALRecord& walRecord);
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
    std::unique_ptr<common::FileInfo> fileInfo;
    std::condition_variable groupCommitCV;
    uint64_t appendedCommitSequence = 0;
    uint64_t durableCommitSequence = 0;
    bool adoptFrozenWAL = false;
    bool syncInProgress = false;
    bool poisoned = false;
    std::string poisonReason;
    // Set once a CHECKPOINT record may have reached the frozen WAL. From then on, the frozen WAL
    // is the checkpoint's commit record and must be left for recovery.
    bool frozenWALHasCheckpointRecord = false;

    // Since most writes to the shared WAL will be flushing local WAL (which has its own checksums),
    // these writes can go through the normal writer. We do still need a checksum writer though for
    // writing COMMIT/CHECKPOINT records
    std::unique_ptr<common::Serializer> serializer;
    bool enableChecksums;
};

} // namespace storage
} // namespace lbug
