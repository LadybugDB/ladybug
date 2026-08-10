#pragma once

#include "common/vector/value_vector.h"
#include "function/comparison/comparison_operation.h"

namespace lbug {
namespace function {

struct Equals {
    static constexpr auto comparisonOperation = ComparisonOperation::EQUAL;

    template<class A, class B>
    static inline void operation(const A& left, const B& right, uint8_t& result,
        common::ValueVector* /*leftVector*/, common::ValueVector* /*rightVector*/) {
        result = left == right;
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
        result = left > right;
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
        uint8_t isGreater = 0;
        uint8_t isEqual = 0;
        GreaterThan::operation(left, right, isGreater, leftVector, rightVector);
        Equals::operation(left, right, isEqual, leftVector, rightVector);
        result = isGreater || isEqual;
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
        GreaterThanEquals::operation(left, right, result, leftVector, rightVector);
        result = !result;
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
        GreaterThan::operation(left, right, result, leftVector, rightVector);
        result = !result;
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
