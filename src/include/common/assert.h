#pragma once
#include "common/exception/internal.h"
#include <format>

namespace lbug {
namespace common {

[[noreturn]] inline void lbugAssertFailureInternal(const char* condition_name, const char* file,
    int linenr) {
    // LCOV_EXCL_START
    throw InternalException(std::format("Assertion failed in file \"{}\" on line {}: {}", file,
        linenr, condition_name));
    // LCOV_EXCL_STOP
}

#define LBUG_ASSERT_UNCONDITIONAL(condition)                                                         \
    static_cast<bool>(condition) ?                                                                 \
        void(0) :                                                                                  \
        lbug::common::lbugAssertFailureInternal(#condition, __FILE__, __LINE__)

#if defined(LBUG_RUNTIME_CHECKS) || !defined(NDEBUG)
#define RUNTIME_CHECK(code) code
#define LBUG_ASSERT(condition) LBUG_ASSERT_UNCONDITIONAL(condition)
#else
#define LBUG_ASSERT(condition) void(0)
#define RUNTIME_CHECK(code) void(0)
#endif

#define LBUG_UNREACHABLE                                                                             \
    /* LCOV_EXCL_START */ [[unlikely]] lbug::common::lbugAssertFailureInternal("LBUG_UNREACHABLE",     \
        __FILE__, __LINE__) /* LCOV_EXCL_STOP */
#define LBUG_UNUSED(expr) (void)(expr)

} // namespace common
} // namespace lbug
