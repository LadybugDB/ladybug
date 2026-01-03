#include "common/enums/drop_type.h"

#include "common/assert.h"

namespace lbug {
namespace common {

std::string DropTypeUtils::toString(DropType type) {
    switch (type) {
    case DropType::TABLE:
        return "Table";
    case DropType::SEQUENCE:
        return "Sequence";
    case DropType::MACRO:
        return "Macro";
    case DropType::GRAPH:
        return "Graph";
    default:
        KU_UNREACHABLE;
    }
}

} // namespace common
} // namespace lbug
