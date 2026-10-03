#pragma once

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types/types.h"
#include "common/types/uuid.h"

namespace lbug {
namespace common {
class VirtualFileSystem;
}
namespace catalog {
class Catalog;
class TableCatalogEntry;
} // namespace catalog
namespace main {
class ClientContext;
}
namespace storage {
class StorageManager;
class NodeTable;

// Per-partition storage. Each partition child of a partitioned node table gets its own
// StorageManager whose data file lives next to the main database file as
// `<base>.<childName>.db`, mirroring how CREATE GRAPH lays out per-graph databases.
//
// The main catalog remains the single source of truth: the registry maps child table IDs to
// their storage managers, and lazily re-opens a child's file from its catalog entry name after
// a reopen (the registry starts empty). Claimed remote partitions (partition-routing hooks)
// never enter the registry, so it holds exactly the local partitions.
class PartitionStorageRegistry {
public:
    static PartitionStorageRegistry* Get(main::ClientContext* context);

    // True if the partition subgraph identified by (parentTableID, partitionIndex) is claimed
    // by an installed PartitionRoutingHooks wrapper and therefore owns no local storage: no
    // data file, no table object, no WAL/checkpoint work. Data is served through the hooks.
    static bool isRemotelyRouted(common::table_id_t parentTableID, uint64_t partitionIndex);

    // Returns the child's StorageManager, creating (and registering) its data file on first
    // use. Idempotent. With requireExistingFile, throws instead of creating a missing or
    // headerless data file. With loadChildState=false, registers the child without parsing its
    // database header or page manager: committed-checkpoint recovery uses this because an
    // interrupted shadow-page apply can leave those pages half-replaced; the caller must load
    // them afterwards (replayCheckpointShadows does) before anything reads or allocates from
    // the file.
    storage::StorageManager& getOrCreate(main::ClientContext* context, common::table_id_t tableID,
        const std::string& childName, bool requireExistingFile = false, bool loadChildState = true);

    // Existing entry or nullptr; never opens a new file.
    storage::StorageManager* tryGet(common::table_id_t tableID);

    // Snapshot of all live partition StorageManagers (for checkpoint/rollback sweeps).
    std::vector<storage::StorageManager*> getAllManagers();

    // Resolution seam for every site that turns a table entry into its NodeTable*. Non-partition
    // children resolve through the main StorageManager exactly as before.
    static storage::NodeTable* resolveNodeTable(main::ClientContext* context,
        catalog::TableCatalogEntry& entry);

    // By-ID variant for paths that only carry a table ID (local-storage commit, WAL replay).
    // Throws if the ID is unknown to the catalog.
    static storage::NodeTable* resolveNodeTableByID(main::ClientContext* context,
        common::table_id_t tableID);

    // Closes each listed child's file handles and deletes its data + WAL files. Used by the
    // DROP-parent cascade and by rollback cleanup of dynamically created partitions.
    void dropAll(main::ClientContext* context, const std::vector<common::table_id_t>& tableIDs);

    // Checkpoint-time sweep: remove registry entries (and their files) whose catalog entry is
    // gone — covers dropped partitioned tables and rolled-back dynamic partitions.
    void dropAllNotInCatalog(main::ClientContext* context, const catalog::Catalog& catalog);

    // Checkpoint-time fix-up: move child files whose catalog name changed (parent rename).
    void reconcilePaths(main::ClientContext* context, const catalog::Catalog& catalog);

    // Eagerly open storage for every partition child in the catalog. Called once at database
    // open so every session holds live Table objects for all partitions (checkpoint serialize,
    // planner stats, and SHOW_INDEXES all resolve through the registry). With
    // recoveringCommittedCheckpoint, throws on a missing child file instead of creating it and
    // defers parsing each child's header/page manager and constructing its table until
    // replayCheckpointShadows has repaired the file; the main file's metadata stream then
    // constructs the tables (StorageManager::deserialize).
    void openAllChildren(main::ClientContext* context, const catalog::Catalog& catalog,
        bool recoveringCommittedCheckpoint = false);

    // Recovery of a committed checkpoint only, after openAllChildren(..., true): applies every
    // registered child's shadow file, then loads each child's database header and page manager
    // from the repaired file. A legacyDatabaseID also accepts shadow files stamped with that
    // pre-bundle database ID. With checkpointBundle (version-1 bundle protocol) every child's
    // shadow must exist; the legacy protocol applied and removed each child's shadow before the
    // checkpoint committed, so a missing shadow there means the child file is already fully
    // applied.
    void replayCheckpointShadows(main::ClientContext* context,
        std::optional<common::uuid> legacyDatabaseID, bool checkpointBundle);

    // Move a partition child's data file (and sidecars) after its catalog rename, keeping the
    // live Table object and its page mappings intact.
    void renameChild(main::ClientContext* context, common::table_id_t tableID,
        const std::string& newChildName);

private:
    std::shared_mutex mtx;
    std::unordered_map<common::table_id_t, std::unique_ptr<storage::StorageManager>> managers;
};

} // namespace storage
} // namespace lbug
