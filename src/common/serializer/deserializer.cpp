#include "common/serializer/deserializer.h"

namespace lbug {
namespace common {

template<>
void Deserializer::deserializeValue(std::string& value) {
    uint64_t valueLength = 0;
    deserializeValue(valueLength);
    value.resize(valueLength);
    reader->read(reinterpret_cast<uint8_t*>(value.data()), valueLength);
}

void Deserializer::validateDebuggingInfo(std::string& value, const std::string& expectedVal) {
#if defined(LBUG_DESER_DEBUG) && (defined(LBUG_RUNTIME_CHECKS) || !defined(NDEBUG))
    deserializeValue<std::string>(value);
    LBUG_ASSERT(value == expectedVal);
#endif
    // DO NOTHING
    LBUG_UNUSED(value);
    LBUG_UNUSED(expectedVal);
}

} // namespace common
} // namespace lbug
