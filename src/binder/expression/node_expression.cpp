#include "binder/expression/node_expression.h"

namespace lbug {
namespace binder {

NodeExpression::~NodeExpression() = default;

std::shared_ptr<Expression> NodeExpression::getPrimaryKey(common::table_id_t tableID) const {
    for (auto& property : propertyExprs) {
        if (property->isPrimaryKey(tableID)) {
            return property;
        }
    }
    UNREACHABLE_CODE;
}

} // namespace binder
} // namespace lbug
