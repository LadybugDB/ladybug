#include <cmath>
#include <cstdint>
#include <limits>

#include "alp/encode.hpp"
#include "gtest/gtest.h"

// Regression tests for the float-cast-overflow in alp::AlpEncode<float>::encode_value
// (issue #879). ALP's encodable-range check and its "impossible to encode" sentinel both
// used the int64-scale ENCODING_UPPER_LIMIT/ENCODING_LOWER_LIMIT, but AlpEncode<float>
// encodes to int32_t, so casting that sentinel -- or any value merely inside the int64
// range -- into ENCODED_TYPE was undefined. UBSAN reported it from
// ColumnChunkData::getMetadataToFlush() on any FLOAT column containing such a value.

namespace {
constexpr int64_t kInt32Min = std::numeric_limits<int32_t>::lowest();
constexpr int64_t kInt32Max = std::numeric_limits<int32_t>::max();

int64_t encodeFloat(float value) {
    return static_cast<int64_t>(alp::AlpEncode<float>::encode_value<true>(value, 0, 0));
}
} // namespace

TEST(AlpEncodeTest, FloatSpecialValuesStayInEncodedRange) {
    // Values ALP cannot encode: the sentinel it returns must itself fit in int32_t.
    EXPECT_EQ(encodeFloat(std::numeric_limits<float>::quiet_NaN()), kInt32Max);
    EXPECT_EQ(encodeFloat(std::numeric_limits<float>::infinity()), kInt32Max);
    EXPECT_EQ(encodeFloat(-0.0f), kInt32Max);
}

TEST(AlpEncodeTest, FloatOutsideInt32RangeIsTreatedAsImpossible) {
    // Finite, and well inside the int64 range, but not representable as int32_t. The old
    // range check let these through and the cast at the end of encode_value was undefined.
    for (const float value : {1e12f, -1e12f, 3e9f, -3e9f}) {
        const int64_t encoded = encodeFloat(value);
        EXPECT_GE(encoded, kInt32Min);
        EXPECT_LE(encoded, kInt32Max);
    }
}

TEST(AlpEncodeTest, OrdinaryFloatsStillEncode) {
    EXPECT_EQ(encodeFloat(3.5f), 4);
    EXPECT_EQ(encodeFloat(0.0f), 0);
    EXPECT_EQ(encodeFloat(-2.25f), -2);
}

TEST(AlpEncodeTest, DoubleBehaviourUnchanged) {
    // AlpEncode<double> encodes to int64_t, where the original constants were already
    // correct; this pins that the fix did not move that boundary.
    using D = alp::AlpEncode<double>;
    constexpr double kSentinel = 9223372036854774784.0;
    EXPECT_EQ(D::encode_value<true>(std::numeric_limits<double>::quiet_NaN(), 0, 0),
        static_cast<int64_t>(kSentinel));
    EXPECT_EQ(D::encode_value<true>(-0.0, 0, 0), static_cast<int64_t>(kSentinel));
    EXPECT_EQ(D::encode_value<true>(1e12, 0, 0), 1000000000000LL);
    EXPECT_EQ(D::encode_value<true>(3.5, 0, 0), 4);
}
