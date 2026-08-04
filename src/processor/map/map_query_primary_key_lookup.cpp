#include "binder/binder.h"
#include "binder/expression/property_expression.h"
#include "binder/expression_binder.h"
#include "catalog/catalog.h"
#include "planner/operator/scan/logical_query_primary_key_lookup.h"
#include "processor/expression_mapper.h"
#include "processor/operator/query_primary_key_lookup.h"
#include "processor/plan_mapper.h"
#include "storage/storage_manager.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapQueryPrimaryKeyLookup(
    const LogicalOperator* logicalOperator) {
    auto& lookup = logicalOperator->constCast<LogicalQueryPrimaryKeyLookup>();
    auto child = logicalOperator->getChild(0).get();
    auto outSchema = lookup.getSchema();
    auto storageManager = storage::StorageManager::Get(*clientContext);
    auto table = storageManager->getTable(lookup.getTableID())->ptrCast<storage::NodeTable>();
    DASSERT(table->tryGetPrimaryKeyIndex() != nullptr);

    auto nodeIDPos = getDataPos(*lookup.getNodeID(), *outSchema);
    std::vector<DataPos> outVectorsPos;
    for (auto& expression : lookup.getProperties()) {
        outVectorsPos.emplace_back(getDataPos(*expression, *outSchema));
    }
    auto scanInfo = ScanOpInfo(nodeIDPos, outVectorsPos);

    auto catalog = catalog::Catalog::Get(*clientContext);
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto tableEntry = catalog->getTableCatalogEntry(transaction, lookup.getTableID());
    auto binder = Binder(clientContext);
    auto expressionBinder = ExpressionBinder(&binder, clientContext);
    auto tableInfo = ScanNodeTableInfo(table, {});
    for (auto& expression : lookup.getProperties()) {
        auto& property = expression->constCast<PropertyExpression>();
        auto propertyName = property.getPropertyName();
        if (!tableEntry->containsProperty(propertyName) && tableEntry->containsProperty("data")) {
            auto columnCaster = ColumnCaster(LogicalType::JSON());
            columnCaster.setJSONExtract(propertyName);
            tableInfo.addColumnInfo(tableEntry->getColumnID("data"), std::move(columnCaster));
            continue;
        }
        auto& columnType = tableEntry->getProperty(propertyName).getType();
        auto columnCaster = ColumnCaster(columnType.copy());
        if (property.getDataType() != columnType) {
            auto columnExpr = std::make_shared<PropertyExpression>(property);
            columnExpr->dataType = columnType.copy();
            columnCaster.setCastExpr(
                expressionBinder.forceCast(columnExpr, property.getDataType()));
        }
        tableInfo.addColumnInfo(tableEntry->getColumnID(propertyName), std::move(columnCaster));
    }

    auto exprMapper = ExpressionMapper(child->getSchema());
    auto keyEvaluator = exprMapper.getEvaluator(lookup.getKey());
    auto alias = lookup.getNodeID()->cast<PropertyExpression>().getRawVariableName();
    auto printInfo = std::make_unique<QueryPrimaryKeyLookupPrintInfo>(tableEntry->getName(),
        lookup.getKey()->toString(), alias, lookup.getProperties());
    return std::make_unique<QueryPrimaryKeyLookup>(table, std::move(scanInfo), std::move(tableInfo),
        std::move(keyEvaluator), mapOperator(child), getOperatorID(), std::move(printInfo));
}

} // namespace processor
} // namespace lbug
