#include "planner/operator/persistent/logical_insert.h"

#include "binder/expression/node_expression.h"
#include "binder/expression/rel_expression.h"
#include "common/cast.h"
#include "planner/operator/factorization/flatten_resolver.h"

using namespace lbug::common;
using namespace lbug::binder;

namespace lbug {
namespace planner {

void LogicalInsert::computeFactorizedSchema() {
    copyChildSchema(0);
    for (auto& info : infos) {
        auto groupPos = schema->createGroup();
        schema->setGroupAsSingleState(groupPos);
        for (auto i = 0u; i < info.columnExprs.size(); ++i) {
            if (info.isReturnColumnExprs[i]) {
                schema->insertToGroupAndScope(info.columnExprs[i], groupPos);
            }
        }
        if (info.tableType == TableType::NODE) {
            auto node = dynamic_cast_checked<NodeExpression*>(info.pattern.get());
            schema->insertToGroupAndScopeMayRepeat(node->getInternalID(), groupPos);
        } else if (info.tableType == TableType::REL) {
            auto rel = dynamic_cast_checked<RelExpression*>(info.pattern.get());
            schema->insertToGroupAndScopeMayRepeat(rel->getInternalID(), groupPos);
        }
    }
}

void LogicalInsert::computeFlatSchema() {
    copyChildSchema(0);
    for (auto& info : infos) {
        for (auto i = 0u; i < info.columnExprs.size(); ++i) {
            if (info.isReturnColumnExprs[i]) {
                schema->insertToGroupAndScope(info.columnExprs[i], 0);
            }
        }
        if (info.tableType == TableType::NODE) {
            auto node = dynamic_cast_checked<NodeExpression*>(info.pattern.get());
            schema->insertToGroupAndScopeMayRepeat(node->getInternalID(), 0);
        } else if (info.tableType == TableType::REL) {
            auto rel = dynamic_cast_checked<RelExpression*>(info.pattern.get());
            schema->insertToGroupAndScopeMayRepeat(rel->getInternalID(), 0);
        }
    }
}

std::string LogicalInsert::getExpressionsForPrinting() const {
    std::string result;
    for (auto i = 0u; i < infos.size() - 1; ++i) {
        result += infos[i].pattern->toString() + ",";
    }
    result += infos[infos.size() - 1].pattern->toString();
    return result;
}

f_group_pos_set LogicalInsert::getGroupsPosToFlatten() {
    auto childSchema = children[0]->getSchema();
    return FlattenAll::getGroupsPosToFlatten(childSchema->getGroupsPosInScope(), *childSchema);
}

} // namespace planner
} // namespace lbug
