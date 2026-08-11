#include "binder/expression/property_expression.h"
#include "binder/expression/scalar_function_expression.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "function/schema/vector_node_rel_functions.h"
#include "processor/operator/arrow_result_collector.h"
#include "processor/physical_plan_util.h"
#include "processor/plan_mapper.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"

using namespace lbug::common;

namespace lbug {
namespace processor {

static bool isProjectedRowIDExpr(const binder::Expression& expr) {
    if (expr.expressionType != ExpressionType::FUNCTION) {
        return false;
    }
    const auto& scalarFunc = expr.constCast<binder::ScalarFunctionExpression>();
    if (scalarFunc.getFunction().name != function::OffsetFunction::name ||
        scalarFunc.getNumChildren() != 1) {
        return false;
    }
    const auto child = scalarFunc.getChild(0);
    if (child->expressionType != ExpressionType::PROPERTY) {
        return false;
    }
    const auto& property = child->constCast<binder::PropertyExpression>();
    return property.isInternalID();
}

static CSRTrackingInfo getCSRTrackingInfo(const binder::expression_vector& expressions) {
    CSRTrackingInfo info;
    std::vector<idx_t> rowIDExprPositions;
    for (auto i = 0u; i < expressions.size(); ++i) {
        if (isProjectedRowIDExpr(*expressions[i])) {
            rowIDExprPositions.push_back(i);
        }
    }
    if (rowIDExprPositions.size() == 2) {
        info.srcRowIDColIdx = rowIDExprPositions[0];
        info.dstRowIDColIdx = rowIDExprPositions[1];
    } else if (rowIDExprPositions.size() == 3) {
        info.srcRowIDColIdx = rowIDExprPositions[0];
        info.relRowIDColIdx = rowIDExprPositions[1];
        info.dstRowIDColIdx = rowIDExprPositions[2];
    }
    return info;
}

// Extract the table ID underlying a projected offset(_id) expression.
static common::table_id_t getRowIDExprTableID(const binder::Expression& expr) {
    const auto& scalarFunc = expr.constCast<binder::ScalarFunctionExpression>();
    const auto& propExpr = scalarFunc.getChild(0)->constCast<binder::PropertyExpression>();
    return propExpr.getSingleTableID();
}

// Resolve the rel group catalog entry being scanned, so we can check its
// CSR-sorted-by-dest declaration. When the rel rowID is projected (the
// 3-expression shape), the rel variable's table ID directly identifies the
// rel group. Otherwise (2-expression shape, no rel rowID) we fall back to
// scanning all rel groups for one whose FROM/TO pair matches the bound (src)
// and nbr (dst) node tables — this is ambiguous if multiple rel groups share
// the same endpoint pair, in which case the first match wins (the sorted-by
// declaration is a user assertion, so ambiguity only risks missing the fast
// path, never correctness — symmetrize() falls back to the safe per-row sort).
static const catalog::RelGroupCatalogEntry* getScannedRelGroupEntry(const CSRTrackingInfo& info,
    const binder::expression_vector& expressions, const transaction::Transaction* trx,
    main::ClientContext* clientContext) {
    auto catalog = catalog::Catalog::Get(*clientContext);
    if (info.hasRelRowID()) {
        const auto relGroupID = getRowIDExprTableID(*expressions[info.relRowIDColIdx]);
        return catalog->getTableCatalogEntry(trx, relGroupID)
            ->ptrCast<catalog::RelGroupCatalogEntry>();
    }
    const auto srcTableID = getRowIDExprTableID(*expressions[info.srcRowIDColIdx]);
    const auto dstTableID = getRowIDExprTableID(*expressions[info.dstRowIDColIdx]);
    for (auto* relGroup : catalog->getRelGroupEntries(trx)) {
        for (const auto& relInfo : relGroup->getRelEntryInfos()) {
            if ((relInfo.nodePair.srcTableID == srcTableID &&
                    relInfo.nodePair.dstTableID == dstTableID) ||
                (relInfo.nodePair.srcTableID == dstTableID &&
                    relInfo.nodePair.dstTableID == srcTableID)) {
                return relGroup;
            }
        }
    }
    return nullptr;
}

std::unique_ptr<PhysicalOperator> PlanMapper::createArrowResultCollector(
    ArrowResultConfig arrowConfig, const binder::expression_vector& expressions,
    planner::Schema* schema, std::unique_ptr<PhysicalOperator> prevOperator,
    OrderPreservationType orderPreservation) {
    std::vector<DataPos> columnDataPos;
    std::vector<LogicalType> columnTypes;
    for (auto& expr : expressions) {
        columnDataPos.push_back(getDataPos(*expr, *schema));
        columnTypes.push_back(expr->getDataType().copy());
    }
    auto sharedState = std::make_shared<ArrowResultCollectorSharedState>();
    sharedState->requireDeterministicOrder =
        (orderPreservation == OrderPreservationType::FIXED_ORDER);
    auto csrTrackingInfo = getCSRTrackingInfo(expressions);
    if (csrTrackingInfo.enabled()) {
        // Look up the source node table's total row count so we can pad
        // trailing empty rows in the CSR indptr (nodes with zero outgoing
        // edges must still have an indptr slot).
        const auto tableID = getRowIDExprTableID(*expressions[csrTrackingInfo.srcRowIDColIdx]);
        auto trx = transaction::Transaction::Get(*clientContext);
        auto table = storage::StorageManager::Get(*clientContext)->getTable(tableID);
        auto nodeTable = table->ptrCast<storage::NodeTable>();
        csrTrackingInfo.numSourceRows = static_cast<int64_t>(nodeTable->getNumTotalRows(trx));
        // Check whether the scanned rel table has been declared CSR-sorted-
        // by-dest (ALTER TABLE ... SET SORTED BY (FROM ASC, TO ASC) CSR) and
        // not mutated since. The declaration is a user assertion;
        // the changeEpoch watermark is the extra safety net that invalidates
        // the fast path on post-declaration mutations. We check the first
        // underlying rel table's storage epoch (matching what alter.cpp
        // captures) — correct for single-rel-table groups (the common case);
        // best-effort for multi-table groups.
        const auto* relGroupEntry =
            getScannedRelGroupEntry(csrTrackingInfo, expressions, trx, clientContext);
        if (relGroupEntry != nullptr && relGroupEntry->isCsrSortedByDest() &&
            !relGroupEntry->getRelEntryInfos().empty()) {
            const auto relOid = relGroupEntry->getRelEntryInfos()[0].oid;
            auto* relTable = storage::StorageManager::Get(*clientContext)->getTable(relOid);
            if (relTable != nullptr &&
                relGroupEntry->getCsrChangeEpoch() == relTable->getChangeEpoch()) {
                csrTrackingInfo.sortedByDest = true;
            }
        }
    }
    auto opInfo = ArrowResultCollectorInfo(arrowConfig.chunkSize, columnDataPos,
        std::move(columnTypes), csrTrackingInfo, orderPreservation);
    auto printInfo = OPPrintInfo::EmptyInfo();
    if (csrTrackingInfo.enabled() &&
        (expressions.size() == 2 || (expressions.size() == 3 && csrTrackingInfo.hasRelRowID()))) {
        auto op = std::make_unique<DirectArrowResultCollector>(sharedState, std::move(opInfo),
            std::move(prevOperator), getOperatorID(), std::move(printInfo));
        op->setDescriptor(std::make_unique<ResultSetDescriptor>(schema));
        return op;
    }
    auto op = std::make_unique<ArrowResultCollector>(sharedState, std::move(opInfo),
        std::move(prevOperator), getOperatorID(), std::move(printInfo));
    op->setDescriptor(std::make_unique<ResultSetDescriptor>(schema));
    return op;
}

} // namespace processor
} // namespace lbug
