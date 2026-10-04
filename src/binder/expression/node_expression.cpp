#include "binder/expression/node_expression.h"

namespace lbug {
namespace binder {

NodeExpression::~NodeExpression() = default;

std::shared_ptr<Expression> NodeExpression::getPrimaryKey(common::table_id_t tableID) const {
    auto primaryKey = tryGetPrimaryKey(tableID);
    if (primaryKey == nullptr) {
        UNREACHABLE_CODE;
    }
    return primaryKey;
}

std::shared_ptr<Expression> NodeExpression::tryGetPrimaryKey(common::table_id_t tableID) const {
    for (auto& property : propertyExprs) {
        if (property->isPrimaryKey(tableID)) {
            return property;
        }
    }
    return nullptr;
}

} // namespace binder
} // namespace lbug
