#include "common/types/json_type.h"

namespace lbug {
namespace common {

constexpr char JsonType::JSON_TYPE_NAME[];

LogicalType JsonType::getJsonType() {
    return LogicalType::JSON();
}

bool JsonType::isJson(const LogicalType& type) {
    return type.getLogicalTypeID() == LogicalTypeID::JSON;
}

} // namespace common
} // namespace lbug
