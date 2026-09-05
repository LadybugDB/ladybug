#pragma once

#include <string>
#include <vector>

#include "binder/expression/expression.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/enums/extend_direction.h"
#include "common/enums/rel_direction.h"
#include "common/types/types.h"
#include "planner/operator/logical_operator.h"
#include "planner/operator/scan/logical_count_extend_chain.h"

namespace lbug {
namespace planner {

struct LogicalCountAntiEdgeChainPrintInfo final : OPPrintInfo {
    std::string relTableName;
    uint64_t numSuffixHops;

    LogicalCountAntiEdgeChainPrintInfo(std::string relTableName, uint64_t numSuffixHops)
        : relTableName{std::move(relTableName)}, numSuffixHops{numSuffixHops} {}

    std::string toString() const override {
        return "Anti-edge rel: " + relTableName + ", suffix hops: " + std::to_string(numSuffixHops);
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<LogicalCountAntiEdgeChainPrintInfo>(relTableName, numSuffixHops);
    }
};

/**
 * LogicalCountAntiEdgeChain computes COUNT(*) over a path whose first two hops h1, h2 (same rel
 * table R, same node table) are additionally filtered by an anti-edge predicate:
 * NOT(EXISTS{MATCH (n0)-[:R]-(n2)}) and optionally id(n0) <> id(n2). Detected by
 * CountRelTableOptimizer::tryRewriteAntiEdgeChainCount from plans shaped like LSQB q9:
 *
 *   HASH_JOIN[INNER key=n2]
 *     FILTER[NOT(EXISTS{...})]
 *       HASH_JOIN[MARK keys={n0,n1...}]  (anti-edge rows as one side)
 *         chain of R-extends from scan(n1) + anti-edge R-extend over scan(n0)
 *     suffix chain of extends from scan(n2)
 *
 * The two chain hops enumerate rows in directions chainN0Dir/chainN2Dir (FWD, BWD or BOTH) and
 * the anti-edge match enumerates rows in antiEdgeDir. The count is computed with pure count
 * arithmetic (see CountAntiEdgeChain):
 *
 *   count = T - A - S
 *   T = sum over chain rows (n1,n2) of degChainN0(n1)*D(n2)             (all tuples)
 *   A = sum over anti-edge rows (n0,n2) of N'(n0,n2)*D(n2)              (anti-edge present)
 *   S = sum over mid nodes n1, nodes p of multN0(p)*multN2(p)*D(p)      (n0=n2 tuples; only
 *                                                                        when the id<> filter
 *                                                                        is present)
 * where D = per-node suffix path counts, degChainN0(n1) = number of n0-hop rows at n1,
 * multN0/multN2(p) = number of n0/n2-hop rows between n1 and p, and
 * N'(n0,n2) = sum over mid nodes x of multRevN0(n0,x)*multRevN2(n2,x) = number of chain
 * enumerations connecting n0 to n2.
 */
class LogicalCountAntiEdgeChain final : public LogicalOperator {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::COUNT_ANTI_EDGE_CHAIN;

public:
    LogicalCountAntiEdgeChain(std::vector<CountChainHop> suffixHops,
        catalog::RelGroupCatalogEntry* antiRelEntry,
        std::vector<common::table_id_t> antiRelTableIDs, common::table_id_t midNodeTableID,
        common::ExtendDirection chainN0Dir, common::ExtendDirection chainN2Dir,
        common::ExtendDirection antiEdgeDir, bool hasNotEquals,
        std::shared_ptr<binder::Expression> countExpr)
        : LogicalOperator{type_}, suffixHops{std::move(suffixHops)}, antiRelEntry{antiRelEntry},
          antiRelTableIDs{std::move(antiRelTableIDs)}, midNodeTableID{midNodeTableID},
          chainN0Dir{chainN0Dir}, chainN2Dir{chainN2Dir}, antiEdgeDir{antiEdgeDir},
          hasNotEquals{hasNotEquals}, countExpr{std::move(countExpr)} {
        cardinality = 1;
    }

    void computeFactorizedSchema() override;
    void computeFlatSchema() override;

    std::string getExpressionsForPrinting() const override { return countExpr->toString(); }

    const std::vector<CountChainHop>& getSuffixHops() const { return suffixHops; }
    catalog::RelGroupCatalogEntry* getAntiRelEntry() const { return antiRelEntry; }
    const std::vector<common::table_id_t>& getAntiRelTableIDs() const { return antiRelTableIDs; }
    common::table_id_t getMidNodeTableID() const { return midNodeTableID; }
    common::ExtendDirection getChainN0Dir() const { return chainN0Dir; }
    common::ExtendDirection getChainN2Dir() const { return chainN2Dir; }
    common::ExtendDirection getAntiEdgeDir() const { return antiEdgeDir; }
    bool getHasNotEquals() const { return hasNotEquals; }
    std::shared_ptr<binder::Expression> getCountExpr() const { return countExpr; }

    std::unique_ptr<OPPrintInfo> getPrintInfo() const override {
        return std::make_unique<LogicalCountAntiEdgeChainPrintInfo>(antiRelEntry->getName(),
            suffixHops.size());
    }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalCountAntiEdgeChain>(suffixHops, antiRelEntry,
            antiRelTableIDs, midNodeTableID, chainN0Dir, chainN2Dir, antiEdgeDir, hasNotEquals,
            countExpr);
    }

private:
    std::vector<CountChainHop> suffixHops;
    catalog::RelGroupCatalogEntry* antiRelEntry;
    std::vector<common::table_id_t> antiRelTableIDs;
    common::table_id_t midNodeTableID;
    common::ExtendDirection chainN0Dir;
    common::ExtendDirection chainN2Dir;
    common::ExtendDirection antiEdgeDir;
    bool hasNotEquals;
    std::shared_ptr<binder::Expression> countExpr;
};

} // namespace planner
} // namespace lbug
