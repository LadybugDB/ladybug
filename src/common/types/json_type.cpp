#include "common/types/json_type.h"

namespace lbug {
namespace common {

constexpr char JsonType::JSON_TYPE_NAME[];

LogicalType JsonType::getJsonType() {
    auto type = LogicalType(LogicalTypeID::STRING, TypeCategory::UDT);
    type.setExtraTypeInfo(std::make_unique<UDTTypeInfo>(JsonType::JSON_TYPE_NAME));
    return type;
}

bool JsonType::isJson(const LogicalType& type) {
    if (!type.isInternalType() &&
        type.getExtraTypeInfo()->constPtrCast<UDTTypeInfo>()->getTypeName() ==
            JsonType::JSON_TYPE_NAME) {
        return true;
    }
    return false;
}

} // namespace common
} // namespace lbug
