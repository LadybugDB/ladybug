#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <unordered_set>

#include "attached_database.h"
#include "common/copy_constructors.h"
#include "storage/partition_storage_registry.h"

namespace lbug {
namespace catalog {
class Catalog;
class GraphCatalogEntry;
} // namespace catalog

namespace storage {
class MemoryManager;
class StorageManager;

class PartitionStorageRegistry;
} // namespace storage

namespace main {

struct GraphWALReplayRequest;

class DatabaseManager {
public:
    DatabaseManager();
    ~DatabaseManager();

    void registerAttachedDatabase(std::unique_ptr<AttachedDatabase> attachedDatabase);
    bool hasAttachedDatabase(const std::string& name);
    LBUG_API AttachedDatabase* getAttachedDatabase(const std::string& name);
    void detachDatabase(const std::string& databaseName);
    std::string getDefaultDatabase() const { return defaultDatabase; }
    bool hasDefaultDatabase() const { return defaultDatabase != ""; }
    void setDefaultDatabase(const std::string& databaseName);
    std::vector<AttachedDatabase*> getAttachedDatabases() const;

    void createGraph(const std::string& graphName, storage::MemoryManager* memoryManager,
        main::ClientContext* clientContext, bool isAnyGraph = false);
    void dropGraph(const std::string& graphName, main::ClientContext* clientContext);
    void loadGraphsFromCatalog(storage::MemoryManager* memoryManager,
        main::ClientContext* clientContext);
    void loadGraphsFromCatalog(storage::MemoryManager* memoryManager,
        main::ClientContext* clientContext, bool mainCheckpointCommitted, bool checkpointBundle,
        std::optional<common::uuid> legacyCheckpointDatabaseID = std::nullopt);
    // Materializes one named graph on demand (see loadGraphsFromCatalog for the batch
    // equivalent). Used during WAL replay when a record tagged with the graph's name
    // arrives before the graph is loaded. Returns true when the graph is loaded afterwards.
    bool loadGraphFromCatalog(storage::MemoryManager* memoryManager,
        main::ClientContext* clientContext, const std::string& graphName);
    // Replays graph WALs queued by loadGraphFromCatalog. Deferred while a recovery
    // transaction is active and run at the next transaction-free point (a replayed commit
    // or the end of a replay pass).
    void replayPendingGraphWALs(main::ClientContext* clientContext);
    void setDefaultGraph(const std::string& graphName);
    void clearDefaultGraph();
    bool hasGraph(const std::string& graphName);
    catalog::Catalog* getGraphCatalog(const std::string& graphName);
    // Runs action with the graph registry's shared lock held, so a concurrent DROP GRAPH
    // cannot destroy the catalog for the action's duration. Callers that dereference the
    // catalog after a getGraphCatalog lookup must use this instead; getGraphCatalog only
    // guards the lookup itself.
    void withGraphCatalog(const std::string& graphName,
        const std::function<void(catalog::Catalog*)>& action);
    catalog::Catalog* getDefaultGraphCatalog() const;
    catalog::Catalog* getReplayOwnerCatalog() const { return replayOwnerCatalog; }
    void setReplayOwnerCatalog(catalog::Catalog* catalog) { replayOwnerCatalog = catalog; }
    bool hasDefaultGraph() const { return defaultGraph != "" && defaultGraph != "main"; }
    std::string getDefaultGraphName() const { return defaultGraph; }
    std::vector<catalog::Catalog*> getGraphs() const;
    void bumpGraphCatalogVersions(const std::unordered_set<catalog::Catalog*>& catalogs);
    storage::StorageManager* getDefaultGraphStorageManager() const;

    LBUG_API void invalidateCache();

    // Given a table ID, find the database that contains it and return its
    // catalog and storage manager. Returns (main catalog, main storage manager)
    // if the table is in the main database. If dbName is non-empty, use that
    // database directly.
    static std::pair<catalog::Catalog*, storage::StorageManager*> resolveTableStorage(
        const ClientContext& context, common::table_id_t tableID, const std::string& dbName = {});

    LBUG_API static DatabaseManager* Get(const ClientContext& context);

private:
    bool loadGraph(main::ClientContext* clientContext, storage::MemoryManager* memoryManager,
        catalog::GraphCatalogEntry* graphEntry, bool mainCheckpointCommitted, bool checkpointBundle,
        std::optional<common::uuid> legacyCheckpointDatabaseID);

    std::vector<std::unique_ptr<AttachedDatabase>> attachedDatabases;
    std::string defaultDatabase;
    std::vector<std::unique_ptr<catalog::Catalog>> graphs;
    mutable std::shared_mutex graphsMutex;
    // withGraphCatalog callbacks re-enter registry lookups on the same thread (e.g.
    // index initialization resolving the default graph catalog); std::shared_mutex is
    // not recursive, so nested same-thread acquisitions are counted instead of
    // reacquired.
    inline static thread_local uint64_t graphsSharedHolds = 0;

    void acquireGraphsShared() const;
    void releaseGraphsShared() const;

    class GraphsSharedLock {
    public:
        explicit GraphsSharedLock(const DatabaseManager& dbManager) : dbManager(dbManager) {
            dbManager.acquireGraphsShared();
        }
        DELETE_COPY_AND_MOVE(GraphsSharedLock);
        ~GraphsSharedLock() { dbManager.releaseGraphsShared(); }

    private:
        const DatabaseManager& dbManager;
    };
    // Owns the per-partition data files of partitioned node tables (phase-B per-partition
    // storage; see docs/partitioning.md 6b).
    storage::PartitionStorageRegistry partitionStorageRegistry;
    catalog::Catalog* replayOwnerCatalog = nullptr;
    std::vector<std::unique_ptr<GraphWALReplayRequest>> pendingGraphWALReplays;

public:
    storage::PartitionStorageRegistry* getPartitionStorageRegistry() {
        return &partitionStorageRegistry;
    }
    std::string defaultGraph;
};

} // namespace main
} // namespace lbug
