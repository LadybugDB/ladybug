#include "processor/operator/ddl/create_index.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "common/exception/binder.h"
#include "processor/execution_context.h"
#include "storage/index/hash_index.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include <format>

using namespace lbug::catalog;
using namespace lbug::common;

namespace lbug {
namespace processor {

void CreateIndex::executeInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    auto catalog = Catalog::Get(*clientContext);
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto memoryManager = storage::MemoryManager::Get(*clientContext);
    auto storageManager = storage::StorageManager::Get(*clientContext);
    const auto indexExists = catalog->containsIndex(transaction, info.tableID, info.indexName) ||
                             catalog->containsIndex(transaction, info.tableID, info.propertyID);
    auto* table = storageManager->getTable(info.tableID)->ptrCast<storage::NodeTable>();
    const auto storageIndexExists =
        table->getIndex(info.indexName).has_value() || table->tryGetPKIndex() != nullptr;
    if (indexExists || storageIndexExists) {
        switch (info.onConflict) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(std::format("Index {} already exists.", info.indexName), memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(info.indexName + " already exists in catalog.");
        }
        default:
            UNREACHABLE_CODE;
        }
    }
    auto indexType = storage::PrimaryKeyIndex::getIndexType();
    storage::IndexInfo indexInfo{info.indexName, indexType.typeName, info.tableID, {info.columnID},
        {info.keyDataType}, indexType.constraintType == storage::IndexConstraintType::PRIMARY,
        indexType.definitionType == storage::IndexDefinitionType::BUILTIN};
    auto index = storage::PrimaryKeyIndex::createNewIndex(std::move(indexInfo),
        storageManager->isInMemory(), *memoryManager,
        *storageManager->getDataFH()->getPageManager(), &storageManager->getShadowFile());
    table->buildIndexAndAdd(clientContext, std::move(index));
    catalog->createIndex(transaction,
        std::make_unique<IndexCatalogEntry>(info.indexType, info.tableID, info.indexName,
            std::vector<property_id_t>{info.propertyID}, std::make_unique<BuiltinIndexAuxInfo>()));
    appendMessage(std::format("Index {} has been created.", info.indexName), memoryManager);
}

} // namespace processor
} // namespace lbug
