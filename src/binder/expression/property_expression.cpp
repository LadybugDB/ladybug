#include "binder/expression/property_expression.h"

using namespace lbug::common;
using namespace lbug::catalog;

namespace lbug {
namespace binder {

bool PropertyExpression::isPrimaryKey() const {
    // A primary key is unique within its own table, not across the tables a variable spans: the
    // same key value can exist once in each node table, so for a variable over more than one
    // label it identifies no single row. Every caller of this overload reads the answer as that
    // uniqueness guarantee, so a multi-label variable must report false.
    if (!isSingleLabel()) {
        return false;
    }
    for (auto& [id, info] : infos) {
        if (!info.isPrimaryKey) {
            return false;
        }
    }
    return true;
}

bool PropertyExpression::isPrimaryKey(table_id_t tableID) const {
    if (!infos.contains(tableID)) {
        return false;
    }
    return infos.at(tableID).isPrimaryKey;
}

bool PropertyExpression::hasProperty(table_id_t tableID) const {
    DASSERT(infos.contains(tableID));
    return infos.at(tableID).exists;
}

} // namespace binder
} // namespace lbug
