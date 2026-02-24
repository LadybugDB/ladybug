#include "common/enums/conflict_action.h"

#include "common/assert.h"

namespace lbug {
namespace common {

std::string ConflictActionUtil::toString(ConflictAction action) {
    switch (action) {
    case ConflictAction::ON_CONFLICT_THROW: {
        return "ON_CONFLICT_THROW";
    }
    case ConflictAction::ON_CONFLICT_DO_NOTHING: {
        return "ON_CONFLICT_DO_NOTHING";
    }
    default:
        UNREACHABLE_CODE;
    }
}

} // namespace common
} // namespace lbug
