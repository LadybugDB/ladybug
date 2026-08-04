#pragma once

#include <cstdint>

namespace lbug {
namespace common {

// WIN32 defines DELETE as a macro (0x00010000L) in winnt.h.
#pragma push_macro("DELETE")
#undef DELETE

enum class DeleteNodeType : uint8_t {
    DELETE = 0,
    DETACH_DELETE = 1,
};

#pragma pop_macro("DELETE")

} // namespace common
} // namespace lbug
