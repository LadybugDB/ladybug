#include "common/enums/rel_multiplicity.h"

#include "common/assert.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include <format>

namespace lbug {
namespace common {

RelMultiplicity RelMultiplicityUtils::getFwd(const std::string& str) {
    auto normStr = common::StringUtils::getUpper(str);
    if ("ONE_ONE" == normStr || "ONE_MANY" == normStr) {
        return RelMultiplicity::ONE;
    } else if ("MANY_ONE" == normStr || "MANY_MANY" == normStr) {
        return RelMultiplicity::MANY;
    }
    throw BinderException(std::format("Cannot bind {} as relationship multiplicity.", str));
}

RelMultiplicity RelMultiplicityUtils::getBwd(const std::string& str) {
    auto normStr = common::StringUtils::getUpper(str);
    if ("ONE_ONE" == normStr || "MANY_ONE" == normStr) {
        return RelMultiplicity::ONE;
    } else if ("ONE_MANY" == normStr || "MANY_MANY" == normStr) {
        return RelMultiplicity::MANY;
    }
    throw BinderException(std::format("Cannot bind {} as relationship multiplicity.", str));
}

std::string RelMultiplicityUtils::toString(RelMultiplicity multiplicity) {
    switch (multiplicity) {
    case RelMultiplicity::ONE:
        return "ONE";
    case RelMultiplicity::MANY:
        return "MANY";
    default:
        KU_UNREACHABLE;
    }
}

} // namespace common
} // namespace lbug
