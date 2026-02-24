#include "common/enums/accumulate_type.h"

#include "common/assert.h"

namespace lbug {
namespace common {

std::string AccumulateTypeUtil::toString(AccumulateType type) {
    switch (type) {
    case AccumulateType::REGULAR: {
        return "REGULAR";
    }
    case AccumulateType::OPTIONAL_: {
        return "OPTIONAL";
    }
    default:
        UNREACHABLE_CODE;
    }
}

} // namespace common
} // namespace lbug
