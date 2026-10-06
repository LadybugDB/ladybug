#pragma once

#include <optional>

#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/vector/value_vector.h"
#include "storage/stats/hyperloglog.h"

namespace lbug {
namespace storage {

class ColumnStats {
public:
    ColumnStats() = default;
    explicit ColumnStats(const common::LogicalType& dataType);
    EXPLICIT_COPY_DEFAULT_MOVE(ColumnStats);

    common::cardinality_t getNumDistinctValues() const { return hll ? hll->count() : 0; }
    // Min/max over non-null values, stored as doubles in the column's physical domain
    // (integers, DATE days, TIMESTAMP micros), for range-selectivity estimation. Unset
    // when no non-null value was seen or the type is not min/max-tracked (nested,
    // strings, ...).
    std::optional<double> getMinValue() const { return minValue; }
    std::optional<double> getMaxValue() const { return maxValue; }

    void update(const common::ValueVector* vector);
    // Tracks min/max for range-selectivity estimation (numerics, dates, timestamps).
    // Other types leave min/max unset.
    void updateMinMax(const common::ValueVector* vector);

    void merge(const ColumnStats& other) {
        if (hll) {
            DASSERT(other.hll);
            hll->merge(*other.hll);
        };
        if (other.minValue.has_value()) {
            minValue = minValue.has_value() ? std::min(*minValue, *other.minValue) : other.minValue;
        }
        if (other.maxValue.has_value()) {
            maxValue = maxValue.has_value() ? std::max(*maxValue, *other.maxValue) : other.maxValue;
        }
    }

    void serialize(common::Serializer& serializer) const {
        serializer.writeDebuggingInfo("has_hll");
        serializer.serializeValue(hll.has_value());
        if (hll) {
            serializer.writeDebuggingInfo("hll");
            hll->serialize(serializer);
        }
        serializer.writeDebuggingInfo("has_min_max");
        serializer.serializeValue(minValue.has_value());
        if (minValue.has_value()) {
            serializer.writeDebuggingInfo("min_max");
            serializer.serializeValue(*minValue);
            serializer.serializeValue(*maxValue);
        }
    }

    static ColumnStats deserialize(common::Deserializer& deserializer) {
        ColumnStats columnStats;
        std::string info;
        deserializer.validateDebuggingInfo(info, "has_hll");
        bool hasHll = false;
        deserializer.deserializeValue(hasHll);
        if (hasHll) {
            deserializer.validateDebuggingInfo(info, "hll");
            columnStats.hll = HyperLogLog::deserialize(deserializer);
        }
        deserializer.validateDebuggingInfo(info, "has_min_max");
        bool hasMinMax = false;
        deserializer.deserializeValue(hasMinMax);
        if (hasMinMax) {
            deserializer.validateDebuggingInfo(info, "min_max");
            double min = 0;
            double max = 0;
            deserializer.deserializeValue(min);
            deserializer.deserializeValue(max);
            columnStats.minValue = min;
            columnStats.maxValue = max;
        }
        return columnStats;
    }

private:
    ColumnStats(const ColumnStats& other)
        : hll{other.hll}, hashes{nullptr}, minValue{other.minValue}, maxValue{other.maxValue} {}

private:
    std::optional<double> minValue;
    std::optional<double> maxValue;
    std::optional<HyperLogLog> hll;
    // Preallocated vector for hash values.
    std::unique_ptr<common::ValueVector> hashes;
};

} // namespace storage
} // namespace lbug
