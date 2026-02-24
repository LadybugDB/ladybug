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
#if defined(DESER_DEBUG) && (defined(RUNTIME_CHECKS) || !defined(NDEBUG))
    deserializeValue<std::string>(value);
    DASSERT(value == expectedVal);
#endif
    // DO NOTHING
    UNUSED(value);
    UNUSED(expectedVal);
}

} // namespace common
} // namespace lbug
