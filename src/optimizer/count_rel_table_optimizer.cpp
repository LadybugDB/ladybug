#include "optimizer/count_rel_table_optimizer.h"

#include "binder/binder.h"
#include "binder/expression/aggregate_function_expression.h"
#include "binder/expression/case_expression.h"
#include "binder/expression/expression_util.h"
#include "binder/expression/literal_expression.h"
#include "binder/expression/node_expression.h"
#include "binder/expression/property_expression.h"
#include "binder/expression/rel_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/node_table_id_pair.h"
#include "common/enums/extend_direction_util.h"
#include "common/enums/join_type.h"
#include "common/enums/path_semantic.h"
#include "common/enums/storage_format.h"
#include "function/aggregate/count.h"
#include "function/aggregate/count_star.h"
#include "function/aggregate_function.h"
#include "function/arithmetic/vector_arithmetic_functions.h"
#include "function/gds/rec_joins.h"
#include "main/client_context.h"
#include "planner/operator/extend/logical_extend.h"
#include "planner/operator/extend/logical_recursive_extend.h"
#include "planner/operator/logical_aggregate.h"
#include "planner/operator/logical_filter.h"
#include "planner/operator/logical_hash_join.h"
#include "planner/operator/logical_order_by.h"
#include "planner/operator/logical_path_property_probe.h"
#include "planner/operator/logical_projection.h"
#include "planner/operator/scan/logical_count_anti_edge_chain.h"
#include "planner/operator/scan/logical_count_extend_chain.h"
#include "planner/operator/scan/logical_count_rel_table.h"
#include "planner/operator/scan/logical_reachable_count.h"
#include "planner/operator/scan/logical_rel_degree_table.h"
#include "planner/operator/scan/logical_scan_node_table.h"
#include "storage/storage_manager.h"
#include "storage/table/table.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::planner;
using namespace lbug::binder;
using namespace lbug::catalog;

namespace lbug {
namespace optimizer {

void CountRelTableOptimizer::rewrite(LogicalPlan* plan) {
    visitOperator(plan->getLastOperator());
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::visitOperator(
    const std::shared_ptr<LogicalOperator>& op) {
    // bottom-up traversal
    for (auto i = 0u; i < op->getNumChildren(); ++i) {
        op->setChild(i, visitOperator(op->getChild(i)));
    }
    auto result = visitOperatorReplaceSwitch(op);
    result->computeFlatSchema();
    return result;
}

static LogicalOperator* skipProjections(LogicalOperator* op);
static bool containsOp(const LogicalOperator* root, const LogicalOperator* target);

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteAntiEdgeChainCount(
    std::shared_ptr<LogicalOperator> op) {
    // Target pattern (LSQB q9 shape): COUNT(*) over an inner hash join of
    //   probe: FILTER[NOT(EXISTS{MATCH (a)-[:R]-(b)})] over
    //          HASH_JOIN[MARK keys={a,b}] of
    //            probe: FILTER[id(a)<>id(b)] over chain(a--R--n1--R--b) from scan(n1)
    //            build: anti-edge R-extend over scan(a)
    //   build: filter-free chain of extends from scan(n2=b)
    // i.e. a path n0-R-n1-R-n2-...-nN whose first two hops are R extends from the scan of the
    // middle node n1 (any FWD/BWD/BOTH direction combination), plus an anti-edge between n0
    // and n2 (same rel R), plus a filter-free suffix chain from n2. The count is computed with
    // count arithmetic T - A - S (see CountAntiEdgeChain), parameterized by the enumeration
    // directions of the two chain hops and of the anti-edge match.
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return op;
    }
    auto aggregate = op->ptrCast<LogicalAggregate>();
    if (aggregate->hasKeys() || aggregate->getDependentKeys().size() != 0 ||
        aggregate->getAggregates().size() != 1) {
        return op;
    }
    auto aggExpr = aggregate->getAggregates()[0];
    if (aggExpr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        return op;
    }
    auto aggFuncExpr = aggExpr->ptrCast<AggregateFunctionExpression>();
    if (aggFuncExpr->getFunction().name != function::CountStarFunction::name ||
        aggFuncExpr->isDistinct() || aggFuncExpr->getNumChildren() != 0) {
        return op;
    }
    auto transaction = transaction::Transaction::Get(*_context);
    if (transaction != nullptr && transaction->isWriteTransaction()) {
        return op;
    }

    // Collect the subtree. Allowed ops: projections, extends, node-ID hash joins,
    // unrestricted scans, and filters (classified below).
    std::vector<LogicalExtend*> extends;
    std::vector<LogicalHashJoin*> joins;
    std::vector<LogicalFilter*> filters;
    std::function<bool(LogicalOperator*)> collect = [&](LogicalOperator* current) -> bool {
        switch (current->getOperatorType()) {
        case LogicalOperatorType::PROJECTION:
            return collect(current->getChild(0).get());
        case LogicalOperatorType::SCAN_NODE_TABLE: {
            auto scan = current->ptrCast<LogicalScanNodeTable>();
            if (scan->getScanType() == LogicalScanNodeTableType::PRIMARY_KEY_SCAN) {
                return false;
            }
            for (auto& predicateSet : scan->getPropertyPredicates()) {
                if (!predicateSet.isEmpty()) {
                    return false;
                }
            }
            return true;
        }
        case LogicalOperatorType::EXTEND:
        case LogicalOperatorType::PACKED_EXTEND:
            extends.push_back(current->ptrCast<LogicalExtend>());
            return collect(current->getChild(0).get());
        case LogicalOperatorType::HASH_JOIN:
            joins.push_back(current->ptrCast<LogicalHashJoin>());
            return collect(current->getChild(0).get()) && collect(current->getChild(1).get());
        case LogicalOperatorType::FILTER:
            filters.push_back(current->ptrCast<LogicalFilter>());
            return collect(current->getChild(0).get());
        default:
            return false;
        }
    };
    if (!collect(op->getChild(0).get())) {
        return op;
    }

    // Exactly one MARK join and exactly one other (top) join, and >= 4 extends
    // (2 triangle hops + anti-edge + >= 1 suffix hop).
    LogicalHashJoin* markJoin = nullptr;
    LogicalHashJoin* topJoin = nullptr;
    for (auto* join : joins) {
        if (join->getJoinType() == JoinType::MARK) {
            if (markJoin != nullptr) {
                return op;
            }
            markJoin = join;
        } else {
            if (topJoin != nullptr) {
                return op;
            }
            topJoin = join;
        }
    }
    if (markJoin == nullptr || topJoin == nullptr || extends.size() < 4) {
        return op;
    }
    if (topJoin->getJoinType() != JoinType::INNER || topJoin->getJoinNodeIDs().size() != 1) {
        return op;
    }
    const auto topKey = topJoin->getJoinNodeIDs()[0];

    // The anti-filter must be NOT(X) where X is the mark expression of the MARK join.
    if (!markJoin->hasMark()) {
        return op;
    }
    const auto& markExpr = markJoin->getMark();
    LogicalFilter* antiFilter = nullptr;
    for (auto* filter : filters) {
        if (filter->getPredicate()->expressionType == ExpressionType::NOT &&
            *filter->getPredicate()->getChild(0) == *markExpr) {
            if (antiFilter != nullptr) {
                return op;
            }
            antiFilter = filter;
        }
    }
    if (antiFilter == nullptr) {
        return op;
    }

    // All other filters must be NOT_EQUALS between internal-ID properties.
    for (auto* filter : filters) {
        if (filter == antiFilter) {
            continue;
        }
        const auto& predicate = filter->getPredicate();
        if (predicate->expressionType != ExpressionType::NOT_EQUALS ||
            predicate->getNumChildren() != 2 ||
            predicate->getChild(0)->expressionType != ExpressionType::PROPERTY ||
            predicate->getChild(1)->expressionType != ExpressionType::PROPERTY ||
            !predicate->getChild(0)->ptrCast<PropertyExpression>()->isInternalID() ||
            !predicate->getChild(1)->ptrCast<PropertyExpression>()->isInternalID()) {
            return op;
        }
    }

    // Anti-edge endpoints come from the MARK join keys; the rel table comes from the single
    // anti-edge extend, which must be one of the MARK join's children.
    std::shared_ptr<NodeExpression> antiNodeA;
    std::shared_ptr<NodeExpression> antiNodeB;
    catalog::RelGroupCatalogEntry* antiRelEntry = nullptr;
    LogicalExtend* antiEdgeExtend = nullptr;
    common::ExtendDirection antiEdgeDir = common::ExtendDirection::BOTH;
    LogicalOperator* markProbeChild = nullptr;
    {
        const auto markKeys = markJoin->getJoinNodeIDs();
        if (markKeys.size() != 2) {
            return op;
        }
        std::vector<std::string> keyNames;
        for (auto& key : markKeys) {
            if (key->expressionType != ExpressionType::PROPERTY ||
                !key->ptrCast<PropertyExpression>()->isInternalID()) {
                return op;
            }
            keyNames.push_back(key->ptrCast<PropertyExpression>()->getVariableName());
        }
        if (keyNames[0] == keyNames[1]) {
            return op;
        }
        for (auto ci = 0u; ci < 2; ++ci) {
            auto* child = skipProjections(markJoin->getChild(ci).get());
            if (child->getOperatorType() != LogicalOperatorType::EXTEND &&
                child->getOperatorType() != LogicalOperatorType::PACKED_EXTEND) {
                continue;
            }
            auto ext = child->ptrCast<LogicalExtend>();
            auto rel = ext->getRel();
            if (rel->getNumEntries() != 1) {
                continue;
            }
            auto* relGroupEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
            if (relGroupEntry->getScanFunction().has_value()) {
                continue;
            }
            auto* extendChild = skipProjections((ext->getChild(0).get()));
            if (extendChild->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
                continue;
            }
            const auto boundName = ext->getBoundNode()->getUniqueName();
            const auto nbrName = ext->getNbrNode()->getUniqueName();
            const bool match = (boundName == keyNames[0] && nbrName == keyNames[1]) ||
                               (boundName == keyNames[1] && nbrName == keyNames[0]);
            if (!match) {
                continue;
            }
            antiRelEntry = relGroupEntry;
            antiEdgeExtend = ext;
            antiEdgeDir = ext->getDirection();
            markProbeChild = markJoin->getChild(1 - ci).get();
            // Resolve node expressions: the extend endpoints are the nodes named by the keys.
            auto nodeByName = [&](const std::string& name) -> std::shared_ptr<NodeExpression> {
                return boundName == name ? ext->getBoundNode() : ext->getNbrNode();
            };
            antiNodeA = nodeByName(keyNames[0]);
            antiNodeB = nodeByName(keyNames[1]);
            break;
        }
        if (antiEdgeExtend == nullptr) {
            return op;
        }
    }

    // Parse the MARK join's other child (the prefix chain): exactly 2 extends of the anti rel
    // group, both sharing the scan root node, with other endpoints {antiNodeA, antiNodeB};
    // only id(a)<>id(b) NOT_EQUALS filters allowed. The scan root is the middle node n1. The
    // hop directions (FWD/BWD/BOTH) are recorded per endpoint for the operator arithmetic.
    std::shared_ptr<NodeExpression> midNode;
    common::ExtendDirection chainN0Dir = common::ExtendDirection::BOTH;
    common::ExtendDirection chainN2Dir = common::ExtendDirection::BOTH;
    {
        auto* current = skipProjections(markProbeChild);
        std::vector<LogicalExtend*> chainExtends;
        LogicalScanNodeTable* scanNode = nullptr;
        while (true) {
            if (current->getOperatorType() == LogicalOperatorType::FILTER) {
                const auto predicate = current->ptrCast<LogicalFilter>()->getPredicate();
                if (predicate->expressionType != ExpressionType::NOT_EQUALS ||
                    predicate->getNumChildren() != 2) {
                    return op;
                }
                const auto& c0 = predicate->getChild(0);
                const auto& c1 = predicate->getChild(1);
                const auto isIdProp = [&](const std::shared_ptr<Expression>& e) {
                    return e->expressionType == ExpressionType::PROPERTY &&
                           e->ptrCast<PropertyExpression>()->isInternalID();
                };
                if (!isIdProp(c0) || !isIdProp(c1)) {
                    return op;
                }
                const auto n0 = antiNodeA->getUniqueName();
                const auto n2 = antiNodeB->getUniqueName();
                const auto v0 = c0->ptrCast<PropertyExpression>()->getVariableName();
                const auto v1 = c1->ptrCast<PropertyExpression>()->getVariableName();
                if (!((v0 == n0 && v1 == n2) || (v0 == n2 && v1 == n0))) {
                    return op;
                }
                current = skipProjections(current->getChild(0).get());
                continue;
            }
            if (current->getOperatorType() == LogicalOperatorType::EXTEND ||
                current->getOperatorType() == LogicalOperatorType::PACKED_EXTEND) {
                chainExtends.push_back(current->ptrCast<LogicalExtend>());
                current = skipProjections(current->getChild(0).get());
                continue;
            }
            break;
        }
        if (current->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE ||
            chainExtends.size() != 2) {
            return op;
        }
        scanNode = current->ptrCast<LogicalScanNodeTable>();
        // The scan's nodeID is the internal-ID property of the mid node; recover the mid node
        // variable name from it and the mid NodeExpression from the chain extends (each
        // extension shares the scan root as one endpoint).
        if (scanNode->getNodeID()->expressionType != ExpressionType::PROPERTY ||
            !scanNode->getNodeID()->ptrCast<PropertyExpression>()->isInternalID()) {
            return op;
        }
        const auto midName =
            scanNode->getNodeID()->ptrCast<PropertyExpression>()->getVariableName();
        midNode = nullptr;
        for (auto* ext : chainExtends) {
            if (ext->getBoundNode()->getUniqueName() == midName) {
                midNode = ext->getBoundNode();
                break;
            }
            if (ext->getNbrNode()->getUniqueName() == midName) {
                midNode = ext->getNbrNode();
            }
        }
        if (midNode == nullptr) {
            return op;
        }
        const auto aName = antiNodeA->getUniqueName();
        const auto bName = antiNodeB->getUniqueName();
        // Both extends must share the scan root as one endpoint, with the two anti-edge
        // endpoints as the other endpoints (one each).
        std::unordered_set<std::string> others;
        std::unordered_map<std::string, common::ExtendDirection> hopDirByOther;
        for (auto* ext : chainExtends) {
            if (ext->getRel()->getEntry(0)->ptrCast<RelGroupCatalogEntry>() != antiRelEntry) {
                return op;
            }
            const auto boundName = ext->getBoundNode()->getUniqueName();
            const auto nbrName = ext->getNbrNode()->getUniqueName();
            if (boundName == midName) {
                others.insert(nbrName);
                hopDirByOther[nbrName] = ext->getDirection();
            } else if (nbrName == midName) {
                others.insert(boundName);
                hopDirByOther[boundName] = ext->getDirection();
            } else {
                return op;
            }
        }
        if (others.size() != 2 || !others.contains(aName) || !others.contains(bName)) {
            return op;
        }
        chainN0Dir = hopDirByOther.at(aName);
        chainN2Dir = hopDirByOther.at(bName);
    }

    // Parse the top join's other child as the filter-free suffix chain rooted at the scan of
    // the top join key node (n2).
    std::vector<CountChainHop> suffixHops;
    {
        auto* suffixChild = containsOp(topJoin->getChild(0).get(), antiFilter) ?
                                topJoin->getChild(1).get() :
                                topJoin->getChild(0).get();
        auto* current = skipProjections(suffixChild);
        std::vector<LogicalExtend*> chainExtends;
        LogicalScanNodeTable* scanNode = nullptr;
        while (true) {
            if (current->getOperatorType() == LogicalOperatorType::EXTEND ||
                current->getOperatorType() == LogicalOperatorType::PACKED_EXTEND) {
                chainExtends.push_back(current->ptrCast<LogicalExtend>());
                current = skipProjections(current->getChild(0).get());
                continue;
            }
            break;
        }
        if (current->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE ||
            chainExtends.empty()) {
            return op;
        }
        scanNode = current->ptrCast<LogicalScanNodeTable>();
        const auto cName = topKey->ptrCast<PropertyExpression>()->getVariableName();
        if (scanNode->getNodeID()->expressionType != ExpressionType::PROPERTY ||
            scanNode->getNodeID()->ptrCast<PropertyExpression>()->getVariableName() != cName) {
            return op;
        }
        // Build suffix hops in n2-outward order (reverse of the top-down walk order). Each hop
        // must extend from the previous hop's "to" node (starting at n2 = c).
        auto currentName = cName;
        for (auto it = chainExtends.rbegin(); it != chainExtends.rend(); ++it) {
            auto* ext = *it;
            auto rel = ext->getRel();
            if (rel->getNumEntries() != 1) {
                return op;
            }
            auto* relGroupEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
            if (relGroupEntry->getScanFunction().has_value()) {
                return op;
            }
            if (ext->getDirection() == ExtendDirection::BOTH) {
                return op;
            }
            const auto boundName = ext->getBoundNode()->getUniqueName();
            const auto nbrName = ext->getNbrNode()->getUniqueName();
            const bool fromIsBound = boundName == currentName;
            const bool fromIsNbr = nbrName == currentName;
            if (!fromIsBound && !fromIsNbr) {
                return op;
            }
            const auto toName = fromIsBound ? nbrName : boundName;
            const bool fromIsSrc =
                fromIsBound ? ext->extendFromSourceNode() : !ext->extendFromSourceNode();
            const auto scanDirection = fromIsSrc ? RelDataDirection::FWD : RelDataDirection::BWD;
            const auto fromIsSrcFinal = fromIsSrc;
            CountChainHop hop;
            auto matched = false;
            const auto fromTableID = fromIsSrcFinal ?
                                         relGroupEntry->getRelEntryInfos()[0].nodePair.srcTableID :
                                         relGroupEntry->getRelEntryInfos()[0].nodePair.dstTableID;
            const auto toTableID = fromIsSrcFinal ?
                                       relGroupEntry->getRelEntryInfos()[0].nodePair.dstTableID :
                                       relGroupEntry->getRelEntryInfos()[0].nodePair.srcTableID;
            // The from/to node expressions must be single-table and match the rel entry pair.
            const NodeExpression* fromNode = nullptr;
            const NodeExpression* toNode = nullptr;
            if (fromIsBound) {
                fromNode = ext->getBoundNode().get();
                toNode = ext->getNbrNode().get();
            } else {
                fromNode = ext->getNbrNode().get();
                toNode = ext->getBoundNode().get();
            }
            if (fromNode->isMultiLabeled() || toNode->isMultiLabeled() ||
                fromNode->getNumEntries() != 1 || toNode->getNumEntries() != 1) {
                return op;
            }
            if (fromTableID != fromNode->getTableIDs()[0] ||
                toTableID != toNode->getTableIDs()[0]) {
                return op;
            }
            hop.relScans.push_back({relGroupEntry->getRelEntryInfos()[0].oid, scanDirection,
                fromTableID, toTableID, relGroupEntry->getName()});
            matched = true;
            (void)matched;
            suffixHops.push_back(std::move(hop));
            currentName = toName;
        }
    }

    // n0, n1, n2 must all bind the same single node table.
    const auto midTableID = midNode->getTableIDs()[0];
    if (antiNodeA->isMultiLabeled() || antiNodeB->isMultiLabeled() ||
        antiNodeA->getNumEntries() != 1 || antiNodeB->getNumEntries() != 1 ||
        midNode->isMultiLabeled() || midNode->getNumEntries() != 1) {
        return op;
    }
    if (antiNodeA->getTableIDs()[0] != midTableID || antiNodeB->getTableIDs()[0] != midTableID) {
        return op;
    }

    std::vector<common::table_id_t> antiRelTableIDs;
    antiRelTableIDs.push_back(antiRelEntry->getRelEntryInfos()[0].oid);
    auto result = std::make_shared<LogicalCountAntiEdgeChain>(std::move(suffixHops), antiRelEntry,
        std::move(antiRelTableIDs), midTableID, chainN0Dir, chainN2Dir, antiEdgeDir,
        true /* hasNotEquals */, aggExpr);
    result->computeFlatSchema();
    return result;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteExtendChainCount(
    std::shared_ptr<LogicalOperator> op) {
    // Must be a keyless COUNT_STAR aggregate. Single-hop chains are handled by the (cheaper,
    // metadata-based) COUNT_REL_TABLE rewrite below, so require >= 2 hops.
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return op;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();
    if (aggregate.hasKeys() || aggregate.getDependentKeys().size() != 0 ||
        aggregate.getAggregates().size() != 1) {
        return op;
    }
    auto aggExpr = aggregate.getAggregates()[0];
    if (aggExpr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        return op;
    }
    auto& aggFuncExpr = aggExpr->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.getFunction().name != function::CountStarFunction::name ||
        aggFuncExpr.isDistinct() || aggFuncExpr.getNumChildren() != 0) {
        return op;
    }

    // The fast path enumerates the committed offset space of the node tables; it must not run
    // for transactions that can carry uncommitted node inserts beyond that space.
    auto transaction = transaction::Transaction::Get(*_context);
    if (transaction != nullptr && transaction->isWriteTransaction()) {
        return op;
    }

    // Collect the extends in the subtree. Only row-count-invariant operators may appear:
    // projections and node-ID-only inner hash joins; scans are ignored. Anything else
    // (filters, multiplicity reducers, ...) can change row counts and disqualifies.
    std::vector<const LogicalExtend*> extends;
    std::function<bool(const LogicalOperator*)> collect =
        [&](const LogicalOperator* current) -> bool {
        switch (current->getOperatorType()) {
        case LogicalOperatorType::PROJECTION: {
            return collect(current->getChild(0).get());
        }
        case LogicalOperatorType::SCAN_NODE_TABLE: {
            // The planner can fold node predicates (e.g. a name lookup) into the scan as
            // property predicates or a primary-key scan. Such a scan yields a restricted node
            // set, but the count chain iterates ALL node offsets, so a restricted scan
            // disqualifies the rewrite.
            auto& scan = current->constCast<LogicalScanNodeTable>();
            if (scan.getScanType() == LogicalScanNodeTableType::PRIMARY_KEY_SCAN) {
                return false;
            }
            for (auto& predicateSet : scan.getPropertyPredicates()) {
                if (!predicateSet.isEmpty()) {
                    return false;
                }
            }
            return true;
        }
        case LogicalOperatorType::EXTEND:
        case LogicalOperatorType::PACKED_EXTEND: {
            extends.push_back(&current->constCast<LogicalExtend>());
            return collect(current->getChild(0).get());
        }
        case LogicalOperatorType::HASH_JOIN: {
            auto& join = current->constCast<LogicalHashJoin>();
            const auto joinConditions = join.getJoinConditions();
            if (join.getJoinType() != JoinType::INNER || joinConditions.size() != 1) {
                return false;
            }
            const auto& probeKey = joinConditions[0].first;
            const auto& buildKey = joinConditions[0].second;
            auto isNodeInternalID = [](const std::shared_ptr<Expression>& expression) {
                return expression->expressionType == ExpressionType::PROPERTY &&
                       expression->constCast<PropertyExpression>().isInternalID();
            };
            if (!isNodeInternalID(probeKey) || !isNodeInternalID(buildKey) ||
                probeKey->constCast<PropertyExpression>().getVariableName() !=
                    buildKey->constCast<PropertyExpression>().getVariableName()) {
                return false;
            }
            return collect(current->getChild(0).get()) && collect(current->getChild(1).get());
        }
        default:
            return false;
        }
    };
    if (!collect(op->getChild(0).get())) {
        return op;
    }
    if (extends.size() < 2) {
        return op;
    }

    // Validate the extends form a simple path over single-table nodes with single-entry,
    // storage-backed rel groups.
    // The fast path below iterates the committed node-group grid of native node tables and
    // reads the CSR of native rel tables. Arrow-backed and icebug-disk tables have neither,
    // so they must be left to the regular plan.
    auto isNativeRelGroupEntry = [](const RelGroupCatalogEntry* entry) {
        return entry->getStorage().empty() && entry->getStorageFormat() == StorageFormat::NONE;
    };
    auto isNativeNodeEntry = [](const NodeTableCatalogEntry* entry) {
        return entry->getStorage().empty() && entry->getStorageFormat() == StorageFormat::NONE;
    };
    struct ChainEdge {
        std::string u;
        std::string v;
        const LogicalExtend* extend;
        bool used = false;
    };
    std::vector<ChainEdge> edges;
    edges.reserve(extends.size());
    std::unordered_map<std::string, std::shared_ptr<NodeExpression>> chainNodes;
    for (auto extend : extends) {
        auto rel = extend->getRel();
        if (rel->getRelType() != QueryRelType::NON_RECURSIVE ||
            extend->getDirection() == ExtendDirection::BOTH || rel->getNumEntries() != 1) {
            return op;
        }
        auto* relGroupEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
        if (relGroupEntry->getScanFunction().has_value() || !isNativeRelGroupEntry(relGroupEntry)) {
            return op;
        }
        auto boundNode = extend->getBoundNode();
        auto nbrNode = extend->getNbrNode();
        if (boundNode->isMultiLabeled() || nbrNode->isMultiLabeled() ||
            boundNode->getNumEntries() != 1 || nbrNode->getNumEntries() != 1) {
            return op;
        }
        if (!isNativeNodeEntry(boundNode->getEntry(0)->ptrCast<NodeTableCatalogEntry>()) ||
            !isNativeNodeEntry(nbrNode->getEntry(0)->ptrCast<NodeTableCatalogEntry>())) {
            return op;
        }
        chainNodes.emplace(boundNode->getUniqueName(), boundNode);
        chainNodes.emplace(nbrNode->getUniqueName(), nbrNode);
        edges.push_back({boundNode->getUniqueName(), nbrNode->getUniqueName(), extend});
    }
    std::unordered_map<std::string, uint32_t> degrees;
    for (auto& edge : edges) {
        degrees[edge.u]++;
        degrees[edge.v]++;
        if (degrees[edge.u] > 2 || degrees[edge.v] > 2) {
            return op;
        }
    }
    if (edges.size() != chainNodes.size() - 1) {
        return op;
    }

    // Order the path starting at a degree-1 endpoint. At each interior node exactly one
    // incident edge is already used, so the next unused incident edge continues the path.
    std::string start;
    for (auto& [name, degree] : degrees) {
        if (degree == 1) {
            start = name;
            break;
        }
    }
    if (start.empty()) {
        return op;
    }
    std::vector<std::string> ordered;
    ordered.push_back(start);
    auto current = start;
    for (auto step = 0u; step < edges.size(); ++step) {
        auto found = false;
        for (auto& edge : edges) {
            if (edge.used) {
                continue;
            }
            if (edge.u == current) {
                edge.used = true;
                current = edge.v;
                found = true;
                break;
            }
            if (edge.v == current) {
                edge.used = true;
                current = edge.u;
                found = true;
                break;
            }
        }
        if (!found) {
            return op;
        }
        ordered.push_back(current);
    }

    // Build the hop specs. For each hop the scan direction is the rel direction keyed by the
    // hop's from-node: FWD when the from-node is the rel's src side, BWD when it is the dst.
    std::vector<CountChainHop> hops;
    hops.reserve(ordered.size() - 1);
    for (auto j = 0u; j + 1 < ordered.size(); ++j) {
        const auto& xName = ordered[j];
        const auto& yName = ordered[j + 1];
        auto x = chainNodes.at(xName);
        auto y = chainNodes.at(yName);
        const LogicalExtend* extend = nullptr;
        for (auto& edge : edges) {
            if ((edge.u == xName && edge.v == yName) || (edge.u == yName && edge.v == xName)) {
                extend = edge.extend;
                break;
            }
        }
        DASSERT(extend != nullptr);
        auto rel = extend->getRel();
        auto* relGroupEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
        const auto extendFromSource = extend->extendFromSourceNode();
        const auto xIsBound = xName == extend->getBoundNode()->getUniqueName();
        const auto fromIsSrc = xIsBound ? extendFromSource : !extendFromSource;
        const auto scanDirection = fromIsSrc ? RelDataDirection::FWD : RelDataDirection::BWD;
        const auto xTableID = x->getTableIDs()[0];
        const auto yTableID = y->getTableIDs()[0];
        CountChainHop hop;
        auto matched = false;
        for (auto& info : relGroupEntry->getRelEntryInfos()) {
            const auto fromTableID =
                fromIsSrc ? info.nodePair.srcTableID : info.nodePair.dstTableID;
            const auto toTableID = fromIsSrc ? info.nodePair.dstTableID : info.nodePair.srcTableID;
            if (fromTableID == xTableID && toTableID == yTableID) {
                hop.relScans.push_back(
                    {info.oid, scanDirection, fromTableID, toTableID, relGroupEntry->getName()});
                matched = true;
            }
        }
        if (!matched) {
            return op;
        }
        hops.push_back(std::move(hop));
    }

    auto result = std::make_shared<LogicalCountExtendChain>(std::move(hops), aggExpr);
    result->computeFlatSchema();
    return result;
}

bool CountRelTableOptimizer::isSimpleCount(LogicalOperator* op) const {
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return false;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();

    // Must have no keys (i.e., a simple aggregate without GROUP BY)
    if (aggregate.hasKeys()) {
        return false;
    }

    // Must have exactly one aggregate expression
    auto aggregates = aggregate.getAggregates();
    if (aggregates.size() != 1) {
        return false;
    }

    auto& aggExpr = aggregates[0];
    if (aggExpr->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        return false;
    }
    auto& aggFuncExpr = aggExpr->constCast<AggregateFunctionExpression>();
    const auto& functionName = aggFuncExpr.getFunction().name;
    // Constant SUM is handled only by the COUNT_REL_TABLE rewrite below. The RelDegreeTable
    // rewrites write the raw degree as INT64 and cannot express SUM(constant) semantics
    // (no constant multiplier, no NULL-on-empty-input), so isSimpleCount must stay COUNT-only.
    if (functionName != function::CountStarFunction::name &&
        functionName != function::CountFunction::name) {
        return false;
    }

    if (aggFuncExpr.isDistinct()) {
        return false;
    }

    return true;
}

bool CountRelTableOptimizer::isConstantSum(LogicalOperator* op) const {
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return false;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();
    if (aggregate.hasKeys() || aggregate.getAggregates().size() != 1) {
        return false;
    }
    auto aggregates = aggregate.getAggregates();
    auto aggregateExpression = aggregates[0];
    if (aggregateExpression->expressionType != ExpressionType::AGGREGATE_FUNCTION) {
        return false;
    }
    auto& aggregateFunction = aggregateExpression->constCast<AggregateFunctionExpression>();
    if (aggregateFunction.getFunction().name != function::AggregateSumFunction::name ||
        aggregateFunction.isDistinct() || aggregateFunction.getNumChildren() != 1) {
        return false;
    }
    auto child = aggregateFunction.getChild(0);
    return child->expressionType == ExpressionType::LITERAL &&
           !child->constCast<LiteralExpression>().isNull();
}

bool CountRelTableOptimizer::isLiteralOne(const Expression& expression) {
    if (expression.expressionType != ExpressionType::LITERAL) {
        return false;
    }
    auto value = expression.constCast<LiteralExpression>().getValue();
    if (value.isNull()) {
        return false;
    }
    switch (value.getDataType().getPhysicalType()) {
    case PhysicalTypeID::INT8:
        return value.getValue<int8_t>() == 1;
    case PhysicalTypeID::INT16:
        return value.getValue<int16_t>() == 1;
    case PhysicalTypeID::INT32:
        return value.getValue<int32_t>() == 1;
    case PhysicalTypeID::INT64:
        return value.getValue<int64_t>() == 1;
    case PhysicalTypeID::INT128:
        return value.getValue<int128_t>() == int128_t{int64_t{1}};
    case PhysicalTypeID::UINT8:
        return value.getValue<uint8_t>() == 1;
    case PhysicalTypeID::UINT16:
        return value.getValue<uint16_t>() == 1;
    case PhysicalTypeID::UINT32:
        return value.getValue<uint32_t>() == 1;
    case PhysicalTypeID::UINT64:
        return value.getValue<uint64_t>() == 1;
    case PhysicalTypeID::UINT128:
        return value.getValue<uint128_t>() == uint128_t{uint64_t{1}};
    default:
        return false;
    }
}

std::shared_ptr<Expression> CountRelTableOptimizer::getConstantSumChild(LogicalOperator* op) {
    DASSERT(op->getOperatorType() == LogicalOperatorType::AGGREGATE);
    auto& aggregate = op->constCast<LogicalAggregate>();
    DASSERT(aggregate.getAggregates().size() == 1);
    auto& aggregateFunction =
        aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    DASSERT(aggregateFunction.getFunction().name == function::AggregateSumFunction::name);
    DASSERT(aggregateFunction.getNumChildren() == 1);
    return aggregateFunction.getChild(0);
}

bool CountRelTableOptimizer::isCountStar(LogicalOperator* op) const {
    auto& aggregate = op->constCast<LogicalAggregate>();
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    return aggFuncExpr.getFunction().name == function::CountStarFunction::name;
}

bool CountRelTableOptimizer::isRelIDExpression(const std::shared_ptr<Expression>& expression,
    const RelExpression& rel) const {
    if (expression->expressionType != ExpressionType::PROPERTY) {
        return false;
    }
    auto& property = expression->constCast<PropertyExpression>();
    return property.isInternalID() && *expression == *rel.getInternalID();
}

bool CountRelTableOptimizer::isCountRelID(LogicalOperator* op, const RelExpression& rel) const {
    auto& aggregate = op->constCast<LogicalAggregate>();
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.getFunction().name != function::CountFunction::name) {
        return false;
    }
    if (aggFuncExpr.getNumChildren() != 1) {
        return false;
    }
    return isRelIDExpression(aggFuncExpr.getChild(0), rel);
}

bool CountRelTableOptimizer::isDistinctCountNodeKey(LogicalOperator* op,
    const std::shared_ptr<Expression>& nodeKey) const {
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return false;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();
    if (aggregate.hasKeys() || aggregate.getAggregates().size() != 1) {
        return false;
    }
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.getFunction().name != function::CountFunction::name ||
        !aggFuncExpr.isDistinct() || aggFuncExpr.getNumChildren() != 1) {
        return false;
    }
    return *aggFuncExpr.getChild(0) == *nodeKey;
}

bool CountRelTableOptimizer::isCountNbr(LogicalOperator* op, const NodeExpression& nbr) const {
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return false;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();
    if (aggregate.getAggregates().size() != 1) {
        return false;
    }
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.isDistinct()) {
        return false;
    }
    if (aggFuncExpr.getFunction().name == function::CountStarFunction::name &&
        aggFuncExpr.getNumChildren() == 0) {
        return true;
    }
    if (aggFuncExpr.getFunction().name != function::CountFunction::name ||
        aggFuncExpr.getNumChildren() != 1) {
        return false;
    }
    return *aggFuncExpr.getChild(0) == *nbr.getInternalID();
}

static bool relTablesForExtend(const LogicalExtend& extend, std::vector<table_id_t>& relTableIDs,
    RelGroupCatalogEntry*& relGroupEntry) {
    auto rel = extend.getRel();
    if (extend.getDirection() != ExtendDirection::FWD || rel->getNumEntries() != 1) {
        return false;
    }
    DASSERT(rel->getNumEntries() == 1);
    relGroupEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();
    // Foreign-backed rel tables are scan-driven and own no CSR metadata to count
    // from; let the regular aggregate pipeline scan them instead.
    if (relGroupEntry->getScanFunction().has_value()) {
        return false;
    }
    auto boundNodeTableIDs = extend.getBoundNode()->getTableIDsSet();
    auto nbrNodeTableIDs = extend.getNbrNode()->getTableIDsSet();
    for (auto& info : relGroupEntry->getRelEntryInfos()) {
        bool matches = extend.extendFromSourceNode() ?
                           boundNodeTableIDs.contains(info.nodePair.srcTableID) &&
                               nbrNodeTableIDs.contains(info.nodePair.dstTableID) :
                           boundNodeTableIDs.contains(info.nodePair.dstTableID) &&
                               nbrNodeTableIDs.contains(info.nodePair.srcTableID);
        if (matches) {
            relTableIDs.push_back(info.oid);
        }
    }
    return !relTableIDs.empty();
}

bool CountRelTableOptimizer::canOptimize(LogicalOperator* aggregate) const {
    // Pattern we're looking for:
    // AGGREGATE (COUNT_STAR or COUNT(rel._ID), no keys)
    //   -> PROJECTION (empty expressions, pass-through, or rel._ID)
    //      -> EXTEND (single rel table, no properties scanned)
    //         -> SCAN_NODE_TABLE (no properties scanned)
    //
    // Note: The projection between aggregate and extend might be empty or
    // just projecting the COUNT(rel) input.

    auto* current = aggregate->getChild(0).get();

    std::vector<LogicalProjection*> projections;
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        projections.push_back(current->ptrCast<LogicalProjection>());
        current = current->getChild(0).get();
    }

    // Now we should have EXTEND
    if (current->getOperatorType() != LogicalOperatorType::EXTEND) {
        return false;
    }
    auto& extend = current->constCast<LogicalExtend>();

    // Don't optimize for undirected edges (BOTH direction) - the query pattern
    // (a)-[e]-(b) generates a plan that scans both directions, and optimizing
    // this would require special handling to avoid double counting.
    if (extend.getDirection() == ExtendDirection::BOTH) {
        return false;
    }

    // The rel should be a single table (not multi-labeled)
    auto rel = extend.getRel();
    if (rel->isMultiLabeled()) {
        return false;
    }

    // Foreign-backed rel tables are scan-driven and own no CSR metadata to
    // count from (ForeignRelTable::getNumTotalRows is not a CSR count); let
    // the regular aggregate pipeline scan them instead.
    for (auto entryIdx = 0u; entryIdx < rel->getNumEntries(); ++entryIdx) {
        auto* relGroupEntry = rel->getEntry(entryIdx)->ptrCast<RelGroupCatalogEntry>();
        if (relGroupEntry != nullptr && relGroupEntry->getScanFunction().has_value()) {
            return false;
        }
    }

    if (!isCountStar(aggregate) && !isCountRelID(aggregate, *rel) && !isConstantSum(aggregate)) {
        return false;
    }

    // Check if we're scanning any properties. COUNT(rel) needs only rel._ID; other rel properties
    // would make the relationship variable observable beyond simple cardinality.
    for (auto& property : extend.getProperties()) {
        if (!isRelIDExpression(property, *rel)) {
            return false;
        }
    }

    for (auto* projection : projections) {
        for (auto& expression : projection->getExpressionsToProject()) {
            if (expression->expressionType != ExpressionType::AGGREGATE_FUNCTION &&
                expression->expressionType != ExpressionType::LITERAL &&
                !isRelIDExpression(expression, *rel)) {
                return false;
            }
        }
    }

    // The child of extend should be SCAN_NODE_TABLE
    auto* extendChild = current->getChild(0).get();
    if (extendChild->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return false;
    }
    auto& scanNode = extendChild->constCast<LogicalScanNodeTable>();

    // Check if node scan has any properties (we can only optimize when no properties needed)
    if (!scanNode.getProperties().empty()) {
        return false;
    }

    return true;
}

static bool containsOp(const LogicalOperator* root, const LogicalOperator* target) {
    if (root == target) {
        return true;
    }
    for (auto i = 0u; i < root->getNumChildren(); ++i) {
        if (containsOp(root->getChild(i).get(), target)) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::visitAggregateReplace(
    std::shared_ptr<LogicalOperator> op) {
    if (auto rewritten = tryRewriteAntiEdgeChainCount(op); rewritten != op) {
        return rewritten;
    }
    if (auto rewritten = tryRewriteExtendChainCount(op); rewritten != op) {
        return rewritten;
    }
    if (auto rewritten = tryRewriteReachableCount(op); rewritten != op) {
        return rewritten;
    }
    if (auto rewritten = tryRewriteActiveBoundCount(op); rewritten != op) {
        return rewritten;
    }
    if (auto rewritten = tryRewriteSortedOffsetCount(op); rewritten != op) {
        return rewritten;
    }
    if (!isSimpleCount(op.get()) && !isConstantSum(op.get())) {
        return op;
    }

    if (!canOptimize(op.get())) {
        return op;
    }

    // Find the EXTEND operator
    auto* current = op->getChild(0).get();
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        current = current->getChild(0).get();
    }

    DASSERT(current->getOperatorType() == LogicalOperatorType::EXTEND);
    auto& extend = current->constCast<LogicalExtend>();
    auto rel = extend.getRel();
    auto boundNode = extend.getBoundNode();
    auto nbrNode = extend.getNbrNode();

    // Get the rel group entry
    DASSERT(rel->getNumEntries() == 1);
    auto* relGroupEntry = rel->getEntry(0)->ptrCast<RelGroupCatalogEntry>();

    // Determine the source and destination node table IDs based on extend direction.
    // If extendFromSource is true, then boundNode is the source and nbrNode is the destination.
    // If extendFromSource is false, then boundNode is the destination and nbrNode is the source.
    auto boundNodeTableIDs = boundNode->getTableIDsSet();
    auto nbrNodeTableIDs = nbrNode->getTableIDsSet();

    // Get only the rel table IDs that match the specific node table ID pairs in the query.
    // A rel table connects a specific (srcTableID, dstTableID) pair.
    std::vector<table_id_t> relTableIDs;
    for (auto& info : relGroupEntry->getRelEntryInfos()) {
        table_id_t srcTableID = info.nodePair.srcTableID;
        table_id_t dstTableID = info.nodePair.dstTableID;

        bool matches = false;
        if (extend.extendFromSourceNode()) {
            // boundNode is src, nbrNode is dst
            matches =
                boundNodeTableIDs.contains(srcTableID) && nbrNodeTableIDs.contains(dstTableID);
        } else {
            // boundNode is dst, nbrNode is src
            matches =
                boundNodeTableIDs.contains(dstTableID) && nbrNodeTableIDs.contains(srcTableID);
        }

        if (matches) {
            relTableIDs.push_back(info.oid);
        }
    }

    // If no matching rel tables, don't optimize (shouldn't happen for valid queries)
    if (relTableIDs.empty()) {
        return op;
    }

    // Get the result expression from the original aggregate.
    auto& aggregate = op->constCast<LogicalAggregate>();
    auto resultExpr = aggregate.getAggregates()[0];

    // Get the bound node table IDs as a vector
    std::vector<table_id_t> boundNodeTableIDsVec(boundNodeTableIDs.begin(),
        boundNodeTableIDs.end());

    const auto constantSum = isConstantSum(op.get());
    auto directConstantSum = false;
    if (constantSum) {
        directConstantSum = isLiteralOne(*getConstantSumChild(op.get()));
    }

    // SUM(1) can use the aggregate's expression as the count output directly. The physical
    // operator writes the count using SUM's result type and returns NULL for an empty input.
    if (!constantSum || directConstantSum) {
        auto countRelTable = std::make_shared<LogicalCountRelTable>(relGroupEntry,
            std::move(relTableIDs), std::move(boundNodeTableIDsVec), boundNode,
            extend.getDirection(), resultExpr, directConstantSum, rel->getDbName(relGroupEntry));
        countRelTable->computeFlatSchema();
        return countRelTable;
    }

    // For SUM(c), produce an INT64 metadata count and evaluate one typed multiplication above it.
    // The CASE preserves SUM's NULL-on-empty-input semantics.
    auto binder = Binder(_context);
    auto* expressionBinder = binder.getExpressionBinder();
    auto countExpr = binder.createInvisibleVariable("__count_rel_table", LogicalType::INT64());
    auto countRelTable = std::make_shared<LogicalCountRelTable>(relGroupEntry,
        std::move(relTableIDs), std::move(boundNodeTableIDsVec), boundNode, extend.getDirection(),
        countExpr, false, rel->getDbName(relGroupEntry));
    countRelTable->computeFlatSchema();

    auto resultType = resultExpr->getDataType().copy();
    auto countForProduct = expressionBinder->implicitCastIfNecessary(countExpr, resultType);
    auto constantValue = getConstantSumChild(op.get())->constCast<LiteralExpression>().getValue();
    auto constantForProduct = expressionBinder->createLiteralExpression(constantValue);
    constantForProduct = expressionBinder->implicitCastIfNecessary(constantForProduct, resultType);
    auto product = expressionBinder->bindScalarFunctionExpression(
        {countForProduct, constantForProduct}, function::MultiplyFunction::name);

    auto zero = expressionBinder->createLiteralExpression(Value{int64_t{0}});
    auto countIsZero = expressionBinder->createEqualityComparisonExpression(countExpr, zero);
    auto nullResult =
        expressionBinder->createNullLiteralExpression(Value::createNullValue(resultType));
    auto projectedSum =
        std::make_shared<CaseExpression>(resultType.copy(), product, resultExpr->getUniqueName());
    projectedSum->addCaseAlternative(countIsZero, nullResult);
    if (resultExpr->hasAlias()) {
        projectedSum->setAlias(resultExpr->getAlias());
    }

    auto projection = std::make_shared<LogicalProjection>(
        expression_vector{std::move(projectedSum)}, std::move(countRelTable));
    projection->computeFlatSchema();
    return projection;
}

static LogicalOperator* skipProjections(LogicalOperator* op) {
    while (op->getOperatorType() == LogicalOperatorType::PROJECTION) {
        op = op->getChild(0).get();
    }
    return op;
}

static bool isPropertyForNodePrimaryKey(const Expression& expression, const NodeExpression& node) {
    if (expression.expressionType != ExpressionType::PROPERTY || node.getNumEntries() != 1) {
        return false;
    }
    auto& property = expression.constCast<PropertyExpression>();
    return property.getVariableName() == node.getUniqueName() &&
           property.isPrimaryKey(node.getTableIDs()[0]);
}

static bool literalToOffset(const Expression& expression, offset_t& offset) {
    if (expression.expressionType != ExpressionType::LITERAL) {
        return false;
    }
    auto value = expression.constCast<LiteralExpression>().getValue();
    if (value.isNull()) {
        return false;
    }
    switch (value.getDataType().getPhysicalType()) {
    case PhysicalTypeID::INT8: {
        auto signedValue = value.getValue<int8_t>();
        if (signedValue < 0) {
            return false;
        }
        offset = static_cast<offset_t>(signedValue);
        return true;
    }
    case PhysicalTypeID::INT16: {
        auto signedValue = value.getValue<int16_t>();
        if (signedValue < 0) {
            return false;
        }
        offset = static_cast<offset_t>(signedValue);
        return true;
    }
    case PhysicalTypeID::INT32: {
        auto signedValue = value.getValue<int32_t>();
        if (signedValue < 0) {
            return false;
        }
        offset = static_cast<offset_t>(signedValue);
        return true;
    }
    case PhysicalTypeID::INT64: {
        auto signedValue = value.getValue<int64_t>();
        if (signedValue < 0) {
            return false;
        }
        offset = static_cast<offset_t>(signedValue);
        return true;
    }
    case PhysicalTypeID::UINT8: {
        offset = static_cast<offset_t>(value.getValue<uint8_t>());
        return true;
    }
    case PhysicalTypeID::UINT16: {
        offset = static_cast<offset_t>(value.getValue<uint16_t>());
        return true;
    }
    case PhysicalTypeID::UINT32: {
        offset = static_cast<offset_t>(value.getValue<uint32_t>());
        return true;
    }
    case PhysicalTypeID::UINT64: {
        offset = static_cast<offset_t>(value.getValue<uint64_t>());
        return true;
    }
    default:
        return false;
    }
}

static bool getPrimaryKeyOffsetPredicate(const Expression& predicate, const NodeExpression& node,
    offset_t& offset) {
    if (predicate.expressionType != ExpressionType::EQUALS || predicate.getNumChildren() != 2) {
        return false;
    }
    auto lhs = predicate.getChild(0);
    auto rhs = predicate.getChild(1);
    if (isPropertyForNodePrimaryKey(*lhs, node)) {
        return literalToOffset(*rhs, offset);
    }
    if (isPropertyForNodePrimaryKey(*rhs, node)) {
        return literalToOffset(*lhs, offset);
    }
    return false;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteReachableCount(
    std::shared_ptr<LogicalOperator> op) {
    // Target: AGGREGATE COUNT(DISTINCT <nbr._ID>) with no keys, over a variable-length
    // (a)-[r*lo..up]->(b) path whose source node `a` is fixed to a single node via a primary-key
    // predicate on a CSR-sorted node table. In this case count(distinct b) is exactly the number of
    // distinct nodes reachable from `a` by a walk of any length in [lo, up], which can be computed
    // by a bounded traversal without the recursive extend / hash-join subtree.
    if (op->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return op;
    }
    auto& aggregate = op->constCast<LogicalAggregate>();
    if (aggregate.hasKeys() || aggregate.getAggregates().size() != 1) {
        return op;
    }
    auto& aggFuncExpr = aggregate.getAggregates()[0]->constCast<AggregateFunctionExpression>();
    if (aggFuncExpr.getFunction().name != function::CountFunction::name ||
        !aggFuncExpr.isDistinct() || aggFuncExpr.getNumChildren() != 1) {
        return op;
    }
    auto countedExpr = aggFuncExpr.getChild(0);

    // Descend through projections to a hash join that binds the recursive extend output with the
    // fixed source node scan.
    auto* current = skipProjections(op->getChild(0).get());
    if (current->getOperatorType() != LogicalOperatorType::HASH_JOIN) {
        return op;
    }

    // Locate the recursive extend within the join subtree.
    LogicalRecursiveExtend* recursiveExtend = nullptr;
    std::function<void(LogicalOperator*)> findRecursive = [&](LogicalOperator* n) {
        if (recursiveExtend != nullptr) {
            return;
        }
        if (n->getOperatorType() == LogicalOperatorType::RECURSIVE_EXTEND) {
            recursiveExtend = n->ptrCast<LogicalRecursiveExtend>();
            return;
        }
        for (auto i = 0u; i < n->getNumChildren(); ++i) {
            findRecursive(n->getChild(i).get());
        }
    };
    findRecursive(current);
    if (recursiveExtend == nullptr) {
        return op;
    }

    auto& bindData = recursiveExtend->getBindData();
    // Only forward variable-length walks are handled. Traversals with a node predicate restrict the
    // reachable set and are left to the (correct) original plan.
    if (bindData.extendDirection != ExtendDirection::FWD ||
        bindData.semantic != common::PathSemantic::WALK || bindData.upperBound == 0 ||
        recursiveExtend->hasNodePredicate()) {
        return op;
    }
    auto boundNode = std::static_pointer_cast<NodeExpression>(bindData.nodeInput);
    auto nbrNode = std::static_pointer_cast<NodeExpression>(bindData.nodeOutput);
    if (boundNode->isMultiLabeled() || nbrNode->isMultiLabeled() ||
        !(*countedExpr == *nbrNode->getInternalID())) {
        return op;
    }

    // Identify the side of the hash join that carries the recursive extend; the other side is the
    // fixed source-node scan of `a`.
    auto subtreeHasRecursive = [](LogicalOperator* n) {
        std::function<bool(LogicalOperator*)> containsRec = [&](LogicalOperator* m) -> bool {
            if (m->getOperatorType() == LogicalOperatorType::RECURSIVE_EXTEND) {
                return true;
            }
            for (auto i = 0u; i < m->getNumChildren(); ++i) {
                if (containsRec(m->getChild(i).get())) {
                    return true;
                }
            }
            return false;
        };
        return containsRec(n);
    };
    auto* leftChild = current->getChild(0).get();
    auto* rightChild = current->getChild(1).get();
    auto* sourceSide = subtreeHasRecursive(leftChild) ? rightChild : leftChild;

    // Navigate to the source scan, skipping projection/semi-masker/filter operators.
    LogicalOperator* source = sourceSide;
    while (source->getOperatorType() == LogicalOperatorType::PROJECTION ||
           source->getOperatorType() == LogicalOperatorType::SEMI_MASKER) {
        source = source->getChild(0).get();
    }
    const LogicalFilter* sourceFilter = nullptr;
    if (source->getOperatorType() == LogicalOperatorType::FILTER) {
        sourceFilter = source->ptrCast<LogicalFilter>();
        source = skipProjections(source->getChild(0).get());
    }
    if (source->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return op;
    }
    auto& scan = source->constCast<LogicalScanNodeTable>();

    // Derive the fixed source offset from a primary-key literal. CSR (primary_key == rowid) lets us
    // turn the pk literal directly into a node offset without a lookup.
    offset_t offset = INVALID_OFFSET;
    if (sourceFilter != nullptr) {
        if (!getPrimaryKeyOffsetPredicate(*sourceFilter->getPredicate(), *boundNode, offset)) {
            return op;
        }
    } else if (scan.getScanType() == LogicalScanNodeTableType::PRIMARY_KEY_SCAN &&
               scan.getExtraInfo() != nullptr) {
        auto& primaryKeyScanInfo = scan.getExtraInfo()->constCast<PrimaryKeyScanInfo>();
        if (primaryKeyScanInfo.isRange || !primaryKeyScanInfo.key ||
            !literalToOffset(*primaryKeyScanInfo.key, offset)) {
            return op;
        }
    } else {
        return op;
    }

    // CSR gate: the invariant primary_key == rowid is only an explicit user declaration, so it must
    // be confirmed and unchanged since declaration.
    if (boundNode->getNumEntries() != 1) {
        return op;
    }
    auto tableID = boundNode->getTableIDs()[0];
    auto* nodeEntry = boundNode->getEntry(0)->ptrCast<NodeTableCatalogEntry>();
    if (!nodeEntry->isCsr()) {
        return op;
    }
    auto* table = storage::StorageManager::Get(*_context)->getTable(tableID);
    if (!table || table->getChangeEpoch() != nodeEntry->getCsrChangeEpoch()) {
        return op;
    }

    auto relEntries = bindData.graphEntry.getRelEntries();
    if (relEntries.size() != 1) {
        return op;
    }
    auto* relGroupEntry = relEntries[0]->ptrCast<RelGroupCatalogEntry>();
    auto countExpr = op->constCast<LogicalAggregate>().getAggregates()[0];
    auto result = std::make_shared<LogicalReachableCount>(relGroupEntry, boundNode, nbrNode,
        bindData.extendDirection, bindData.lowerBound, bindData.upperBound, countExpr,
        std::vector<offset_t>{offset});
    result->computeFlatSchema();
    return result;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteSortedOffsetCount(
    std::shared_ptr<LogicalOperator> op) {
    if (!isSimpleCount(op.get())) {
        return op;
    }
    auto* current = skipProjections(op->getChild(0).get());
    const LogicalFilter* filter = nullptr;
    if (current->getOperatorType() == LogicalOperatorType::FILTER) {
        filter = current->ptrCast<LogicalFilter>();
        current = skipProjections(current->getChild(0).get());
    }
    if (current->getOperatorType() != LogicalOperatorType::EXTEND) {
        return op;
    }
    auto& extend = current->constCast<LogicalExtend>();
    auto boundNode = extend.getBoundNode();
    if (boundNode->getNumEntries() != 1 || extend.getDirection() == ExtendDirection::BOTH ||
        !extend.getProperties().empty()) {
        return op;
    }
    auto tableID = boundNode->getTableIDs()[0];
    auto* nodeEntry = boundNode->getEntry(0)->ptrCast<NodeTableCatalogEntry>();
    // The CSR declaration asserts primary_key == rowid (csr_index interchangeable with the rel
    // table's table_offset). This is only an explicit user declaration, not a derivable fact from
    // the sort order, so it must be gated on the CSR flag.
    if (!nodeEntry->isCsr()) {
        return op;
    }
    // Any mutation of the node table invalidates the CSR invariant, so disregard the
    // optimization if the table has been mutated since the CSR declaration.
    auto* table = storage::StorageManager::Get(*_context)->getTable(tableID);
    if (!table || table->getChangeEpoch() != nodeEntry->getCsrChangeEpoch()) {
        return op;
    }
    auto nodeKey = boundNode->getPrimaryKey(tableID);
    if (!nodeKey) {
        return op;
    }
    auto* scan = current->getChild(0).get();
    if (scan->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return op;
    }
    auto& scanNode = scan->constCast<LogicalScanNodeTable>();
    offset_t offset = INVALID_OFFSET;
    if (filter) {
        if (!getPrimaryKeyOffsetPredicate(*filter->getPredicate(), *boundNode, offset)) {
            return op;
        }
    } else {
        if (scanNode.getScanType() != LogicalScanNodeTableType::PRIMARY_KEY_SCAN ||
            scanNode.getExtraInfo() == nullptr) {
            return op;
        }
        auto& primaryKeyScanInfo = scanNode.getExtraInfo()->constCast<PrimaryKeyScanInfo>();
        if (primaryKeyScanInfo.isRange || !primaryKeyScanInfo.key ||
            !literalToOffset(*primaryKeyScanInfo.key, offset)) {
            return op;
        }
    }
    for (auto& property : scanNode.getProperties()) {
        if (!(*property == *nodeKey)) {
            return op;
        }
    }
    std::vector<table_id_t> relTableIDs;
    RelGroupCatalogEntry* relGroupEntry = nullptr;
    if (!relTablesForExtend(extend, relTableIDs, relGroupEntry)) {
        return op;
    }
    auto countExpr = op->constCast<LogicalAggregate>().getAggregates()[0];
    auto result =
        std::make_shared<LogicalRelDegreeTable>(relGroupEntry, std::move(relTableIDs), boundNode,
            extend.getDirection(), RelDegreeTableMode::OFFSET_COUNT, nodeKey, countExpr, 1, offset);
    result->computeFlatSchema();
    return result;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteActiveBoundCount(
    std::shared_ptr<LogicalOperator> op) {
    auto* current = op->getChild(0).get();
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        current = current->getChild(0).get();
    }
    if (current->getOperatorType() != LogicalOperatorType::EXTEND) {
        return op;
    }
    auto& extend = current->constCast<LogicalExtend>();
    auto boundNode = extend.getBoundNode();
    if (boundNode->isMultiLabeled()) {
        return op;
    }
    auto boundKey = boundNode->getPrimaryKey(boundNode->getTableIDs()[0]);
    if (!boundKey || !isDistinctCountNodeKey(op.get(), boundKey)) {
        return op;
    }
    if (!extend.getProperties().empty()) {
        return op;
    }
    auto* scan = current->getChild(0).get();
    if (scan->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return op;
    }
    auto& scanNode = scan->constCast<LogicalScanNodeTable>();
    for (auto& property : scanNode.getProperties()) {
        if (!(*property == *boundKey)) {
            return op;
        }
    }
    std::vector<table_id_t> relTableIDs;
    RelGroupCatalogEntry* relGroupEntry = nullptr;
    if (!relTablesForExtend(extend, relTableIDs, relGroupEntry)) {
        return op;
    }
    auto countExpr = op->constCast<LogicalAggregate>().getAggregates()[0];
    auto result =
        std::make_shared<LogicalRelDegreeTable>(relGroupEntry, std::move(relTableIDs), boundNode,
            extend.getDirection(), RelDegreeTableMode::ACTIVE_BOUND_COUNT, boundKey, countExpr, 1);
    result->computeFlatSchema();
    return result;
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::visitOrderByReplace(
    std::shared_ptr<LogicalOperator> op) {
    return tryRewriteDegreeTopK(op);
}

std::shared_ptr<LogicalOperator> CountRelTableOptimizer::tryRewriteDegreeTopK(
    std::shared_ptr<LogicalOperator> op) {
    auto& orderBy = op->constCast<LogicalOrderBy>();
    if (!orderBy.hasLimitNum() || orderBy.hasSkipNum() ||
        !ExpressionUtil::canEvaluateAsLiteral(*orderBy.getLimitNum()) ||
        orderBy.getExpressionsToOrderBy().size() != 1 || orderBy.getIsAscOrders().size() != 1 ||
        orderBy.getIsAscOrders()[0]) {
        return op;
    }
    const auto limit = ExpressionUtil::evaluateAsSkipLimit(*orderBy.getLimitNum());
    auto* current = op->getChild(0).get();
    while (current->getOperatorType() == LogicalOperatorType::PROJECTION) {
        current = current->getChild(0).get();
    }
    if (current->getOperatorType() != LogicalOperatorType::AGGREGATE) {
        return op;
    }
    auto& aggregate = current->constCast<LogicalAggregate>();
    if (aggregate.getKeys().size() != 1 || aggregate.getAggregates().size() != 1 ||
        aggregate.getDependentKeys().size() != 0 ||
        !(*orderBy.getExpressionsToOrderBy()[0] == *aggregate.getAggregates()[0])) {
        return op;
    }
    auto nodeKey = aggregate.getKeys()[0];
    auto* aggregateChild = current->getChild(0).get();
    while (aggregateChild->getOperatorType() == LogicalOperatorType::PROJECTION) {
        aggregateChild = aggregateChild->getChild(0).get();
    }
    if (aggregateChild->getOperatorType() != LogicalOperatorType::EXTEND) {
        return op;
    }
    auto& extend = aggregateChild->constCast<LogicalExtend>();
    auto boundNode = extend.getBoundNode();
    if (boundNode->isMultiLabeled() ||
        !(*nodeKey == *boundNode->getPrimaryKey(boundNode->getTableIDs()[0])) ||
        !isCountNbr(current, *extend.getNbrNode()) || !extend.getProperties().empty()) {
        return op;
    }
    auto* scan = aggregateChild->getChild(0).get();
    if (scan->getOperatorType() != LogicalOperatorType::SCAN_NODE_TABLE) {
        return op;
    }
    auto& scanNode = scan->constCast<LogicalScanNodeTable>();
    for (auto& property : scanNode.getProperties()) {
        if (!(*property == *nodeKey)) {
            return op;
        }
    }
    std::vector<table_id_t> relTableIDs;
    RelGroupCatalogEntry* relGroupEntry = nullptr;
    if (!relTablesForExtend(extend, relTableIDs, relGroupEntry)) {
        return op;
    }
    auto result = std::make_shared<LogicalRelDegreeTable>(relGroupEntry, std::move(relTableIDs),
        boundNode, extend.getDirection(), RelDegreeTableMode::TOP_K_DEGREES, nodeKey,
        aggregate.getAggregates()[0], limit);
    result->computeFlatSchema();
    return result;
}

} // namespace optimizer
} // namespace lbug
