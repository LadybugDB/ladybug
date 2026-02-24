#pragma once
#include "common/exception/internal.h"
#include <format>

namespace lbug {
namespace common {

[[noreturn]] inline void assertFailureInternal(const char* condition_name, const char* file,
    int linenr) {
    // LCOV_EXCL_START
    throw InternalException(std::format("Assertion failed in file \"{}\" on line {}: {}", file,
        linenr, condition_name));
    // LCOV_EXCL_STOP
}

#define ASSERT(condition)                                                         \
    static_cast<bool>(condition) ?                                                                 \
        void(0) :                                                                                  \
        lbug::common::assertFailureInternal(#condition, __FILE__, __LINE__)

#if defined(RUNTIME_CHECKS) || !defined(NDEBUG)
#define RUNTIME_CHECK(code) code
#define DASSERT(condition) ASSERT(condition)
#else
#define DASSERT(condition) void(0)
#define RUNTIME_CHECK(code) void(0)
#endif

#define UNREACHABLE_CODE                                                                             \
    /* LCOV_EXCL_START */ [[unlikely]] lbug::common::assertFailureInternal("UNREACHABLE_CODE",     \
        __FILE__, __LINE__) /* LCOV_EXCL_STOP */
#define UNUSED(expr) (void)(expr)

} // namespace common
} // namespace lbug
