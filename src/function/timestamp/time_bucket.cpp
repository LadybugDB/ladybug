#include "function/timestamp/time_bucket.h"

#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#include "binder/expression/literal_expression.h"
#include "common/exception/binder.h"
#include "common/exception/overflow.h"
#include "common/types/date_t.h"
#include "common/types/int128_t.h"
#include "common/types/interval_t.h"
#include "common/types/timestamp_t.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace function {

namespace {

// Ladybug stores DATE and TIMESTAMP values relative to the Unix epoch.
const int64_t EPOCH_ANCHOR_MICROS = 0;

struct TimeBucketBindData final : FunctionBindData {
    explicit TimeBucketBindData(std::vector<LogicalType> paramTypes, LogicalType resultType,
        interval_t interval)
        : FunctionBindData{std::move(paramTypes), std::move(resultType)}, interval{interval} {}

    std::unique_ptr<FunctionBindData> copy() const override {
        return std::make_unique<TimeBucketBindData>(LogicalType::copy(paramTypes),
            resultType.copy(), interval);
    }

    interval_t interval;
};

static int128_t floorDiv(int128_t value, int128_t divisor) {
    auto quotient = value / divisor;
    if (value % divisor < 0) {
        quotient = quotient - 1;
    }
    return quotient;
}

static int64_t fixedDurationMicros(const interval_t& interval) {
    const auto duration =
        int128_t{interval.days} * Interval::MICROS_PER_DAY + int128_t{interval.micros};
    if (duration <= 0 || duration > std::numeric_limits<int64_t>::max()) {
        throw BinderException("time_bucket interval must be positive and representable.");
    }
    return static_cast<int64_t>(duration);
}

static interval_t multiplyInterval(const interval_t& interval, int64_t multiplier) {
    const auto months = int128_t{interval.months} * multiplier;
    const auto days = int128_t{interval.days} * multiplier;
    const auto micros = int128_t{interval.micros} * multiplier;
    if (months < std::numeric_limits<int32_t>::min() ||
        months > std::numeric_limits<int32_t>::max() ||
        days < std::numeric_limits<int32_t>::min() || days > std::numeric_limits<int32_t>::max() ||
        micros < std::numeric_limits<int64_t>::min() ||
        micros > std::numeric_limits<int64_t>::max()) {
        throw OverflowException("time_bucket interval multiplier is out of range.");
    }
    return interval_t{static_cast<int32_t>(months), static_cast<int32_t>(days),
        static_cast<int64_t>(micros)};
}

static timestamp_t calendarBucket(timestamp_t value, const interval_t& interval) {
    const timestamp_t anchor{EPOCH_ANCHOR_MICROS};
    auto candidate = [&](int64_t index) { return anchor + multiplyInterval(interval, index); };
    int64_t low = 0;
    int64_t high = 0;
    if (value >= anchor) {
        high = 1;
        while (candidate(high) <= value) {
            low = high;
            if (high > std::numeric_limits<int32_t>::max() / 2) {
                throw OverflowException("time_bucket calendar bucket is out of range.");
            }
            high *= 2;
        }
    } else {
        low = -1;
        while (candidate(low) > value) {
            high = low;
            if (low < std::numeric_limits<int32_t>::min() / 2) {
                throw OverflowException("time_bucket calendar bucket is out of range.");
            }
            low *= 2;
        }
    }
    while (high - low > 1) {
        const auto mid = low + (high - low) / 2;
        if (candidate(mid) <= value) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return candidate(low);
}

static timestamp_t bucketTimestamp(timestamp_t value, const interval_t& interval) {
    if (interval.months != 0) {
        return calendarBucket(value, interval);
    }
    const auto width = fixedDurationMicros(interval);
    const auto result = floorDiv(value.value, width) * width;
    if (result < std::numeric_limits<int64_t>::min() ||
        result > std::numeric_limits<int64_t>::max()) {
        throw OverflowException("time_bucket result is out of TIMESTAMP range.");
    }
    return timestamp_t{static_cast<int64_t>(result)};
}

static date_t bucketDate(date_t value, const interval_t& interval) {
    if (interval.micros % Interval::MICROS_PER_DAY != 0) {
        throw BinderException("time_bucket DATE interval must be a whole number of days.");
    }
    const auto timestamp = Timestamp::fromDateTime(value, dtime_t{0});
    return Timestamp::getDate(bucketTimestamp(timestamp, interval));
}

template<typename T>
static T bucketTimestampValue(T value, const interval_t& interval) {
    if (interval.months == 0) {
        const auto widthMicros = fixedDurationMicros(interval);
        int128_t bucketValue{0};
        if constexpr (std::is_same_v<T, timestamp_sec_t>) {
            const auto width = widthMicros / Interval::MICROS_PER_SEC;
            bucketValue = floorDiv(value.value, width) * width;
        } else if constexpr (std::is_same_v<T, timestamp_ms_t>) {
            const auto width = widthMicros / Interval::MICROS_PER_MSEC;
            bucketValue = floorDiv(value.value, width) * width;
        } else if constexpr (std::is_same_v<T, timestamp_ns_t>) {
            const auto width = int128_t{widthMicros} * Interval::NANOS_PER_MICRO;
            bucketValue = floorDiv(value.value, width) * width;
        } else {
            bucketValue = floorDiv(value.value, widthMicros) * widthMicros;
        }
        if (bucketValue < std::numeric_limits<int64_t>::min() ||
            bucketValue > std::numeric_limits<int64_t>::max()) {
            throw OverflowException("time_bucket result is out of range.");
        }
        return T{static_cast<int64_t>(bucketValue)};
    }

    timestamp_t timestamp;
    if constexpr (std::is_same_v<T, timestamp_sec_t>) {
        timestamp = Timestamp::fromEpochSeconds(value.value);
    } else if constexpr (std::is_same_v<T, timestamp_ms_t>) {
        timestamp = Timestamp::fromEpochMilliSeconds(value.value);
    } else if constexpr (std::is_same_v<T, timestamp_ns_t>) {
        timestamp =
            timestamp_t{static_cast<int64_t>(floorDiv(value.value, Interval::NANOS_PER_MICRO))};
    } else {
        timestamp = timestamp_t{value.value};
    }
    const auto result = bucketTimestamp(timestamp, interval);
    if constexpr (std::is_same_v<T, timestamp_sec_t>) {
        return timestamp_sec_t{Timestamp::getEpochSeconds(result)};
    } else if constexpr (std::is_same_v<T, timestamp_ms_t>) {
        return timestamp_ms_t{Timestamp::getEpochMilliSeconds(result)};
    } else if constexpr (std::is_same_v<T, timestamp_ns_t>) {
        const auto resultNanos = int128_t{result.value} * Interval::NANOS_PER_MICRO;
        if (resultNanos < std::numeric_limits<int64_t>::min() ||
            resultNanos > std::numeric_limits<int64_t>::max()) {
            throw OverflowException("time_bucket result is out of TIMESTAMP_NS range.");
        }
        return timestamp_ns_t{static_cast<int64_t>(resultNanos)};
    } else {
        return T{result.value};
    }
}

template<typename T>
static void executeTimestamp(const ValueVector& input, SelectionVector* inputSel,
    ValueVector& result, SelectionVector* resultSel, const interval_t& interval) {
    const auto inputUnfiltered = inputSel->isUnfiltered();
    const auto resultUnfiltered = resultSel->isUnfiltered();
    for (auto i = 0u; i < inputSel->getSelSize(); ++i) {
        const auto inputPos = inputUnfiltered ? i : (*inputSel)[i];
        const auto resultPos = resultUnfiltered ? i : (*resultSel)[i];
        result.setNull(resultPos, input.isNull(inputPos));
        if (!result.isNull(resultPos)) {
            result.setValue(resultPos, bucketTimestampValue(input.getValue<T>(inputPos), interval));
        }
    }
}

static void executeDate(const ValueVector& input, SelectionVector* inputSel, ValueVector& result,
    SelectionVector* resultSel, const interval_t& interval) {
    const auto inputUnfiltered = inputSel->isUnfiltered();
    const auto resultUnfiltered = resultSel->isUnfiltered();
    for (auto i = 0u; i < inputSel->getSelSize(); ++i) {
        const auto inputPos = inputUnfiltered ? i : (*inputSel)[i];
        const auto resultPos = resultUnfiltered ? i : (*resultSel)[i];
        result.setNull(resultPos, input.isNull(inputPos));
        if (!result.isNull(resultPos)) {
            result.setValue(resultPos, bucketDate(input.getValue<date_t>(inputPos), interval));
        }
    }
}

static void execFunc(const std::vector<std::shared_ptr<ValueVector>>& parameters,
    const std::vector<SelectionVector*>& parameterSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* dataPtr) {
    DASSERT(parameters.size() == 2);
    const auto& bindData = *reinterpret_cast<TimeBucketBindData*>(dataPtr);
    const auto& input = *parameters[1];
    switch (input.dataType.getLogicalTypeID()) {
    case LogicalTypeID::DATE:
        executeDate(input, parameterSelVectors[1], result, resultSelVector, bindData.interval);
        return;
    case LogicalTypeID::TIMESTAMP:
        executeTimestamp<timestamp_t>(input, parameterSelVectors[1], result, resultSelVector,
            bindData.interval);
        return;
    case LogicalTypeID::TIMESTAMP_TZ:
        executeTimestamp<timestamp_tz_t>(input, parameterSelVectors[1], result, resultSelVector,
            bindData.interval);
        return;
    case LogicalTypeID::TIMESTAMP_SEC:
        executeTimestamp<timestamp_sec_t>(input, parameterSelVectors[1], result, resultSelVector,
            bindData.interval);
        return;
    case LogicalTypeID::TIMESTAMP_MS:
        executeTimestamp<timestamp_ms_t>(input, parameterSelVectors[1], result, resultSelVector,
            bindData.interval);
        return;
    case LogicalTypeID::TIMESTAMP_NS:
        executeTimestamp<timestamp_ns_t>(input, parameterSelVectors[1], result, resultSelVector,
            bindData.interval);
        return;
    default:
        throw BinderException("time_bucket requires a Ladybug date/time value.");
    }
}

static std::unique_ptr<FunctionBindData> bindFunc(const ScalarBindFuncInput& input) {
    if (input.arguments.size() != 2 ||
        input.arguments[0]->expressionType != ExpressionType::LITERAL) {
        throw BinderException("First parameter of time_bucket must be a constant INTERVAL.");
    }
    const auto& intervalValue = input.arguments[0]->constPtrCast<LiteralExpression>()->getValue();
    if (intervalValue.isNull()) {
        throw BinderException("First parameter of time_bucket must be a non-null INTERVAL.");
    }
    const auto interval = intervalValue.getValue<interval_t>();
    if (interval.months < 0 || interval.days < 0 || interval.micros < 0 ||
        (interval.months == 0 && interval.days == 0 && interval.micros == 0)) {
        throw BinderException("time_bucket interval must be positive.");
    }
    if (interval.months == 0) {
        fixedDurationMicros(interval);
    }
    const auto typeID = input.arguments[1]->dataType.getLogicalTypeID();
    if (typeID == LogicalTypeID::DATE && interval.micros % Interval::MICROS_PER_DAY != 0) {
        throw BinderException("time_bucket DATE interval must be a whole number of days.");
    }
    if (typeID == LogicalTypeID::TIMESTAMP_SEC && interval.micros % Interval::MICROS_PER_SEC != 0) {
        throw BinderException("time_bucket interval must align with TIMESTAMP_SEC precision.");
    }
    if (typeID == LogicalTypeID::TIMESTAMP_MS && interval.micros % Interval::MICROS_PER_MSEC != 0) {
        throw BinderException("time_bucket interval must align with TIMESTAMP_MS precision.");
    }
    std::vector<LogicalType> paramTypes;
    paramTypes.push_back(input.arguments[0]->dataType.copy());
    paramTypes.push_back(input.arguments[1]->dataType.copy());
    return std::make_unique<TimeBucketBindData>(std::move(paramTypes),
        input.arguments[1]->dataType.copy(), interval);
}

} // namespace

function_set TimeBucketFunction::getFunctionSet() {
    function_set result;
    const std::vector<LogicalTypeID> dateTimeTypes{LogicalTypeID::DATE, LogicalTypeID::TIMESTAMP,
        LogicalTypeID::TIMESTAMP_SEC, LogicalTypeID::TIMESTAMP_MS, LogicalTypeID::TIMESTAMP_NS,
        LogicalTypeID::TIMESTAMP_TZ};
    for (const auto typeID : dateTimeTypes) {
        auto function = std::make_unique<ScalarFunction>(name,
            std::vector<LogicalTypeID>{LogicalTypeID::INTERVAL, typeID}, typeID, execFunc);
        function->bindFunc = bindFunc;
        result.push_back(std::move(function));
    }
    return result;
}

} // namespace function
} // namespace lbug
