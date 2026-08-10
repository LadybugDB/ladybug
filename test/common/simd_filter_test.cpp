#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#include "common/data_chunk/data_chunk_state.h"
#include "common/data_chunk/sel_vector.h"
#include "common/null_mask.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "function/binary_function_executor.h"
#include "function/comparison/comparison_functions.h"
#include "function/comparison/simd_filter.h"
#include "gtest/gtest.h"

using namespace lbug;

namespace {

template<typename T>
bool evaluateComparison(T left, T right, function::ComparisonOperation operation) {
    switch (operation) {
    case function::ComparisonOperation::EQUAL:
        return function::Equals::operation(left, right);
    case function::ComparisonOperation::NOT_EQUAL:
        return function::NotEquals::operation(left, right);
    case function::ComparisonOperation::GREATER_THAN:
        return function::GreaterThan::operation(left, right);
    case function::ComparisonOperation::GREATER_THAN_EQUAL:
        return function::GreaterThanEquals::operation(left, right);
    case function::ComparisonOperation::LESS_THAN:
        return function::LessThan::operation(left, right);
    case function::ComparisonOperation::LESS_THAN_EQUAL:
        return function::LessThanEquals::operation(left, right);
    }
    return false;
}

constexpr std::array COMPARISON_OPERATIONS = {function::ComparisonOperation::EQUAL,
    function::ComparisonOperation::NOT_EQUAL, function::ComparisonOperation::GREATER_THAN,
    function::ComparisonOperation::GREATER_THAN_EQUAL, function::ComparisonOperation::LESS_THAN,
    function::ComparisonOperation::LESS_THAN_EQUAL};

template<typename T>
void verifyIntegerKernels(common::PhysicalTypeID type) {
    constexpr common::sel_t COUNT = 257;
    std::vector<T> data(COUNT);
    T constant;
    if constexpr (std::is_unsigned_v<T>) {
        constant = static_cast<T>(T{1} << (sizeof(T) * 8 - 1));
        for (common::sel_t i = 0; i < COUNT; ++i) {
            switch (i % 5) {
            case 0:
                data[i] = 0;
                break;
            case 1:
                data[i] = constant - 1;
                break;
            case 2:
                data[i] = constant;
                break;
            case 3:
                data[i] = constant + 1;
                break;
            default:
                data[i] = std::numeric_limits<T>::max();
            }
        }
    } else {
        constant = static_cast<T>(47);
        for (common::sel_t i = 0; i < COUNT; ++i) {
            data[i] = static_cast<T>((i * 37 + 11) % 101);
        }
        data[0] = std::numeric_limits<T>::min();
        data[1] = std::numeric_limits<T>::max();
    }
    common::NullMask nullMask{COUNT};
    for (common::sel_t i = 3; i < COUNT; i += 17) {
        nullMask.setNull(i, true);
    }
    for (const auto i : {63u, 64u, 127u, 128u}) {
        nullMask.setNull(i, true);
    }

    std::vector<common::sel_t> scalar(COUNT);
    std::vector<common::sel_t> dispatched(COUNT);
    std::vector<common::sel_t> expected;
    expected.reserve(COUNT);
    for (const auto operation : COMPARISON_OPERATIONS) {
        const auto scalarCount = function::simd::selectConstantScalar(data.data(), COUNT, &constant,
            nullMask.getData(), scalar.data(), type, operation);
        const auto dispatchedCount = function::simd::selectConstant(data.data(), COUNT, &constant,
            nullMask.getData(), dispatched.data(), type, operation);
        expected.clear();
        for (common::sel_t i = 0; i < COUNT; ++i) {
            if (!nullMask.isNull(i) && evaluateComparison(data[i], constant, operation)) {
                expected.push_back(i);
            }
        }
        ASSERT_EQ(scalarCount, expected.size());
        ASSERT_EQ(dispatchedCount, expected.size());
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), scalar.begin()));
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), dispatched.begin()));
    }

    std::vector<T> right(COUNT);
    for (common::sel_t i = 0; i < COUNT; ++i) {
        right[i] = data[(i * 13 + 7) % COUNT];
    }
    common::NullMask rightNullMask{COUNT};
    for (const auto i : {5u, 64u, 126u, 127u, 192u}) {
        rightNullMask.setNull(i, true);
    }
    for (const auto operation : COMPARISON_OPERATIONS) {
        const auto scalarCount = function::simd::selectVectorScalar(data.data(), right.data(),
            COUNT, nullMask.getData(), rightNullMask.getData(), scalar.data(), type, operation);
        const auto dispatchedCount = function::simd::selectVector(data.data(), right.data(), COUNT,
            nullMask.getData(), rightNullMask.getData(), dispatched.data(), type, operation);
        expected.clear();
        for (common::sel_t i = 0; i < COUNT; ++i) {
            if (!nullMask.isNull(i) && !rightNullMask.isNull(i) &&
                evaluateComparison(data[i], right[i], operation)) {
                expected.push_back(i);
            }
        }
        ASSERT_EQ(scalarCount, expected.size());
        ASSERT_EQ(dispatchedCount, expected.size());
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), scalar.begin()));
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), dispatched.begin()));
    }
}

template<typename T>
void verifyFloatingKernel(common::PhysicalTypeID type) {
    constexpr common::sel_t COUNT = 257;
    std::vector<T> data(COUNT);
    for (common::sel_t i = 0; i < COUNT; ++i) {
        data[i] = static_cast<T>(static_cast<int64_t>(i % 17) - 8);
    }
    data[0] = -std::numeric_limits<T>::infinity();
    data[1] = -T{0};
    data[2] = T{0};
    data[31] = std::numeric_limits<T>::infinity();
    data[32] = std::numeric_limits<T>::quiet_NaN();
    data[129] = std::numeric_limits<T>::signaling_NaN();
    const T constant = T{1};
    common::NullMask nullMask{COUNT};
    for (const auto i : {17u, 63u, 64u, 127u, 128u, 255u}) {
        nullMask.setNull(i, true);
    }

    std::vector<common::sel_t> scalar(COUNT);
    std::vector<common::sel_t> dispatched(COUNT);
    for (const auto operation : COMPARISON_OPERATIONS) {
        const auto scalarCount = function::simd::selectConstantScalar(data.data(), COUNT, &constant,
            nullMask.getData(), scalar.data(), type, operation);
        const auto dispatchedCount = function::simd::selectConstant(data.data(), COUNT, &constant,
            nullMask.getData(), dispatched.data(), type, operation);
        std::vector<common::sel_t> expected;
        for (common::sel_t i = 0; i < COUNT; ++i) {
            if (!nullMask.isNull(i) && evaluateComparison(data[i], constant, operation)) {
                expected.push_back(i);
            }
        }
        ASSERT_EQ(scalarCount, expected.size());
        ASSERT_EQ(dispatchedCount, expected.size());
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), scalar.begin()));
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), dispatched.begin()));
    }

    std::vector<T> right(COUNT);
    for (common::sel_t i = 0; i < COUNT; ++i) {
        right[i] = data[(i * 19 + 3) % COUNT];
    }
    right[65] = std::numeric_limits<T>::quiet_NaN();
    common::NullMask rightNullMask{COUNT};
    for (const auto i : {5u, 64u, 126u, 127u, 192u}) {
        rightNullMask.setNull(i, true);
    }
    for (const auto operation : COMPARISON_OPERATIONS) {
        const auto scalarCount = function::simd::selectVectorScalar(data.data(), right.data(),
            COUNT, nullMask.getData(), rightNullMask.getData(), scalar.data(), type, operation);
        const auto dispatchedCount = function::simd::selectVector(data.data(), right.data(), COUNT,
            nullMask.getData(), rightNullMask.getData(), dispatched.data(), type, operation);
        std::vector<common::sel_t> expected;
        for (common::sel_t i = 0; i < COUNT; ++i) {
            if (!nullMask.isNull(i) && !rightNullMask.isNull(i) &&
                evaluateComparison(data[i], right[i], operation)) {
                expected.push_back(i);
            }
        }
        ASSERT_EQ(scalarCount, expected.size());
        ASSERT_EQ(dispatchedCount, expected.size());
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), scalar.begin()));
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), dispatched.begin()));
    }
}

} // namespace

TEST(SIMDFilterTest, KernelsMatchIntegerComparisons) {
    verifyIntegerKernels<int8_t>(common::PhysicalTypeID::INT8);
    verifyIntegerKernels<int16_t>(common::PhysicalTypeID::INT16);
    verifyIntegerKernels<int32_t>(common::PhysicalTypeID::INT32);
    verifyIntegerKernels<int64_t>(common::PhysicalTypeID::INT64);
    verifyIntegerKernels<uint8_t>(common::PhysicalTypeID::UINT8);
    verifyIntegerKernels<uint16_t>(common::PhysicalTypeID::UINT16);
    verifyIntegerKernels<uint32_t>(common::PhysicalTypeID::UINT32);
    verifyIntegerKernels<uint64_t>(common::PhysicalTypeID::UINT64);
}

TEST(SIMDFilterTest, KernelsPreserveFloatingPointSemantics) {
    verifyFloatingKernel<float>(common::PhysicalTypeID::FLOAT);
    verifyFloatingKernel<double>(common::PhysicalTypeID::DOUBLE);
}

TEST(SIMDFilterTest, BinaryExecutorHandlesBothConstantDirections) {
    constexpr common::sel_t COUNT = 128;
    auto dataState = std::make_shared<common::DataChunkState>(COUNT);
    dataState->setToUnflat();
    dataState->initOriginalAndSelectedSize(COUNT);
    common::ValueVector data{common::LogicalType::INT64(), nullptr, dataState};
    for (common::sel_t i = 0; i < COUNT; ++i) {
        data.setValue<int64_t>(i, static_cast<int64_t>(i));
    }
    data.setNull(73, true);

    auto constantState = common::DataChunkState::getSingleValueDataChunkState();
    common::ValueVector constant{common::LogicalType::INT64(), nullptr, constantState};
    constant.setValue<int64_t>(0, 63);

    common::SelectionVector output{COUNT};
    ASSERT_TRUE((
        function::BinaryFunctionExecutor::selectComparison<int64_t, int64_t, function::GreaterThan>(
            data, constant, output, nullptr)));
    ASSERT_EQ(output.getSelSize(), 63u);
    output.setToFiltered();
    EXPECT_EQ(output[0], 64u);
    EXPECT_EQ(output[62], 127u);

    output.setToUnfiltered(COUNT);
    ASSERT_TRUE(
        (function::BinaryFunctionExecutor::selectComparison<int64_t, int64_t, function::LessThan>(
            constant, data, output, nullptr)));
    ASSERT_EQ(output.getSelSize(), 63u);
    output.setToFiltered();
    EXPECT_EQ(output[0], 64u);
    EXPECT_EQ(output[62], 127u);
}

TEST(SIMDFilterTest, BinaryExecutorHandlesSharedDenseVectors) {
    constexpr common::sel_t COUNT = 128;
    auto state = std::make_shared<common::DataChunkState>(COUNT);
    state->setToUnflat();
    state->initOriginalAndSelectedSize(COUNT);
    common::ValueVector left{common::LogicalType::INT32(), nullptr, state};
    common::ValueVector right{common::LogicalType::INT32(), nullptr, state};
    for (common::sel_t i = 0; i < COUNT; ++i) {
        left.setValue<int32_t>(i, static_cast<int32_t>(i));
        right.setValue<int32_t>(i, static_cast<int32_t>(COUNT - 1 - i));
    }
    left.setNull(17, true);
    right.setNull(18, true);

    common::SelectionVector output{COUNT};
    ASSERT_TRUE(
        (function::BinaryFunctionExecutor::selectComparison<int32_t, int32_t, function::LessThan>(
            left, right, output, nullptr)));
    ASSERT_EQ(output.getSelSize(), 62u);
    output.setToFiltered();
    EXPECT_EQ(output[0], 0u);
    EXPECT_EQ(output[16], 16u);
    EXPECT_EQ(output[17], 19u);
    EXPECT_EQ(output[61], 63u);
}
