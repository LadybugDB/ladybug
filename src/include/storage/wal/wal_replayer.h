#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "storage/wal/wal_record.h"

namespace lbug {
namespace catalog {
class Catalog;
} // namespace catalog

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
        common::oid_t persistedGraphOIDFloor = 0;
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
    void recordReplayedEntryID(catalog::CatalogEntryType entryType, common::oid_t recordedEntryID,
        common::oid_t replayedEntryID) const;
    common::oid_t getReplayedEntryID(catalog::CatalogEntryType entryType,
        common::oid_t recordedEntryID) const;
    bool tryGetReplayedEntryID(catalog::CatalogEntryType entryType, common::oid_t recordedEntryID,
        common::oid_t& replayedEntryID) const;
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
    void replayUpdateSequenceNamedRecord(const WALRecord& walRecord) const;
    void replaySequenceRecord(common::sequence_id_t sequenceID, uint64_t kCount,
        const std::string& sequenceName) const;

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
    // Owner names whose materialization already failed during this replay pass, so
    // their remaining records skip the graph-recovery pass instead of repeating it.
    // Cleared whenever replayed graph DDL can change which owners are loadable.
    mutable std::unordered_set<std::string> failedOwnerNames;
    // Last owner resolved by replayWALRecord, keyed by its upper name, so a run of
    // records tagged with one graph skips the per-record registry scans. A cached
    // catalog is valid only while registry membership cannot change: every record
    // case that can change it (graph create/drop, load-extension replay) and every
    // pass start null it first, so a hit needs no liveness check.
    mutable std::string cachedOwnerName;
    mutable catalog::Catalog* cachedOwnerCatalog = nullptr;
    // Entry IDs recorded in a graph's WAL come from the recording session's view of its
    // catalog, which can differ from the replaying catalog: a standalone session's plain
    // catalog lacks the ANY-graph infrastructure entries the graph materializes with,
    // and rolled-back CREATEs consume IDs without leaving WAL records. CREATE replay
    // records each recorded->replayed ID per entry type (each type has an independent
    // ID space); ID-addressed records translate through this map, scoped to the owning
    // catalog the record replays against, and fall back to the recorded ID. Cleared per
    // replayGraphWAL call.
    mutable std::unordered_map<const catalog::Catalog*,
        std::unordered_map<catalog::CatalogEntryType,
            std::unordered_map<common::oid_t, common::oid_t>>>
        replayedEntryIDs;
    // Graph-entry IDs this pass could have assigned, from the base catalog's graph-set
    // counter captured when the pass applies its first record (the persisted state).
    // GRAPH_ENTRY drops treat recorded IDs at or above this floor as shifting replay
    // IDs rather than stable persisted ones. Captured once per pass; graph WAL passes
    // reset it alongside replayedEntryIDs.
    mutable common::oid_t graphOIDReplayFloor = 0;
    mutable bool graphOIDReplayFloorValid = false;
    // Catalogs a GRAPH_ENTRY drop unregistered mid-pass. The active recovery transaction
    // can hold undo records pointing into them, so destruction waits until the replayer
    // dies, after the pass and its transactions have completed.
    mutable std::vector<std::unique_ptr<catalog::Catalog>> retiredCatalogs;
};

} // namespace storage
} // namespace lbug
