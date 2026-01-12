#include "main/database_manager.h"

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
#include "storage/shadow_utils.h"
#include "storage/storage_manager.h"
#include "storage/storage_utils.h"
#include "transaction/transaction.h"
#include "transaction/transaction_context.h"
#include <format>

using namespace lbug::common;

namespace lbug {
namespace main {

DatabaseManager::DatabaseManager() : defaultDatabase{""} {}

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
    if (getAttachedDatabase(databaseName) == nullptr) {
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

void DatabaseManager::createGraph(const std::string& graphName,
    storage::MemoryManager* memoryManager, main::ClientContext* clientContext, bool isAnyGraph) {
    auto upperCaseName = StringUtils::getUpper(graphName);
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            throw RuntimeException{std::format("Graph {} already exists.", graphName)};
        }
    }
    auto catalog = std::make_unique<catalog::Catalog>();
    catalog->setCatalogName(graphName);
    auto dbPath = clientContext->getDatabasePath();
    auto graphPath = DBConfig::isDBPathInMemory(dbPath) ?
                         ":" + graphName :
                         storage::StorageUtils::getGraphPath(dbPath, graphName);
    auto storageManager = std::make_unique<storage::StorageManager>(graphPath, false, false,
        *memoryManager, false, common::VirtualFileSystem::GetUnsafe(*clientContext));
    storageManager->initDataFileHandle(common::VirtualFileSystem::GetUnsafe(*clientContext),
        clientContext);
    catalog->setStorageManager(std::move(storageManager));

    if (isAnyGraph) {
        // Use DUMMY_CHECKPOINT_TRANSACTION to create tables
        auto* transaction = &transaction::DUMMY_CHECKPOINT_TRANSACTION;

        // Create serial name for the id column: _nodes_id_serial
        auto serialName = "_nodes_id_serial";
        auto serialLiteral =
            std::make_unique<parser::ParsedLiteralExpression>(Value(serialName), serialName);
        auto serialDefault = std::make_unique<parser::ParsedFunctionExpression>(
            function::NextValFunction::name, std::move(serialLiteral), serialName);

        std::vector<binder::PropertyDefinition> nodeProperties;
        nodeProperties.emplace_back(binder::PropertyDefinition(
            binder::ColumnDefinition("id", common::LogicalType::SERIAL()),
            std::move(serialDefault)));
        nodeProperties.emplace_back(binder::PropertyDefinition(
            binder::ColumnDefinition("label", common::LogicalType::STRING())));
        nodeProperties.emplace_back(
            binder::PropertyDefinition(binder::ColumnDefinition("data", LogicalType::JSON())));

        auto nodeExtraInfo = std::make_unique<binder::BoundExtraCreateNodeTableInfo>("id",
            std::move(nodeProperties), "");
        auto nodeTableInfo =
            binder::BoundCreateTableInfo(catalog::CatalogEntryType::NODE_TABLE_ENTRY, "_nodes",
                common::ConflictAction::ON_CONFLICT_THROW, std::move(nodeExtraInfo), false);
        auto* nodeEntry = catalog->createTableEntry(transaction, nodeTableInfo);
        // Mark entry as committed so it's visible to all transactions
        nodeEntry->setTimestamp(0);
        catalog->getStorageManager()->createTable(nodeEntry->ptrCast<catalog::TableCatalogEntry>());
        auto nodeTableID = nodeEntry->ptrCast<catalog::TableCatalogEntry>()->getTableID();

        std::vector<binder::PropertyDefinition> relProperties;
        relProperties.emplace_back(
            binder::ColumnDefinition("_id", common::LogicalType::INTERNAL_ID()));
        relProperties.emplace_back(
            binder::ColumnDefinition("label", common::LogicalType::STRING()));
        relProperties.emplace_back(binder::ColumnDefinition("data", LogicalType::JSON()));

        std::vector<catalog::NodeTableIDPair> nodePairs;
        nodePairs.emplace_back(catalog::NodeTableIDPair(nodeTableID, nodeTableID));

        auto relExtraInfo = std::unique_ptr<binder::BoundExtraCreateRelTableGroupInfo>(
            new binder::BoundExtraCreateRelTableGroupInfo(std::move(relProperties),
                common::RelMultiplicity::MANY, common::RelMultiplicity::MANY,
                common::ExtendDirection::BOTH, std::move(nodePairs), std::string("")));
        auto relTableInfo = binder::BoundCreateTableInfo(catalog::CatalogEntryType::REL_GROUP_ENTRY,
            "_edges", common::ConflictAction::ON_CONFLICT_THROW, std::move(relExtraInfo), false);
        auto* relEntry = catalog->createTableEntry(transaction, relTableInfo);
        // Mark entry as committed so it's visible to all transactions
        relEntry->setTimestamp(0);
        catalog->getStorageManager()->createTable(relEntry->ptrCast<catalog::TableCatalogEntry>());
    }

    graphs.push_back(std::move(catalog));
    if (defaultGraph == "") {
        defaultGraph = graphName;
    }
}

void DatabaseManager::dropGraph(const std::string& graphName, main::ClientContext* clientContext) {
    auto upperCaseName = StringUtils::getUpper(graphName);
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

            // Delete the physical graph files
            if (!graphPath.empty() && !DBConfig::isDBPathInMemory(graphPath)) {
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
    throw RuntimeException{std::format("No graph named {}.", graphName)};
}

void DatabaseManager::setDefaultGraph(const std::string& graphName) {
    auto upperCaseName = StringUtils::getUpper(graphName);
    if (upperCaseName == "MAIN") {
        defaultGraph = "main";
        return;
    }
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

bool DatabaseManager::hasGraph(const std::string& graphName) {
    auto upperCaseName = StringUtils::getUpper(graphName);
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
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            return graph.get();
        }
    }
    throw BinderException{std::format("No graph named {}.", graphName)};
}

catalog::Catalog* DatabaseManager::getDefaultGraphCatalog() const {
    if (defaultGraph == "" || defaultGraph == "main") {
        return nullptr;
    }
    auto upperCaseName = StringUtils::getUpper(defaultGraph);
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
    for (auto& graph : graphs) {
        result.push_back(graph.get());
    }
    return result;
}

} // namespace main
} // namespace lbug
