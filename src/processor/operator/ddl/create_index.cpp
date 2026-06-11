#include "processor/operator/ddl/create_index.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "processor/execution_context.h"
#include "storage/index/art_index.h"
#include "storage/index/hash_index.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include <format>

using namespace lbug::catalog;
using namespace lbug::common;

namespace lbug {
namespace processor {

static std::string getExistingIndexName(Catalog* catalog, transaction::Transaction* transaction,
    common::table_id_t tableID, common::property_id_t propertyID) {
    for (auto* indexEntry : catalog->getIndexEntries(transaction, tableID)) {
        if (indexEntry->containsPropertyID(propertyID)) {
            return indexEntry->getIndexName();
        }
    }
    return "";
}

void CreateIndex::executeInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    auto catalog = Catalog::Get(*clientContext);
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto memoryManager = storage::MemoryManager::Get(*clientContext);
    auto storageManager = storage::StorageManager::Get(*clientContext);
    const auto indexNameExists = catalog->containsIndex(transaction, info.tableID, info.indexName);
    const auto indexedPropertyExists =
        catalog->containsIndex(transaction, info.tableID, info.propertyID);
    if (indexNameExists || indexedPropertyExists) {
        const auto existingIndexName = indexNameExists ? info.indexName :
                                                         getExistingIndexName(catalog, transaction,
                                                             info.tableID, info.propertyID);
        switch (info.onConflict) {
        case ConflictAction::ON_CONFLICT_DO_NOTHING: {
            appendMessage(std::format("Index {} already exists.", existingIndexName),
                memoryManager);
            return;
        }
        case ConflictAction::ON_CONFLICT_THROW: {
            throw BinderException(existingIndexName + " already exists in catalog.");
        }
        default:
            UNREACHABLE_CODE;
        }
    }
    auto* table = storageManager->getTable(info.tableID)->ptrCast<storage::NodeTable>();
    const auto storageIndexNameExists = table->getIndex(info.indexName).has_value();
    auto storagePKIndexExists = table->tryGetPrimaryKeyIndex() != nullptr;
    const auto canCreatePhysicalIndex =
        !storageIndexNameExists && (!info.isPrimary || !storagePKIndexExists);
    auto indexTypeOptional = storageManager->getIndexType(info.indexType);
    if (!indexTypeOptional.has_value()) {
        throw BinderException(std::format("Index type {} does not exist.", info.indexType));
    }
    const auto& indexType = indexTypeOptional.value().get();
    if (info.isPrimary && storagePKIndexExists &&
        table->tryGetPrimaryKeyIndex()->getIndexInfo().indexType != indexType.typeName) {
        throw BinderException(std::format(
            "Cannot create {} index because the table already has a {} primary-key index.",
            indexType.typeName, table->tryGetPrimaryKeyIndex()->getIndexInfo().indexType));
    }
    if (canCreatePhysicalIndex) {
        storage::IndexInfo indexInfo{info.indexName, indexType.typeName, info.tableID,
            {info.columnID}, {info.keyDataType}, info.isPrimary,
            indexType.definitionType == storage::IndexDefinitionType::BUILTIN};
        std::unique_ptr<storage::Index> index;
        if (StringUtils::caseInsensitiveEquals(indexType.typeName,
                storage::PrimaryKeyIndex::getIndexType().typeName)) {
            index = storage::PrimaryKeyIndex::createNewIndex(std::move(indexInfo),
                storageManager->isInMemory(), *memoryManager,
                *storageManager->getDataFH()->getPageManager(), &storageManager->getShadowFile());
        } else if (StringUtils::caseInsensitiveEquals(indexType.typeName,
                       storage::ArtPrimaryKeyIndex::getIndexType().typeName)) {
            index = storage::ArtPrimaryKeyIndex::createNewIndex(std::move(indexInfo));
        } else {
            throw BinderException(
                std::format("Index type {} is not supported by CREATE INDEX.", indexType.typeName));
        }
        table->buildIndexAndAdd(clientContext, std::move(index));
        storagePKIndexExists = storagePKIndexExists || info.isPrimary;
    }
    if (!storageIndexNameExists) {
        catalog->createIndex(transaction,
            std::make_unique<IndexCatalogEntry>(indexType.typeName, info.tableID, info.indexName,
                std::vector<property_id_t>{info.propertyID},
                std::make_unique<BuiltinIndexAuxInfo>()));
        appendMessage(std::format("Index {} has been created.", info.indexName), memoryManager);
        return;
    }
    if (storageIndexNameExists) {
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
}

} // namespace processor
} // namespace lbug
