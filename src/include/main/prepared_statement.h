#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/api.h"
#include "common/types/value/value.h"
#include "query_summary.h"

namespace lbug {
namespace processor {
class PhysicalPlan;
struct ResultSetDescriptor;
} // namespace processor
} // namespace lbug

namespace lbug {
namespace common {
class LogicalType;
}
namespace parser {
class Statement;
}
namespace binder {
class Expression;
}
namespace planner {
class LogicalPlan;
}

namespace main {

// Prepared statement cached in client context and NEVER serialized to client side.
struct CachedPreparedStatement {
    bool useInternalCatalogEntry = false;
    std::shared_ptr<parser::Statement> parsedStatement;
    std::unique_ptr<planner::LogicalPlan> logicalPlan;
    std::vector<std::shared_ptr<binder::Expression>> columns;
    std::vector<std::string> columnNames;

    // Cached physical plan for fast re-execution. When canReuseCachedPlanWith returns true,
    // this operator-tree template is cloned and its sink state is refreshed for each call,
    // avoiding the PlanMapper::mapOperator recursion entirely.
    std::unique_ptr<processor::PhysicalPlan> physicalPlanCache;
    // Cached result-set descriptor. copy() does not propagate the descriptor from an
    // operator tree, so we store it here separately and re-attach it on the cloned sink.
    std::unique_ptr<processor::ResultSetDescriptor> resultSetDescriptor;

    CachedPreparedStatement();
    ~CachedPreparedStatement();

    std::vector<std::string> getColumnNames() const;
    std::vector<common::LogicalType> getColumnTypes() const;
};

/**
 * @brief A prepared statement is a parameterized query which can avoid planning the same query for
 * repeated execution.
 */
class PreparedStatement {
    friend class Connection;
    friend class ClientContext;

public:
    LBUG_API ~PreparedStatement();
    /**
     * @return the query is prepared successfully or not.
     */
    LBUG_API bool isSuccess() const;
    /**
     * @return the error message if the query is not prepared successfully.
     */
    LBUG_API std::string getErrorMessage() const;
    /**
     * @return the prepared statement is read-only or not.
     */
    LBUG_API bool isReadOnly() const;

    const std::unordered_set<std::string>& getUnknownParameters() const {
        return unknownParameters;
    }
    bool canReuseCachedPlanWith(
        const std::unordered_map<std::string, std::unique_ptr<common::Value>>& inputParams) const;
    std::unordered_set<std::string> getKnownParameters();
    void updateParameter(const std::string& name, common::Value* value);
    void addParameter(const std::string& name, common::Value* value);
    LBUG_API void setParameter(const std::string& name, common::Value value);

    std::string getName() const { return cachedPreparedStatementName; }

    common::StatementType getStatementType() const;

    static std::unique_ptr<PreparedStatement> getPreparedStatementWithError(
        const std::string& errorMessage);

private:
    bool success = true;
    bool readOnly = true;
    std::string errMsg;
    PreparedSummary preparedSummary;
    std::string cachedPreparedStatementName;
    std::unordered_set<std::string> unknownParameters;
    std::unordered_map<std::string, std::shared_ptr<common::Value>> parameterMap;
};

} // namespace main
} // namespace lbug
