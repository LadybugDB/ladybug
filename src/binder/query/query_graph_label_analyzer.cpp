#include "binder/query/query_graph_label_analyzer.h"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/string_format.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::catalog;
using namespace lbug::transaction;

namespace lbug {
namespace binder {

// NOLINTNEXTLINE(readability-non-const-parameter): graph is supposed to be modified.
void QueryGraphLabelAnalyzer::pruneLabel(QueryGraph& graph) const {
    for (auto i = 0u; i < graph.getNumQueryNodes(); ++i) {
        pruneNode(graph, *graph.getQueryNode(i));
    }
    for (auto i = 0u; i < graph.getNumQueryRels(); ++i) {
        pruneRel(*graph.getQueryRel(i));
    }
}

struct Candidates {
    table_id_set_t idSet;
    std::unordered_set<std::string> nameSet;
    bool hasForeignTableWildcard = false;

    void insert(const table_id_set_t& idsToInsert, Catalog* catalog, Transaction* transaction) {
        for (auto id : idsToInsert) {
            // Skip FOREIGN_TABLE_ID as it's a sentinel value, not a real table
            if (id == FOREIGN_TABLE_ID) {
                idSet.insert(id);
                nameSet.insert("<foreign>");
                hasForeignTableWildcard = true;
                continue;
            }
            auto name = catalog->getTableCatalogEntry(transaction, id)->getName();
            idSet.insert(id);
            nameSet.insert(name);
        }
    }

    bool empty() const { return idSet.empty(); }

    bool contains(const table_id_t& id) const {
        // If we have FOREIGN_TABLE_ID wildcard, any foreign table ID matches
        // We can't determine if an ID is foreign just from the ID itself,
        // so we'll handle this in the caller by checking entry type
        return idSet.contains(id) || (hasForeignTableWildcard && id != INVALID_TABLE_ID);
    }

    bool contains(const table_id_t& id, const TableCatalogEntry* entry) const {
        if (idSet.contains(id)) {
            return true;
        }
        // If we have FOREIGN_TABLE_ID wildcard and this is a foreign table, it matches
        if (hasForeignTableWildcard && entry->getType() == CatalogEntryType::FOREIGN_TABLE_ENTRY) {
            return true;
        }
        return false;
    }

    std::string toString() const {
        auto names = std::vector<std::string>{nameSet.begin(), nameSet.end()};
        auto result = names[0];
        for (auto j = 1u; j < names.size(); ++j) {
            result += ", " + names[j];
        }
        return result;
    }
};

void QueryGraphLabelAnalyzer::pruneNode(const QueryGraph& graph, NodeExpression& node) const {
    auto catalog = Catalog::Get(clientContext);
    for (auto i = 0u; i < graph.getNumQueryRels(); ++i) {
        auto queryRel = graph.getQueryRel(i);
        if (queryRel->isRecursive()) {
            continue;
        }
        Candidates candidates;
        auto isSrcConnect = *queryRel->getSrcNode() == node;
        auto isDstConnect = *queryRel->getDstNode() == node;
        auto tx = transaction::Transaction::Get(clientContext);
        if (queryRel->getDirectionType() == RelDirectionType::BOTH) {
            if (isSrcConnect || isDstConnect) {
                for (auto entry : queryRel->getEntries()) {
                    auto& relEntry = entry->constCast<RelGroupCatalogEntry>();
                    candidates.insert(relEntry.getSrcNodeTableIDSet(), catalog, tx);
                    candidates.insert(relEntry.getDstNodeTableIDSet(), catalog, tx);
                }
            }
        } else {
            if (isSrcConnect) {
                for (auto entry : queryRel->getEntries()) {
                    auto& relEntry = entry->constCast<RelGroupCatalogEntry>();
                    candidates.insert(relEntry.getSrcNodeTableIDSet(), catalog, tx);
                }
            } else if (isDstConnect) {
                for (auto entry : queryRel->getEntries()) {
                    auto& relEntry = entry->constCast<RelGroupCatalogEntry>();
                    candidates.insert(relEntry.getDstNodeTableIDSet(), catalog, tx);
                }
            }
        }
        if (candidates.empty()) { // No need to prune.
            continue;
        }
        std::vector<TableCatalogEntry*> prunedEntries;
        for (auto entry : node.getEntries()) {
            if (!candidates.contains(entry->getTableID(), entry)) {
                continue;
            }
            prunedEntries.push_back(entry);
        }
        node.setEntries(prunedEntries);
        if (prunedEntries.empty()) {
            if (throwOnViolate) {
                throw BinderException(
                    stringFormat("Query node {} violates schema. Expected labels are {}.",
                        node.toString(), candidates.toString()));
            }
        }
    }
}

bool hasOverlap(const table_id_set_t& left, const table_id_set_t& right,
    const std::vector<TableCatalogEntry*>& leftEntries,
    const std::vector<TableCatalogEntry*>& rightEntries) {
    // Check for FOREIGN_TABLE_ID wildcard matching
    // FOREIGN_TABLE_ID in right set means it accepts any foreign table from left
    if (right.contains(FOREIGN_TABLE_ID)) {
        // Check if left has any foreign table entries
        for (auto entry : leftEntries) {
            if (entry->getType() == CatalogEntryType::FOREIGN_TABLE_ENTRY) {
                return true;
            }
        }
        // Also check if left IDs contain any foreign table
        // (in case leftEntries is empty but IDs are present)
        if (left.contains(FOREIGN_TABLE_ID)) {
            return true;
        }
    }
    // FOREIGN_TABLE_ID in left set means it accepts any foreign table from right
    if (left.contains(FOREIGN_TABLE_ID)) {
        // Check if right has any foreign table entries
        for (auto entry : rightEntries) {
            if (entry->getType() == CatalogEntryType::FOREIGN_TABLE_ENTRY) {
                return true;
            }
        }
        // Also check if right IDs contain any foreign table
        if (right.contains(FOREIGN_TABLE_ID)) {
            return true;
        }
    }
    // Regular table ID matching
    for (auto id : left) {
        if (right.contains(id)) {
            return true;
        }
    }
    return false;
}

void QueryGraphLabelAnalyzer::pruneRel(RelExpression& rel) const {
    if (rel.isRecursive()) {
        return;
    }
    std::vector<TableCatalogEntry*> prunedEntries;
    auto srcTableIDSet = rel.getSrcNode()->getTableIDsSet();
    auto dstTableIDSet = rel.getDstNode()->getTableIDsSet();
    auto srcEntries = rel.getSrcNode()->getEntries();
    auto dstEntries = rel.getDstNode()->getEntries();

    if (rel.getDirectionType() == RelDirectionType::BOTH) {
        for (auto& entry : rel.getEntries()) {
            auto& relEntry = entry->constCast<RelGroupCatalogEntry>();
            auto relSrcIDSet = relEntry.getSrcNodeTableIDSet();
            auto relDstIDSet = relEntry.getDstNodeTableIDSet();
            // For foreign-backed rels, we don't have real entries to pass, so we pass empty vectors
            // The hasOverlap function will handle FOREIGN_TABLE_ID checking against
            // srcEntries/dstEntries
            std::vector<TableCatalogEntry*> emptyEntries;
            auto fwdSrcOverlap = hasOverlap(srcTableIDSet, relSrcIDSet, srcEntries, emptyEntries);
            auto fwdDstOverlap = hasOverlap(dstTableIDSet, relDstIDSet, dstEntries, emptyEntries);
            auto fwdOverlap = fwdSrcOverlap && fwdDstOverlap;
            auto bwdSrcOverlap = hasOverlap(dstTableIDSet, relSrcIDSet, dstEntries, emptyEntries);
            auto bwdDstOverlap = hasOverlap(srcTableIDSet, relDstIDSet, srcEntries, emptyEntries);
            auto bwdOverlap = bwdSrcOverlap && bwdDstOverlap;
            if (fwdOverlap || bwdOverlap) {
                prunedEntries.push_back(entry);
            }
        }
    } else {
        for (auto& entry : rel.getEntries()) {
            auto& relEntry = entry->constCast<RelGroupCatalogEntry>();
            auto relSrcIDSet = relEntry.getSrcNodeTableIDSet();
            auto relDstIDSet = relEntry.getDstNodeTableIDSet();
            std::vector<TableCatalogEntry*> emptyEntries;
            auto srcOverlap = hasOverlap(srcTableIDSet, relSrcIDSet, srcEntries, emptyEntries);
            auto dstOverlap = hasOverlap(dstTableIDSet, relDstIDSet, dstEntries, emptyEntries);
            if (srcOverlap && dstOverlap) {
                prunedEntries.push_back(entry);
            }
        }
    }
    rel.setEntries(prunedEntries);
    // Note the pruning for node should guarantee the following exception won't be triggered.
    // For safety (and consistency) reason, we still write the check but skip coverage check.
    // LCOV_EXCL_START
    if (prunedEntries.empty()) {
        if (throwOnViolate) {
            throw BinderException(stringFormat("Cannot find a label for relationship {} that "
                                               "connects to all of its neighbour nodes.",
                rel.toString()));
        }
    }
    // LCOV_EXCL_STOP
}

} // namespace binder
} // namespace lbug
