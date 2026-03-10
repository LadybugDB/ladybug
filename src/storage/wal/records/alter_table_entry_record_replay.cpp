#include "binder/binder.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/catalog_entry.h"
#include "processor/expression_mapper.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"
#include "storage/table/table.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::binder;
using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayAlterTableEntryRecord(const WALRecord& walRecord) const {
    auto binder = Binder(&clientContext);
    auto& alterEntryRecord = walRecord.constCast<AlterTableEntryRecord>();
    auto catalog = Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    auto storageManager = StorageManager::Get(clientContext);
    auto ownedAlterInfo = alterEntryRecord.ownedAlterInfo.get();
    catalog->alterTableEntry(transaction, *ownedAlterInfo);
    auto& pageAllocator = *PageManager::Get(clientContext);
    switch (ownedAlterInfo->alterType) {
    case AlterType::ADD_PROPERTY: {
        const auto exprBinder = binder.getExpressionBinder();
        const auto addInfo = ownedAlterInfo->extraInfo->constPtrCast<BoundExtraAddPropertyInfo>();
        const auto boundDefault =
            exprBinder->bindExpression(*addInfo->propertyDefinition.defaultExpr);
        auto exprMapper = ExpressionMapper();
        const auto defaultValueEvaluator = exprMapper.getEvaluator(boundDefault);
        defaultValueEvaluator->init(ResultSet(0) /* dummy ResultSet */, &clientContext);
        const auto entry = catalog->getTableCatalogEntry(transaction, ownedAlterInfo->tableName);
        const auto& addedProp = entry->getProperty(addInfo->propertyDefinition.getName());
        TableAddColumnState state{addedProp, *defaultValueEvaluator};
        DASSERT(StorageManager::Get(clientContext));
        switch (entry->getTableType()) {
        case TableType::REL: {
            for (auto& relEntryInfo : entry->cast<RelGroupCatalogEntry>().getRelEntryInfos()) {
                storageManager->getTable(relEntryInfo.oid)
                    ->addColumn(transaction, state, pageAllocator);
            }
        } break;
        case TableType::NODE: {
            storageManager->getTable(entry->getTableID())
                ->addColumn(transaction, state, pageAllocator);
        } break;
        default: {
            UNREACHABLE_CODE;
        }
        }
    } break;
    case AlterType::ADD_FROM_TO_CONNECTION: {
        auto extraInfo = ownedAlterInfo->extraInfo->constPtrCast<BoundExtraAlterFromToConnection>();
        auto relGroupEntry = catalog->getTableCatalogEntry(transaction, ownedAlterInfo->tableName)
                                 ->ptrCast<RelGroupCatalogEntry>();
        auto relEntryInfo =
            relGroupEntry->getRelEntryInfo(extraInfo->fromTableID, extraInfo->toTableID);
        storageManager->addRelTable(relGroupEntry, *relEntryInfo);
    } break;
    default:
        break;
    }
}

} // namespace storage
} // namespace lbug
