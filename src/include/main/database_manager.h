#pragma once

#include "attached_database.h"

namespace lbug {
namespace catalog {
class Catalog;
} // namespace catalog

namespace storage {
class MemoryManager;
class StorageManager;
} // namespace storage

namespace main {

class DatabaseManager {
public:
    DatabaseManager();

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
    void setDefaultGraph(const std::string& graphName);
    void clearDefaultGraph();
    bool hasGraph(const std::string& graphName);
    catalog::Catalog* getGraphCatalog(const std::string& graphName);
    catalog::Catalog* getDefaultGraphCatalog() const;
    bool hasDefaultGraph() const { return defaultGraph != "" && defaultGraph != "main"; }
    std::string getDefaultGraphName() const { return defaultGraph; }
    std::vector<catalog::Catalog*> getGraphs() const;
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
    std::vector<std::unique_ptr<AttachedDatabase>> attachedDatabases;
    std::string defaultDatabase;
    std::vector<std::unique_ptr<catalog::Catalog>> graphs;
    std::string defaultGraph;
};

} // namespace main
} // namespace lbug
