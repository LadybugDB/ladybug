#pragma once

#include <optional>
#include <string>
#include <vector>

#include "storage/wal/wal_record.h"

namespace lbug {
namespace main {
class ClientContext;
} // namespace main

namespace storage {
class Checkpointer;
class StorageManager;
class WALReplayer {
public:
    struct GraphRecoveryState {
        struct WALReplayRange {
            std::string path;
            uint64_t endOffset;
        };

        std::vector<WALReplayRange> walReplayRanges;
        bool retireActiveWAL = false;
        bool retireFrozenWAL = false;
    };

    explicit WALReplayer(main::ClientContext& clientContext);

    void replay(bool throwOnWalReplayFailure, bool enableChecksums) const;
    GraphRecoveryState prepareGraphCheckpoint(StorageManager& storageManager, bool checkpointBundle,
        std::optional<common::uuid> checkpointDatabaseID = std::nullopt) const;
    void replayGraphWAL(StorageManager& storageManager,
        const GraphRecoveryState& recoveryState) const;
    void retireGraphCheckpointWALs(StorageManager& storageManager,
        const GraphRecoveryState& recoveryState) const;
    void removeGraphCheckpointShadow(StorageManager& storageManager) const;

private:
    struct WALReplayInfo {
        uint64_t offsetDeserialized = 0;
        bool isLastRecordCheckpoint = false;
        common::uuid walDatabaseID{};
        uint64_t checkpointFormatVersion = 0;
    };

    void replayWALRecord(WALRecord& walRecord) const;
    void replayCreateCatalogEntryRecord(WALRecord& walRecord) const;
    void replayCreateIndexRecord(WALRecord& walRecord) const;
    void replayDropCatalogEntryRecord(const WALRecord& walRecord) const;
    void replayAlterTableEntryRecord(const WALRecord& walRecord) const;
    void replayTableInsertionRecord(const WALRecord& walRecord) const;
    void replayNodeDeletionRecord(const WALRecord& walRecord) const;
    void replayNodeUpdateRecord(const WALRecord& walRecord) const;
    void replayRelDeletionRecord(const WALRecord& walRecord) const;
    void replayRelDetachDeletionRecord(const WALRecord& walRecord) const;
    void replayRelUpdateRecord(const WALRecord& walRecord) const;
    void replayCopyTableRecord(const WALRecord& walRecord) const;
    void replayUpdateSequenceRecord(const WALRecord& walRecord) const;

    void replayNodeTableInsertRecord(const WALRecord& walRecord) const;
    void replayRelTableInsertRecord(const WALRecord& walRecord) const;

    void replayLoadExtensionRecord(const WALRecord& walRecord) const;

    // This function is used to deserialize the WAL records without actually applying them to the
    // storage.
    WALReplayInfo dryReplay(common::FileInfo& fileInfo, bool throwOnWalReplayFailure,
        bool enableChecksums) const;

    void replayFrozenWAL(Checkpointer& checkpointer, bool throwOnWalReplayFailure,
        bool enableChecksums) const;
    void replayActiveWAL(Checkpointer& checkpointer, bool throwOnWalReplayFailure,
        bool enableChecksums, bool checkpointRead) const;
    void replayCommittedCheckpoint(Checkpointer& checkpointer,
        std::unique_ptr<common::FileInfo>& fileInfo, const std::string& checkpointWALPath,
        const WALReplayInfo& replayInfo) const;
    // Checkpoints the state replayed from a frozen WAL without a CHECKPOINT record, committing
    // that frozen WAL instead of rotating the active WAL.
    void completeInterruptedCheckpoint() const;

    common::uuid readShadowDatabaseID(const std::string& path) const;
    // A shadow file stamped with another database's ID belongs to a checkpoint bundle whose fate
    // only that database can decide, so a standalone open must leave every recovery artifact in
    // place instead of discarding the bundle's shadow (see the call site in replay()).
    void throwIfShadowOwnedByAnotherDatabase() const;

    void removeWALAndShadowFiles(const std::string& walFilePath) const;
    void removeFileAndSyncParentDirectory(const std::string& path) const;
    bool removeFileIfExists(const std::string& path) const;

    std::unique_ptr<common::FileInfo> openWALFile() const;
    void syncWALFile(const common::FileInfo& fileInfo) const;
    void truncateWALFile(common::FileInfo& fileInfo, uint64_t size) const;

private:
    main::ClientContext& clientContext;
    std::string walPath;
    std::string checkpointWalPath;
    std::string shadowFilePath;
};

} // namespace storage
} // namespace lbug
