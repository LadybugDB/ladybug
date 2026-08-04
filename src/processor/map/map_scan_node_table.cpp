#include "binder/binder.h"
#include "binder/expression/property_expression.h"
#include "binder/expression_binder.h"
#include "catalog/catalog.h"
#include "common/mask.h"
#include "main/attached_database.h"
#include "main/database_manager.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "processor/expression_mapper.h"
#include "processor/operator/scan/primary_key_scan_node_table.h"
#include "processor/operator/scan/scan_node_table.h"
#include "processor/plan_mapper.h"
#include "storage/storage_manager.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapScanNodeTable(
    const LogicalOperator* logicalOperator) {
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto& scan = logicalOperator->constCast<LogicalScanNodeTable>();
    const auto outSchema = scan.getSchema();
    auto nodeIDPos = getDataPos(*scan.getNodeID(), *outSchema);
    std::vector<DataPos> outVectorsPos;
    for (auto& expression : scan.getProperties()) {
        outVectorsPos.emplace_back(getDataPos(*expression, *outSchema));
    }
    auto scanInfo = ScanOpInfo(nodeIDPos, outVectorsPos);
    const auto tableIDs = scan.getTableIDs();
    // Resolve (catalog, storage, entry) for each table ID once, with property-match
    // fallback when dbName is empty (handles ID overlap between databases).
    struct ResolvedTable {
        catalog::Catalog* cat;
        storage::StorageManager* sm;
        catalog::TableCatalogEntry* entry;
    };
    std::vector<ResolvedTable> resolved;
    {
        auto& dbMap = scan.getTableDBMap();
        for (const auto& tableID : tableIDs) {
            auto it = dbMap.find(tableID);
            auto dbName = it != dbMap.end() ? it->second : std::string{};
            auto [cat, sm] =
                main::DatabaseManager::resolveTableStorage(*clientContext, tableID, dbName);
            auto tableEntry = cat->getTableCatalogEntry(transaction, tableID);
            // Fallback: if dbName is empty and the entry lacks any scanned
            // property, try attached databases (IDs overlap between databases).
            if (dbName.empty() && !scan.getProperties().empty()) {
                bool hasAnyProperty = false;
                for (auto& expr : scan.getProperties()) {
                    auto& prop = expr->constCast<PropertyExpression>();
                    if (prop.hasProperty(tableEntry->getTableID()) &&
                        tableEntry->containsProperty(prop.getPropertyName())) {
                        hasAnyProperty = true;
                        break;
                    }
                }
                if (!hasAnyProperty) {
                    auto* dbManager = main::DatabaseManager::Get(*clientContext);
                    for (auto* attachedDB : dbManager->getAttachedDatabases()) {
                        if (attachedDB->getDBType() == common::ATTACHED_LBUG_DB_TYPE) {
                            auto* attachedLbug =
                                static_cast<main::AttachedLbugDatabase*>(attachedDB);
                            if (attachedLbug->getStorageManager()->containsTable(tableID)) {
                                auto* attachedCat = attachedDB->getCatalog();
                                auto* attachedEntry =
                                    attachedCat->getTableCatalogEntry(transaction, tableID);
                                bool found = false;
                                for (auto& expr : scan.getProperties()) {
                                    auto& prop = expr->constCast<PropertyExpression>();
                                    if (prop.hasProperty(attachedEntry->getTableID()) &&
                                        attachedEntry->containsProperty(prop.getPropertyName())) {
                                        found = true;
                                        break;
                                    }
                                }
                                if (found) {
                                    cat = attachedCat;
                                    sm = attachedLbug->getStorageManager();
                                    tableEntry = attachedEntry;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            resolved.push_back({cat, sm, tableEntry});
        }
    }
    // Build tableInfos and sharedStates from the single resolution.
    std::vector<std::string> tableNames;
    std::vector<ScanNodeTableInfo> tableInfos;
    std::vector<std::shared_ptr<ScanNodeTableSharedState>> sharedStates;
    auto binder = Binder(clientContext);
    auto expressionBinder = ExpressionBinder(&binder, clientContext);
    for (size_t i = 0; i < tableIDs.size(); ++i) {
        auto& r = resolved[i];
        auto table = r.sm->getTable(tableIDs[i])->ptrCast<storage::NodeTable>();
        tableNames.push_back(r.entry->getName());
        // Build tableInfo
        auto tableInfo = ScanNodeTableInfo(table, copyVector(scan.getPropertyPredicates()));
        for (auto& expr : scan.getProperties()) {
            auto& property = expr->constCast<PropertyExpression>();
            if (property.hasProperty(r.entry->getTableID())) {
                auto propertyName = property.getPropertyName();
                if (!r.entry->containsProperty(propertyName) && r.entry->containsProperty("data")) {
                    auto columnCaster = ColumnCaster(LogicalType::JSON());
                    columnCaster.setJSONExtract(propertyName);
                    tableInfo.addColumnInfo(r.entry->getColumnID("data"), std::move(columnCaster));
                    continue;
                }
                auto& columnType = r.entry->getProperty(propertyName).getType();
                auto columnCaster = ColumnCaster(columnType.copy());
                if (property.getDataType() != columnType) {
                    auto columnExpr = std::make_shared<PropertyExpression>(property);
                    columnExpr->dataType = columnType.copy();
                    columnCaster.setCastExpr(
                        expressionBinder.forceCast(columnExpr, property.getDataType()));
                }
                tableInfo.addColumnInfo(r.entry->getColumnID(propertyName),
                    std::move(columnCaster));
            } else {
                tableInfo.addColumnInfo(INVALID_COLUMN_ID, ColumnCaster(LogicalType::ANY()));
            }
        }
        tableInfos.push_back(std::move(tableInfo));
        // Build sharedState
        auto semiMask = SemiMaskUtil::createMask(table->getNumTotalRows(transaction));
        sharedStates.push_back(std::make_shared<ScanNodeTableSharedState>(std::move(semiMask)));
    }
    auto alias = scan.getNodeID()->cast<PropertyExpression>().getRawVariableName();
    std::unique_ptr<PhysicalOperator> result;
    switch (scan.getScanType()) {
    case LogicalScanNodeTableType::SCAN: {
        auto printInfo =
            std::make_unique<ScanNodeTablePrintInfo>(tableNames, alias, scan.getProperties());
        auto progressSharedState = std::make_shared<ScanNodeTableProgressSharedState>();
        return std::make_unique<ScanNodeTable>(std::move(scanInfo), std::move(tableInfos),
            std::move(sharedStates), getOperatorID(), std::move(printInfo), progressSharedState);
    }
    case LogicalScanNodeTableType::PRIMARY_KEY_SCAN: {
        auto& primaryKeyScanInfo = scan.getExtraInfo()->constCast<PrimaryKeyScanInfo>();
        auto exprMapper = ExpressionMapper(outSchema);
        auto evaluator =
            primaryKeyScanInfo.isRange && primaryKeyScanInfo.lowerBound == nullptr ?
                nullptr :
                exprMapper.getEvaluator(primaryKeyScanInfo.isRange ? primaryKeyScanInfo.lowerBound :
                                                                     primaryKeyScanInfo.key);
        auto upperBoundEvaluator = primaryKeyScanInfo.upperBound == nullptr ?
                                       nullptr :
                                       exprMapper.getEvaluator(primaryKeyScanInfo.upperBound);
        auto sharedState = std::make_shared<PrimaryKeyScanSharedState>(tableInfos.size());
        auto keyString =
            primaryKeyScanInfo.key != nullptr        ? primaryKeyScanInfo.key->toString() :
            primaryKeyScanInfo.lowerBound != nullptr ? primaryKeyScanInfo.lowerBound->toString() :
                                                       primaryKeyScanInfo.upperBound->toString();
        std::string indexType = "NONE";
        if (tableInfos.size() == 1) {
            auto& nodeTable = tableInfos[0].table->cast<storage::NodeTable>();
            if (auto* pkIndex = nodeTable.tryGetPrimaryKeyIndex()) {
                indexType = pkIndex->getIndexInfo().indexType;
            }
        }
        auto printInfo = std::make_unique<PrimaryKeyScanPrintInfo>(scan.getProperties(), keyString,
            alias, indexType);
        return std::make_unique<PrimaryKeyScanNodeTable>(std::move(scanInfo), std::move(tableInfos),
            std::move(evaluator), std::move(upperBoundEvaluator), primaryKeyScanInfo.isRange,
            false /* isIndexEquality */, primaryKeyScanInfo.lowerInclusive,
            primaryKeyScanInfo.upperInclusive, "", std::move(sharedState), getOperatorID(),
            std::move(printInfo));
    }
    case LogicalScanNodeTableType::SECONDARY_INDEX_SCAN: {
        auto& secondaryIndexScanInfo = scan.getExtraInfo()->constCast<SecondaryIndexScanInfo>();
        auto exprMapper = ExpressionMapper(outSchema);
        auto evaluator = exprMapper.getEvaluator(secondaryIndexScanInfo.key);
        auto sharedState = std::make_shared<PrimaryKeyScanSharedState>(tableInfos.size());
        auto printInfo = std::make_unique<PrimaryKeyScanPrintInfo>(scan.getProperties(),
            secondaryIndexScanInfo.key->toString(), alias, "ART");
        return std::make_unique<PrimaryKeyScanNodeTable>(std::move(scanInfo), std::move(tableInfos),
            std::move(evaluator), nullptr /* upperBoundEvaluator */, true /* isRange */,
            true /* isIndexEquality */, true, true, secondaryIndexScanInfo.indexName,
            std::move(sharedState), getOperatorID(), std::move(printInfo));
    }
    default:
        UNREACHABLE_CODE;
    }
}

} // namespace processor
} // namespace lbug
