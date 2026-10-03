#include "main/database_manager.h"

#include <unordered_set>

#include "binder/ddl/bound_create_table_info.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "common/serializer/in_mem_file_writer.h"
#include "common/serializer/serializer.h"
#include "common/string_utils.h"
#include "common/types/types.h"
#include "function/sequence/sequence_functions.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/db_config.h"
#include "parser/expression/parsed_function_expression.h"
#include "parser/expression/parsed_literal_expression.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/checkpointer.h"
#include "storage/database_header.h"
#include "storage/partition_storage_registry.h"
#include "storage/shadow_utils.h"
#include "storage/storage_manager.h"
#include "storage/storage_utils.h"
#include "storage/wal/wal_replayer.h"
#include "transaction/transaction.h"
#include "transaction/transaction_context.h"
#include <format>

using namespace lbug::transaction;

using namespace lbug::common;

namespace lbug {
namespace main {

DatabaseManager::DatabaseManager() : defaultDatabase{""} {}

DatabaseManager::~DatabaseManager() = default;

// A graph materialized during an active recovery transaction cannot replay its own WAL
// inline: graph WALs hold standalone-session transactions whose BEGIN/COMMIT would nest
// inside the caller's recovery transaction. The loader queues one of these and
// replayPendingGraphWALs() runs the queued replays at the next transaction-free point.
struct GraphWALReplayRequest {
    storage::StorageManager* storageManager = nullptr;
    std::string graphName;
    storage::WALReplayer::GraphRecoveryState recoveryState;
};

void DatabaseManager::registerAttachedDatabase(std::unique_ptr<AttachedDatabase> attachedDatabase) {
    if (defaultDatabase == "") {
        defaultDatabase = attachedDatabase->getDBName();
    }
    if (hasAttachedDatabase(attachedDatabase->getDBName())) {
        throw RuntimeException{std::format(
            "Duplicate attached database name: {}. Attached database name must be unique.",
            attachedDatabase->getDBName())};
    }
    attachedDatabases.push_back(std::move(attachedDatabase));
}

bool DatabaseManager::hasAttachedDatabase(const std::string& name) {
    auto upperCaseName = StringUtils::getUpper(name);
    for (auto& attachedDatabase : attachedDatabases) {
        auto attachedDBName = StringUtils::getUpper(attachedDatabase->getDBName());
        if (attachedDBName == upperCaseName) {
            return true;
        }
    }
    return false;
}

AttachedDatabase* DatabaseManager::getAttachedDatabase(const std::string& name) {
    auto upperCaseName = StringUtils::getUpper(name);
    for (auto& attachedDatabase : attachedDatabases) {
        auto attachedDBName = StringUtils::getUpper(attachedDatabase->getDBName());
        if (attachedDBName == upperCaseName) {
            return attachedDatabase.get();
        }
    }
    throw RuntimeException{std::format("No database named {}.", name)};
}

void DatabaseManager::detachDatabase(const std::string& databaseName) {
    auto upperCaseName = StringUtils::getUpper(databaseName);
    for (auto it = attachedDatabases.begin(); it != attachedDatabases.end(); ++it) {
        auto attachedDBName = (*it)->getDBName();
        StringUtils::toUpper(attachedDBName);
        if (attachedDBName == upperCaseName) {
            attachedDatabases.erase(it);
            return;
        }
    }
    throw RuntimeException{std::format("Database: {} doesn't exist.", databaseName)};
}

void DatabaseManager::setDefaultDatabase(const std::string& databaseName) {
    if (!hasAttachedDatabase(databaseName)) {
        throw RuntimeException{std::format("No database named {}.", databaseName)};
    }
    defaultDatabase = databaseName;
}

std::vector<AttachedDatabase*> DatabaseManager::getAttachedDatabases() const {
    std::vector<AttachedDatabase*> attachedDatabasesPtr;
    for (auto& attachedDatabase : attachedDatabases) {
        attachedDatabasesPtr.push_back(attachedDatabase.get());
    }
    return attachedDatabasesPtr;
}

void DatabaseManager::invalidateCache() {
    for (auto& attachedDatabase : attachedDatabases) {
        attachedDatabase->invalidateCache();
    }
}

DatabaseManager* DatabaseManager::Get(const ClientContext& context) {
    return context.getDatabase()->getDatabaseManager();
}

static void createAnyGraphTables(catalog::Catalog& catalog) {
    // Use DUMMY_CHECKPOINT_TRANSACTION to create tables
    auto* dummyTransaction = &transaction::DUMMY_CHECKPOINT_TRANSACTION;

    // Create serial name for the id column: _nodes_id_serial
    auto serialName = "_nodes_id_serial";
    auto serialLiteral =
        std::make_unique<parser::ParsedLiteralExpression>(Value(serialName), serialName);
    auto serialDefault = std::make_unique<parser::ParsedFunctionExpression>(
        function::NextValFunction::name, std::move(serialLiteral), serialName);

    std::vector<binder::PropertyDefinition> nodeProperties;
    nodeProperties.emplace_back(binder::PropertyDefinition(
        binder::ColumnDefinition("id", common::LogicalType::SERIAL()), std::move(serialDefault)));
    nodeProperties.emplace_back(binder::PropertyDefinition(binder::ColumnDefinition("label",
        common::LogicalType::LIST(common::LogicalType::STRING()))));
    nodeProperties.emplace_back(
        binder::PropertyDefinition(binder::ColumnDefinition("data", LogicalType::JSON())));

    auto nodeExtraInfo = std::make_unique<binder::BoundExtraCreateNodeTableInfo>("id",
        std::move(nodeProperties), "");
    auto nodeTableInfo = binder::BoundCreateTableInfo(catalog::CatalogEntryType::NODE_TABLE_ENTRY,
        "_nodes", common::ConflictAction::ON_CONFLICT_THROW, std::move(nodeExtraInfo), false);
    auto* nodeEntry = catalog.createTableEntry(dummyTransaction, nodeTableInfo);
    // Mark entry as committed so it's visible to all transactions
    nodeEntry->setTimestamp(0);
    catalog.getStorageManager()->createTable(nodeEntry->ptrCast<catalog::TableCatalogEntry>());
    auto nodeTableID = nodeEntry->ptrCast<catalog::TableCatalogEntry>()->getTableID();

    std::vector<binder::PropertyDefinition> relProperties;
    relProperties.emplace_back(binder::ColumnDefinition("_id", common::LogicalType::INTERNAL_ID()));
    relProperties.emplace_back(binder::ColumnDefinition("label", common::LogicalType::STRING()));
    relProperties.emplace_back(binder::ColumnDefinition("data", LogicalType::JSON()));

    std::vector<binder::BoundRelTableInfo> relTableInfos;
    relTableInfos.emplace_back(catalog::NodeTableIDPair(nodeTableID, nodeTableID),
        common::RelMultiplicity::MANY, common::RelMultiplicity::MANY);

    auto relExtraInfo = std::unique_ptr<binder::BoundExtraCreateRelTableGroupInfo>(
        new binder::BoundExtraCreateRelTableGroupInfo(std::move(relProperties),
            common::RelMultiplicity::MANY, common::RelMultiplicity::MANY,
            common::ExtendDirection::BOTH, std::move(relTableInfos), std::string("")));
    auto relTableInfo = binder::BoundCreateTableInfo(catalog::CatalogEntryType::REL_GROUP_ENTRY,
        "_edges", common::ConflictAction::ON_CONFLICT_THROW, std::move(relExtraInfo), false);
    auto* relEntry = catalog.createTableEntry(dummyTransaction, relTableInfo);
    // Mark entry as committed so it's visible to all transactions
    relEntry->setTimestamp(0);
    catalog.getStorageManager()->createTable(relEntry->ptrCast<catalog::TableCatalogEntry>());
}

void DatabaseManager::createGraph(const std::string& graphName,
    storage::MemoryManager* memoryManager, main::ClientContext* clientContext, bool isAnyGraph) {
    if (StringUtils::caseInsensitiveEquals(graphName, "main")) {
        throw RuntimeException{"MAIN is a reserved graph name."};
    }

    auto upperCaseName = StringUtils::getUpper(graphName);

    // Check if graph already exists in system catalog
    auto mainCatalog = clientContext->getDatabase()->getCatalog();
    auto transaction = TransactionContext::Get(*clientContext)->getActiveTransaction();
    if (mainCatalog->containsGraph(transaction, graphName)) {
        throw RuntimeException{std::format("Graph {} already exists.", graphName)};
    }

    // Also check in-memory graphs vector (for any edge cases)
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            throw RuntimeException{std::format("Graph {} already exists.", graphName)};
        }
    }

    // Add to system catalog first (for transactional durability)
    mainCatalog->createGraph(transaction, graphName, isAnyGraph);

    auto catalog = std::make_unique<catalog::Catalog>();
    catalog->setCatalogName(graphName);
    // Extension functions are registered in the main catalog only; let function
    // lookup fall back to it while the session is on this graph.
    catalog->setFunctionFallback(mainCatalog);
    auto dbPath = clientContext->getDatabasePath();
    auto graphPath = DBConfig::isDBPathInMemory(dbPath) ?
                         ":" + graphName :
                         storage::StorageUtils::getGraphPath(dbPath, graphName);
    auto storageManager = std::make_unique<storage::StorageManager>(graphPath, false, false,
        *memoryManager, false, clientContext->getDBConfig()->enableDefaultHashIndex,
        common::VirtualFileSystem::GetUnsafe(*clientContext));
    storageManager->initDataFileHandle(common::VirtualFileSystem::GetUnsafe(*clientContext),
        clientContext);
    catalog->setStorageManager(std::move(storageManager));

    if (isAnyGraph) {
        createAnyGraphTables(*catalog);
    }

    {
        std::unique_lock lck{graphsMutex};
        graphs.push_back(std::move(catalog));
    }
    // NOTE: Do NOT set defaultGraph here. Setting defaultGraph before the transaction
    // commits causes Catalog::Get() in Transaction::publishCommit() to return the graph's
    // catalog instead of the main catalog, so the main catalog's version is never
    // incremented. Without the version bump, the CHECKPOINT path skips serializing the
    // main catalog (because changedSinceLastCheckpoint() is false), and the graph catalog
    // entry in the main catalog's `graphs` set is lost on reopen.
    // Users must explicitly USE GRAPH to work in the graph.
}

void DatabaseManager::dropGraph(const std::string& graphName, main::ClientContext* clientContext) {
    if (StringUtils::caseInsensitiveEquals(graphName, "main")) {
        throw BinderException{"Cannot drop the main graph."};
    }

    auto upperCaseName = StringUtils::getUpper(graphName);

    // Check if graph exists in system catalog first
    auto mainCatalog = clientContext->getDatabase()->getCatalog();
    auto transaction = TransactionContext::Get(*clientContext)->getActiveTransaction();
    if (!mainCatalog->containsGraph(transaction, graphName)) {
        throw RuntimeException{std::format("No graph named {}.", graphName)};
    }
    // A node-table subgraph is owned by its table and dropped with it. Refusing a standalone
    // DROP GRAPH keeps the subgraph-per-node-table invariant intact.
    if (mainCatalog->containsTable(transaction, graphName)) {
        throw RuntimeException{std::format(
            "Cannot drop graph {}: it is a node-table subgraph. Drop the node table instead.",
            graphName)};
    }

    // Remove from system catalog
    mainCatalog->dropGraph(transaction, graphName);

    std::unique_lock lck{graphsMutex};
    for (auto it = graphs.begin(); it != graphs.end(); ++it) {
        auto graphNameUpper = StringUtils::getUpper((*it)->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            if (defaultGraph != "" && StringUtils::getUpper(defaultGraph) == upperCaseName) {
                defaultGraph = "";
            }
            auto storageManager = (*it)->getStorageManager();
            std::string graphPath;
            if (storageManager != nullptr) {
                graphPath = storageManager->getDatabasePath();
                storageManager->closeFileHandle();
            }
            if (hasAttachedDatabase(graphName)) {
                detachDatabase(graphName);
            }
            graphs.erase(it);
            lck.unlock();

            // Delete the physical graph files
            if (!graphPath.empty() &&
                !DBConfig::isDBPathInMemory(clientContext->getDatabasePath())) {
                auto vfs = common::VirtualFileSystem::GetUnsafe(*clientContext);
                vfs->removeFileIfExists(graphPath, clientContext);
                vfs->removeFileIfExists(storage::StorageUtils::getWALFilePath(graphPath),
                    clientContext);
                vfs->removeFileIfExists(storage::StorageUtils::getShadowFilePath(graphPath),
                    clientContext);
                vfs->removeFileIfExists(storage::StorageUtils::getTmpFilePath(graphPath),
                    clientContext);
            }

            auto dbStorageManager = clientContext->getDatabase()->getStorageManager();
            auto databaseHeader = dbStorageManager->getOrInitDatabaseHeader(*clientContext);
            auto newHeader = std::make_unique<storage::DatabaseHeader>(*databaseHeader);
            newHeader->catalogPageRange.startPageIdx = common::INVALID_PAGE_IDX;
            newHeader->catalogPageRange.numPages = 0;
            newHeader->metadataPageRange.startPageIdx = common::INVALID_PAGE_IDX;
            newHeader->metadataPageRange.numPages = 0;
            dbStorageManager->setDatabaseHeader(std::move(newHeader));
            return;
        }
    }
}

void DatabaseManager::setDefaultGraph(const std::string& graphName) {
    auto upperCaseName = StringUtils::getUpper(graphName);
    if (upperCaseName == "MAIN") {
        defaultGraph = "main";
        return;
    }
    std::shared_lock lck{graphsMutex};
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            defaultGraph = graphName;
            return;
        }
    }
    throw BinderException{std::format("No graph named {}.", graphName)};
}

void DatabaseManager::clearDefaultGraph() {
    defaultGraph = "main";
}

void DatabaseManager::loadGraphsFromCatalog(storage::MemoryManager* memoryManager,
    main::ClientContext* clientContext) {
    loadGraphsFromCatalog(memoryManager, clientContext, false, false);
}

void DatabaseManager::loadGraphsFromCatalog(storage::MemoryManager* memoryManager,
    main::ClientContext* clientContext, bool mainCheckpointCommitted, bool checkpointBundle,
    std::optional<common::uuid> legacyCheckpointDatabaseID) {
    auto mainCatalog = clientContext->getDatabase()->getCatalog();
    // During WAL replay, a tagged record can trigger materialization of a graph whose
    // create-graph record is part of the still-uncommitted recovery transaction; enumerate
    // with that transaction so such a graph is visible to the loader. Every replayed BEGIN is
    // guaranteed to reach its COMMIT record (dryReplay advances the replay bound only past
    // COMMIT records), so a graph visible here can never belong to an aborted transaction.
    // Outside replay there is no active transaction and DUMMY_CHECKPOINT_TRANSACTION applies.
    auto* transaction = TransactionContext::Get(*clientContext)->hasActiveTransaction() ?
                            Transaction::Get(*clientContext) :
                            &transaction::DUMMY_CHECKPOINT_TRANSACTION;
    auto graphEntries = mainCatalog->getGraphEntries(transaction);
    std::unordered_set<std::string> loadedGraphNames;
    {
        std::shared_lock lck{graphsMutex};
        loadedGraphNames.reserve(graphs.size());
        for (const auto& graph : graphs) {
            loadedGraphNames.insert(StringUtils::getUpper(graph->getCatalogName()));
        }
    }

    for (auto* graphEntry : graphEntries) {
        auto graphName = graphEntry->getName();
        // Partition subgraphs are registered as graphs in the catalog (so show_graphs lists
        // them and queries can address them), but their data files are owned by the partition
        // storage registry through the main StorageManager. Loading them here would open a
        // second StorageManager on the same file and turn them into checkpoint targets whose
        // empty catalogs treat every live partition as orphaned (deleting its data on close).
        if (mainCatalog->containsTable(transaction, graphName)) {
            continue;
        }

        auto upperCaseName = StringUtils::getUpper(graphName);
        if (loadedGraphNames.contains(upperCaseName)) {
            continue;
        }

        if (loadGraph(clientContext, memoryManager, graphEntry, mainCheckpointCommitted,
                checkpointBundle, legacyCheckpointDatabaseID)) {
            loadedGraphNames.insert(std::move(upperCaseName));
        }
    }
}

bool DatabaseManager::loadGraph(main::ClientContext* clientContext,
    storage::MemoryManager* memoryManager, catalog::GraphCatalogEntry* graphEntry,
    bool mainCheckpointCommitted, bool checkpointBundle,
    std::optional<common::uuid> legacyCheckpointDatabaseID) {
    auto graphName = graphEntry->getName();
    auto catalog = std::make_unique<catalog::Catalog>();
    catalog->setCatalogName(graphName);
    // Extension functions are registered in the main catalog only; let
    // function lookup fall back to it while the session is on this graph.
    auto* mainCatalog = clientContext->getDatabase()->getCatalog();
    catalog->setFunctionFallback(mainCatalog);
    auto dbPath = clientContext->getDatabasePath();
    auto graphPath = DBConfig::isDBPathInMemory(dbPath) ?
                         ":" + graphName :
                         storage::StorageUtils::getGraphPath(dbPath, graphName);

    // Check if graph file exists before trying to load
    auto vfs = common::VirtualFileSystem::GetUnsafe(*clientContext);
    if (!DBConfig::isDBPathInMemory(dbPath) && !vfs->fileOrPathExists(graphPath)) {
        if (mainCheckpointCommitted &&
            vfs->fileOrPathExists(storage::StorageUtils::getShadowFilePath(graphPath))) {
            throw RuntimeException(std::format(
                "Cannot recover committed checkpoint: graph file {} is missing.", graphPath));
        }
        // Graph file doesn't exist, skip this graph
        return false;
    }

    auto storageManager = std::make_unique<storage::StorageManager>(graphPath,
        clientContext->getDBConfig()->readOnly, false, *memoryManager, false,
        clientContext->getDBConfig()->enableDefaultHashIndex, vfs);
    storageManager->initDataFileHandle(vfs, clientContext);
    storage::WALReplayer walReplayer{*clientContext};
    auto recoveryState = walReplayer.prepareGraphCheckpoint(*storageManager, checkpointBundle,
        legacyCheckpointDatabaseID);
    bool hasPersistedCatalog = false;
    if (storageManager->getDataFH()->getNumPages() > 0) {
        auto persistedHeader = storage::DatabaseHeader::readDatabaseHeader(
            *storageManager->getDataFH()->getFileInfo());
        hasPersistedCatalog =
            persistedHeader.has_value() &&
            persistedHeader->catalogPageRange.startPageIdx != common::INVALID_PAGE_IDX;
        storage::Checkpointer::readCheckpoint(clientContext, catalog.get(), storageManager.get());
    }
    catalog->setStorageManager(std::move(storageManager));
    if (graphEntry->isAnyGraphType() && !hasPersistedCatalog) {
        // A graph whose create-graph record replayed from the WAL (never checkpointed)
        // materializes without the ANY-graph infrastructure that createGraph built in
        // memory: it was not WAL-logged. Recreate it so the catalog's ID space matches
        // logging time and ANY-graph label queries keep working. An empty table set in
        // a persisted snapshot is deliberate state, not missing initialization.
        createAnyGraphTables(*catalog);
    }
    auto* graphStorageManager = catalog->getStorageManager();
    {
        std::unique_lock lck{graphsMutex};
        graphs.push_back(std::move(catalog));
    }

    if (TransactionContext::Get(*clientContext)->hasActiveTransaction()) {
        // Lazy materialization runs inside the caller's recovery transaction, and a
        // graph WAL holds standalone-session transactions: replaying it here would
        // begin a second recovery transaction nested inside the caller's. Queue the
        // replay for the next transaction-free point (replayPendingGraphWALs) instead.
        pendingGraphWALReplays.push_back(std::make_unique<GraphWALReplayRequest>(
            GraphWALReplayRequest{graphStorageManager, graphName, std::move(recoveryState)}));
        return true;
    }

    const auto previousDefaultGraph = defaultGraph;
    defaultGraph = graphName;
    try {
        walReplayer.replayGraphWAL(*graphStorageManager, recoveryState);
    } catch (...) {
        defaultGraph = previousDefaultGraph;
        throw;
    }
    defaultGraph = previousDefaultGraph;
    walReplayer.retireGraphCheckpointWALs(*graphStorageManager, recoveryState);
    if (!mainCheckpointCommitted) {
        walReplayer.removeGraphCheckpointShadow(*graphStorageManager);
    }
    // NOTE: defaultGraph is only set around replayGraphWAL above (see createGraph for
    // the rationale against leaving it set). Users must explicitly USE GRAPH to work
    // in the graph.
    return true;
}

bool DatabaseManager::loadGraphFromCatalog(storage::MemoryManager* memoryManager,
    main::ClientContext* clientContext, const std::string& graphName) {
    auto mainCatalog = clientContext->getDatabase()->getCatalog();
    // Lazy materialization is only requested from inside WAL replay, where the active
    // recovery transaction must see graphs whose create-graph record already replayed
    // (see loadGraphsFromCatalog for the same visibility argument).
    auto* transaction = TransactionContext::Get(*clientContext)->hasActiveTransaction() ?
                            Transaction::Get(*clientContext) :
                            &transaction::DUMMY_CHECKPOINT_TRANSACTION;
    if (!mainCatalog->containsGraph(transaction, graphName)) {
        return false;
    }
    // A partition subgraph's data files are owned by the partition storage registry and
    // must not be opened here (see the loadGraphsFromCatalog loop).
    if (mainCatalog->containsTable(transaction, graphName)) {
        return false;
    }
    if (hasGraph(graphName)) {
        return true;
    }
    auto* graphEntry = mainCatalog->getGraphEntry(transaction, graphName);
    return loadGraph(clientContext, memoryManager, graphEntry, false /* mainCheckpointCommitted */,
        false /* checkpointBundle */, std::nullopt);
}

void DatabaseManager::replayPendingGraphWALs(main::ClientContext* clientContext) {
    if (pendingGraphWALReplays.empty()) {
        return;
    }
    auto pendingReplays = std::move(pendingGraphWALReplays);
    pendingGraphWALReplays.clear();
    storage::WALReplayer walReplayer{*clientContext};
    for (auto& replayRequest : pendingReplays) {
        const auto previousDefaultGraph = defaultGraph;
        defaultGraph = replayRequest->graphName;
        try {
            walReplayer.replayGraphWAL(*replayRequest->storageManager,
                replayRequest->recoveryState);
        } catch (...) {
            defaultGraph = previousDefaultGraph;
            throw;
        }
        defaultGraph = previousDefaultGraph;
        walReplayer.retireGraphCheckpointWALs(*replayRequest->storageManager,
            replayRequest->recoveryState);
        // Deferred requests only come from lazy materialization during main-WAL replay,
        // which always runs with mainCheckpointCommitted=false.
        walReplayer.removeGraphCheckpointShadow(*replayRequest->storageManager);
    }
}

bool DatabaseManager::hasGraph(const std::string& graphName) {
    auto upperCaseName = StringUtils::getUpper(graphName);
    std::shared_lock lck{graphsMutex};
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            return true;
        }
    }
    return false;
}

catalog::Catalog* DatabaseManager::getGraphCatalog(const std::string& graphName) {
    auto upperCaseName = StringUtils::getUpper(graphName);
    std::shared_lock lck{graphsMutex};
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            return graph.get();
        }
    }
    throw BinderException{std::format("No graph named {}.", graphName)};
}

void DatabaseManager::withGraphCatalog(const std::string& graphName,
    const std::function<void(catalog::Catalog*)>& action) {
    auto upperCaseName = StringUtils::getUpper(graphName);
    std::shared_lock lck{graphsMutex};
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            action(graph.get());
            return;
        }
    }
    throw BinderException{std::format("No graph named {}.", graphName)};
}

catalog::Catalog* DatabaseManager::getDefaultGraphCatalog() const {
    if (defaultGraph == "" || defaultGraph == "main") {
        return nullptr;
    }
    auto upperCaseName = StringUtils::getUpper(defaultGraph);
    std::shared_lock lck{graphsMutex};
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            return graph.get();
        }
    }
    return nullptr;
}

storage::StorageManager* DatabaseManager::getDefaultGraphStorageManager() const {
    auto graphCatalog = getDefaultGraphCatalog();
    if (graphCatalog != nullptr) {
        return graphCatalog->getStorageManager();
    }
    return nullptr;
}

std::vector<catalog::Catalog*> DatabaseManager::getGraphs() const {
    std::vector<catalog::Catalog*> result;
    std::shared_lock lck{graphsMutex};
    for (auto& graph : graphs) {
        result.push_back(graph.get());
    }
    return result;
}

void DatabaseManager::bumpGraphCatalogVersions(
    const std::unordered_set<catalog::Catalog*>& catalogs) {
    std::shared_lock lck{graphsMutex};
    for (auto& graph : graphs) {
        if (catalogs.contains(graph.get())) {
            graph->incrementVersion();
        }
    }
}

std::pair<catalog::Catalog*, storage::StorageManager*> DatabaseManager::resolveTableStorage(
    const ClientContext& context, common::table_id_t tableID, const std::string& dbName) {
    if (!dbName.empty()) {
        auto* attachedDB = DatabaseManager::Get(context)->getAttachedDatabase(dbName);
        if (attachedDB->getDBType() == common::ATTACHED_LBUG_DB_TYPE) {
            auto* attachedLbug = static_cast<main::AttachedLbugDatabase*>(attachedDB);
            if (attachedLbug->getStorageManager()->containsTable(tableID)) {
                return {attachedDB->getCatalog(), attachedLbug->getStorageManager()};
            }
            throw common::RuntimeException(
                std::format("Table with ID {} not found in database {}.", tableID, dbName));
        }
        // Foreign attached databases (duckdb, sqlite, postgres) register their
        // tables in the main catalog/storage manager (shadow entries guarantee
        // table ID uniqueness); fall through to the main-path resolution.
    }
    auto* mainSM = storage::StorageManager::Get(context);
    if (mainSM->containsTable(tableID)) {
        return {catalog::Catalog::Get(context), mainSM};
    }
    // Partition children live in their own data files (phase-B per-partition storage);
    // lazily open the child's file from its catalog entry on first touch after reopen.
    {
        auto& registry = *Get(context)->getPartitionStorageRegistry();
        if (registry.tryGet(tableID) != nullptr ||
            catalog::Catalog::Get(context)->containsTable(transaction::Transaction::Get(context),
                tableID)) {
            auto* entry = catalog::Catalog::Get(context)->getTableCatalogEntry(
                transaction::Transaction::Get(context), tableID);
            auto& sm = registry.getOrCreate(const_cast<ClientContext*>(&context), tableID,
                entry->getName());
            if (!sm.containsTable(tableID) && !sm.isReadOnly()) {
                sm.createTable(entry, const_cast<ClientContext*>(&context));
            }
            return {catalog::Catalog::Get(context), &sm};
        }
    }
    auto* dbManager = DatabaseManager::Get(context);
    for (auto* attachedDB : dbManager->getAttachedDatabases()) {
        if (attachedDB->getDBType() == common::ATTACHED_LBUG_DB_TYPE) {
            auto* attachedLbug = static_cast<main::AttachedLbugDatabase*>(attachedDB);
            if (attachedLbug->getStorageManager()->containsTable(tableID)) {
                return {attachedDB->getCatalog(), attachedLbug->getStorageManager()};
            }
        }
    }
    throw common::RuntimeException(
        std::format("Table with ID {} not found in any attached database.", tableID));
}

} // namespace main
} // namespace lbug
