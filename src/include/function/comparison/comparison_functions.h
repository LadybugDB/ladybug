#pragma once

#include <cmath>
#include <limits>
#include <type_traits>

#include "common/vector/value_vector.h"
#include "function/comparison/comparison_operation.h"

namespace lbug {
namespace function {

// FLOAT and DOUBLE comparisons use a total order, not IEEE comparisons: every NaN equals every
// other NaN and is greater than all other values, including infinity. The SIMD filter kernels,
// hashing, ORDER BY keys, zone-map pruning and the indexes must agree with this order.
template<class T>
bool isNaN(const T& value) {
    if constexpr (std::is_floating_point_v<T>) {
        return std::isnan(value);
    } else {
        return false;
    }
}

// The value that represents `value` among the values Equals treats as equal to it: +0.0 for both
// zeros and the quiet NaN for every NaN. Byte-encoded keys use it so that equal values share a key.
template<class T>
T canonicalFloat(T value) {
    static_assert(std::is_floating_point_v<T>);
    if (value == 0) {
        return T{0};
    }
    if (std::isnan(value)) {
        return std::numeric_limits<T>::quiet_NaN();
    }
    return value;
}

template<class A, class B>
constexpr bool hasFloatingOperand = std::is_floating_point_v<A> || std::is_floating_point_v<B>;

struct Equals {
    static constexpr auto comparisonOperation = ComparisonOperation::EQUAL;

    template<class A, class B>
    static inline void operation(const A& left, const B& right, uint8_t& result,
        common::ValueVector* /*leftVector*/, common::ValueVector* /*rightVector*/) {
        if constexpr (hasFloatingOperand<A, B>) {
            result = left == right || (isNaN(left) && isNaN(right));
        } else {
            result = left == right;
        }
    }

    template<class T>
    static bool operation(const T& left, const T& right) {
        uint8_t result = 0;
        operation<T>(left, right, result, nullptr, nullptr);
        return result;
    }
};

struct NotEquals {
    static constexpr auto comparisonOperation = ComparisonOperation::NOT_EQUAL;

    template<class A, class B>
    static inline void operation(const A& left, const B& right, uint8_t& result,
        common::ValueVector* leftVector, common::ValueVector* rightVector) {
        Equals::operation(left, right, result, leftVector, rightVector);
        result = !result;
    }

    template<class T>
    static bool operation(const T& left, const T& right) {
        uint8_t result = 0;
        operation<T>(left, right, result, nullptr, nullptr);
        return result;
    }
};

struct GreaterThan {
    static constexpr auto comparisonOperation = ComparisonOperation::GREATER_THAN;

    template<class A, class B>
    static inline void operation(const A& left, const B& right, uint8_t& result,
        common::ValueVector* /*leftVector*/, common::ValueVector* /*rightVector*/) {
        if constexpr (hasFloatingOperand<A, B>) {
            // `!(left <= right)` also holds when either side is NaN; testing it first keeps the
            // common case to one comparison.
            result = !(left <= right) && !isNaN(right);
        } else {
            result = left > right;
        }
    }

    template<class T>
    static bool operation(const T& left, const T& right) {
        uint8_t result = 0;
        operation<T>(left, right, result, nullptr, nullptr);
        return result;
    }
};

struct GreaterThanEquals {
    static constexpr auto comparisonOperation = ComparisonOperation::GREATER_THAN_EQUAL;

    template<class A, class B>
    static inline void operation(const A& left, const B& right, uint8_t& result,
        common::ValueVector* leftVector, common::ValueVector* rightVector) {
        // The float branches of this and the next two functors are GreaterThan and Equals combined
        // into one comparison and a NaN check.
        if constexpr (hasFloatingOperand<A, B>) {
            result = left >= right || isNaN(left);
        } else {
            uint8_t isGreater = 0;
            uint8_t isEqual = 0;
            GreaterThan::operation(left, right, isGreater, leftVector, rightVector);
            Equals::operation(left, right, isEqual, leftVector, rightVector);
            result = isGreater || isEqual;
        }
    }

    template<class T>
    static bool operation(const T& left, const T& right) {
        uint8_t result = 0;
        operation<T>(left, right, result, nullptr, nullptr);
        return result;
    }
};

struct LessThan {
    static constexpr auto comparisonOperation = ComparisonOperation::LESS_THAN;

    template<class A, class B>
    static inline void operation(const A& left, const B& right, uint8_t& result,
        common::ValueVector* leftVector, common::ValueVector* rightVector) {
        if constexpr (hasFloatingOperand<A, B>) {
            result = !(left >= right) && !isNaN(left);
        } else {
            GreaterThanEquals::operation(left, right, result, leftVector, rightVector);
            result = !result;
        }
    }

    template<class T>
    static bool operation(const T& left, const T& right) {
        uint8_t result = 0;
        operation<T>(left, right, result, nullptr, nullptr);
        return result;
    }
};

struct LessThanEquals {
    static constexpr auto comparisonOperation = ComparisonOperation::LESS_THAN_EQUAL;

    template<class A, class B>
    static inline void operation(const A& left, const B& right, uint8_t& result,
        common::ValueVector* leftVector, common::ValueVector* rightVector) {
        if constexpr (hasFloatingOperand<A, B>) {
            result = left <= right || isNaN(right);
        } else {
            GreaterThan::operation(left, right, result, leftVector, rightVector);
            result = !result;
        }
    }

    template<class T>
    static bool operation(const T& left, const T& right) {
        uint8_t result = 0;
        operation<T>(left, right, result, nullptr, nullptr);
        return result;
    }
};

// specialization for equal and greater than.
template<>
void Equals::operation(const common::list_entry_t& left, const common::list_entry_t& right,
    uint8_t& result, common::ValueVector* leftVector, common::ValueVector* rightVector);
template<>
void Equals::operation(const common::struct_entry_t& left, const common::struct_entry_t& right,
    uint8_t& result, common::ValueVector* leftVector, common::ValueVector* rightVector);
template<>
void GreaterThan::operation(const common::list_entry_t& left, const common::list_entry_t& right,
    uint8_t& result, common::ValueVector* leftVector, common::ValueVector* rightVector);
template<>
void GreaterThan::operation(const common::struct_entry_t& left, const common::struct_entry_t& right,
    uint8_t& result, common::ValueVector* leftVector, common::ValueVector* rightVector);

} // namespace function
} // namespace lbug
