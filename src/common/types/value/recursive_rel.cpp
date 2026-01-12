#include "common/types/value/recursive_rel.h"

#include "common/exception/exception.h"
#include "common/types/types.h"
#include "common/types/value/value.h"
#include <format>

namespace lbug {
namespace common {

Value* RecursiveRelVal::getNodes(const Value* val) {
    throwIfNotRecursiveRel(val);
    return val->children[0].get();
}

Value* RecursiveRelVal::getRels(const Value* val) {
    throwIfNotRecursiveRel(val);
    return val->children[1].get();
}

void RecursiveRelVal::throwIfNotRecursiveRel(const Value* val) {
    // LCOV_EXCL_START
    if (val->dataType.getLogicalTypeID() != LogicalTypeID::RECURSIVE_REL) {
        throw Exception(
            std::format("Expected RECURSIVE_REL type, but got {} type", val->dataType.toString()));
    }
    // LCOV_EXCL_STOP
}

} // namespace common
} // namespace lbug
