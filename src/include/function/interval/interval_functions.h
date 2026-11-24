#pragma once

#include "common/exception/overflow.h"
#include "common/types/interval_t.h"
#include "function/arithmetic/multiply.h"

namespace lbug {
namespace function {

namespace detail {
// Range-checked narrowing for interval fields: extreme to_* inputs must raise
// instead of silently wrapping (see #1691, #1694).
inline int32_t toInt32Checked(int64_t value) {
    if (value > INT32_MAX || value < INT32_MIN) {
        throw common::OverflowException{"Interval value is out of range"};
    }
    return static_cast<int32_t>(value);
}
} // namespace detail

struct ToYears {
    static inline void operation(int64_t& input, common::interval_t& result) {
        int64_t months = 0;
        try {
            Multiply::operation(input, (int64_t)common::Interval::MONTHS_PER_YEAR, months);
        } catch (const common::OverflowException& e) {
            throw common::OverflowException{"Interval value is out of range"};
        }
        result.days = result.micros = 0;
        result.months = detail::toInt32Checked(months);
    }
};

struct ToMonths {
    static inline void operation(int64_t& input, common::interval_t& result) {
        result.days = result.micros = 0;
        result.months = detail::toInt32Checked(input);
    }
};

struct ToDays {
    static inline void operation(int64_t& input, common::interval_t& result) {
        result.micros = result.months = 0;
        result.days = detail::toInt32Checked(input);
    }
};

struct ToHours {
    static inline void operation(int64_t& input, common::interval_t& result) {
        int64_t micros = 0;
        try {
            Multiply::operation(input, common::Interval::MICROS_PER_HOUR, micros);
        } catch (const common::OverflowException& e) {
            throw common::OverflowException{"Interval value is out of range"};
        }
        result.months = result.days = 0;
        result.micros = micros;
    }
};

struct ToMinutes {
    static inline void operation(int64_t& input, common::interval_t& result) {
        int64_t micros = 0;
        try {
            Multiply::operation(input, common::Interval::MICROS_PER_MINUTE, micros);
        } catch (const common::OverflowException& e) {
            throw common::OverflowException{"Interval value is out of range"};
        }
        result.months = result.days = 0;
        result.micros = micros;
    }
};

struct ToSeconds {
    static inline void operation(int64_t& input, common::interval_t& result) {
        int64_t micros = 0;
        try {
            Multiply::operation(input, common::Interval::MICROS_PER_SEC, micros);
        } catch (const common::OverflowException& e) {
            throw common::OverflowException{"Interval value is out of range"};
        }
        result.months = result.days = 0;
        result.micros = micros;
    }
};

struct ToMilliseconds {
    static inline void operation(int64_t& input, common::interval_t& result) {
        int64_t micros = 0;
        try {
            Multiply::operation(input, common::Interval::MICROS_PER_MSEC, micros);
        } catch (const common::OverflowException& e) {
            throw common::OverflowException{"Interval value is out of range"};
        }
        result.months = result.days = 0;
        result.micros = micros;
    }
};

struct ToMicroseconds {
    static inline void operation(int64_t& input, common::interval_t& result) {
        result.months = result.days = 0;
        result.micros = input;
    }
};

} // namespace function
} // namespace lbug
