#include "main/database_manager.h"

#include "catalog/catalog.h"
#include "common/exception/binder.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "main/client_context.h"
#include "main/database.h"

using namespace lbug::common;

namespace lbug {
namespace main {

DatabaseManager::DatabaseManager() : defaultDatabase{""} {}

void DatabaseManager::registerAttachedDatabase(std::unique_ptr<AttachedDatabase> attachedDatabase) {
    if (defaultDatabase == "") {
        defaultDatabase = attachedDatabase->getDBName();
    }
    if (hasAttachedDatabase(attachedDatabase->getDBName())) {
        throw RuntimeException{stringFormat(
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
    throw RuntimeException{stringFormat("No database named {}.", name)};
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
    throw RuntimeException{stringFormat("Database: {} doesn't exist.", databaseName)};
}

void DatabaseManager::setDefaultDatabase(const std::string& databaseName) {
    if (getAttachedDatabase(databaseName) == nullptr) {
        throw RuntimeException{stringFormat("No database named {}.", databaseName)};
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

void DatabaseManager::createGraph(const std::string& graphName) {
    auto upperCaseName = StringUtils::getUpper(graphName);
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            throw RuntimeException{stringFormat("Graph {} already exists.", graphName)};
        }
    }
    auto catalog = std::make_unique<catalog::Catalog>();
    catalog->setCatalogName(graphName);
    graphs.push_back(std::move(catalog));
    if (defaultGraph == "") {
        defaultGraph = graphName;
    }
}

void DatabaseManager::dropGraph(const std::string& graphName) {
    auto upperCaseName = StringUtils::getUpper(graphName);
    for (auto it = graphs.begin(); it != graphs.end(); ++it) {
        auto graphNameUpper = StringUtils::getUpper((*it)->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            // Check if this is the default graph
            if (defaultGraph != "" && StringUtils::getUpper(defaultGraph) == upperCaseName) {
                defaultGraph = "";
            }
            graphs.erase(it);
            return;
        }
    }
    throw RuntimeException{stringFormat("No graph named {}.", graphName)};
}

void DatabaseManager::setDefaultGraph(const std::string& graphName) {
    auto upperCaseName = StringUtils::getUpper(graphName);
    if (upperCaseName == "LOCAL") {
        defaultGraph = "local";
        return;
    }
    for (auto& graph : graphs) {
        auto graphNameUpper = StringUtils::getUpper(graph->getCatalogName());
        if (graphNameUpper == upperCaseName) {
            defaultGraph = graphName;
            return;
        }
    }
    throw BinderException{stringFormat("No graph named {}.", graphName)};
}

void DatabaseManager::clearDefaultGraph() {
    defaultGraph = "local";
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
    throw BinderException{stringFormat("No graph named {}.", graphName)};
}

catalog::Catalog* DatabaseManager::getDefaultGraphCatalog() const {
    if (defaultGraph == "") {
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

std::vector<catalog::Catalog*> DatabaseManager::getGraphs() const {
    std::vector<catalog::Catalog*> result;
    for (auto& graph : graphs) {
        result.push_back(graph.get());
    }
    return result;
}

} // namespace main
} // namespace lbug
