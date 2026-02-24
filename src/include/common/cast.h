#pragma once

#include <typeinfo>

#include "common/assert.h"

namespace lbug {
namespace common {

template<typename TO, typename FROM>
TO dynamic_cast_checked(FROM* old) {
#if defined(RUNTIME_CHECKS) || !defined(NDEBUG)
    static_assert(std::is_pointer<TO>());
    TO newVal = dynamic_cast<TO>(old);
    DASSERT(newVal != nullptr);
    return newVal;
#else
    return reinterpret_cast<TO>(old);
#endif
}

template<typename TO, typename FROM>
TO dynamic_cast_checked(FROM& old) {
#if defined(RUNTIME_CHECKS) || !defined(NDEBUG)
    static_assert(std::is_reference<TO>());
    try {
        TO newVal = dynamic_cast<TO>(old);
        return newVal;
    } catch (std::bad_cast& e) {
        DASSERT(false);
    }
#else
    return reinterpret_cast<TO>(old);
#endif
}

} // namespace common
} // namespace lbug
