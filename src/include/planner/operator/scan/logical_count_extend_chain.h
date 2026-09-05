#pragma once

#include <string>
#include <vector>

#include "binder/expression/expression.h"
#include "common/enums/rel_direction.h"
#include "common/types/types.h"
#include "planner/operator/logical_operator.h"

namespace lbug {
namespace planner {

// A single rel-table scan participating in one hop of a count chain. The CSR of the rel table
// is scanned in `scanDirection`, which is the direction keyed by the hop's "from" node table:
// FWD when the from-node is the rel's src side, BWD when it is the dst side. Iterating the
// from-side node offsets and reading the nbr column then yields exactly the (from, to) edges
// crossing this hop.
struct CountChainRelScanSpec {
    common::table_id_t relTableID = common::INVALID_TABLE_ID;
    common::RelDataDirection scanDirection = common::RelDataDirection::FWD;
    common::table_id_t fromNodeTableID = common::INVALID_TABLE_ID;
    common::table_id_t toNodeTableID = common::INVALID_TABLE_ID;
    std::string relTableName; // for printing
};

// One hop of the extend chain. A hop may cover several rel tables (e.g. multi-entry rel groups
// matched against the same node table pair).
struct CountChainHop {
    std::vector<CountChainRelScanSpec> relScans;
};

struct LogicalCountExtendChainPrintInfo final : OPPrintInfo {
    std::vector<std::string> relTableNames;
    uint64_t numHops;

    LogicalCountExtendChainPrintInfo(std::vector<std::string> relTableNames, uint64_t numHops)
        : relTableNames{std::move(relTableNames)}, numHops{numHops} {}

    std::string toString() const override {
        auto result = std::string("Tables: ");
        for (auto i = 0u; i < relTableNames.size(); ++i) {
            result += relTableNames[i];
            if (i + 1 < relTableNames.size()) {
                result += ", ";
            }
        }
        result += ", Hops: " + std::to_string(numHops);
        return result;
    }

    std::unique_ptr<OPPrintInfo> copy() const override {
        return std::make_unique<LogicalCountExtendChainPrintInfo>(relTableNames, numHops);
    }
};

/**
 * LogicalCountExtendChain computes COUNT(*) over a pure chain of extends (a path in the query
 * graph) using count-only arithmetic instead of materializing tuples. Created by
 * CountRelTableOptimizer::tryRewriteExtendChainCount when the plan under a COUNT(*) aggregate is
 * an unfiltered chain of extends (possibly connected by hash joins on node IDs).
 *
 * The count is the number of paths through the chain, which can be computed hop by hop: each
 * hop accumulates, per destination node, the sum of the partial path counts of its source
 * nodes. Only per-node count vectors (one int64 per node offset) are propagated; neighbor IDs
 * are read from the CSR neighbor column but never materialized as tuples, and no hash joins are
 * required.
 */
class LogicalCountExtendChain final : public LogicalOperator {
    static constexpr LogicalOperatorType type_ = LogicalOperatorType::COUNT_EXTEND_CHAIN;

public:
    LogicalCountExtendChain(std::vector<CountChainHop> hops,
        std::shared_ptr<binder::Expression> countExpr)
        : LogicalOperator{type_}, hops{std::move(hops)}, countExpr{std::move(countExpr)} {
        cardinality = 1; // Always returns exactly one row.
    }

    void computeFactorizedSchema() override;
    void computeFlatSchema() override;

    std::string getExpressionsForPrinting() const override { return countExpr->toString(); }

    const std::vector<CountChainHop>& getHops() const { return hops; }
    std::shared_ptr<binder::Expression> getCountExpr() const { return countExpr; }

    std::unique_ptr<OPPrintInfo> getPrintInfo() const override {
        std::vector<std::string> relTableNames;
        for (auto& hop : hops) {
            for (auto& spec : hop.relScans) {
                relTableNames.push_back(spec.relTableName);
            }
        }
        return std::make_unique<LogicalCountExtendChainPrintInfo>(std::move(relTableNames),
            hops.size());
    }

    std::unique_ptr<LogicalOperator> copy() override {
        return std::make_unique<LogicalCountExtendChain>(hops, countExpr);
    }

private:
    std::vector<CountChainHop> hops;
    std::shared_ptr<binder::Expression> countExpr;
};

} // namespace planner
} // namespace lbug
