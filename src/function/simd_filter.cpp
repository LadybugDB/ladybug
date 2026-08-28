#include "function/comparison/simd_filter.h"

#include <cstdlib>
#include <string_view>

#include "common/data_chunk/data_chunk_state.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/null_mask.h"
#include "common/simd/cpu_features.h"
#include "common/vector/value_vector.h"

#if defined(_MSC_VER) && defined(LBUG_SIMD_COMPILED_AVX2)
#include <intrin.h>
#endif

namespace lbug {
namespace function {
namespace simd {

#if defined(LBUG_SIMD_COMPILED_AVX2)
common::sel_t selectConstantAVX2(const void* data, common::sel_t count, const void* constant,
    const uint64_t* nullMask, common::sel_t* output, common::PhysicalTypeID type,
    ComparisonOperation operation);
common::sel_t selectVectorAVX2(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output,
    common::PhysicalTypeID type, ComparisonOperation operation);
#endif
#if defined(LBUG_SIMD_COMPILED_NEON)
common::sel_t selectConstantNEON(const void* data, common::sel_t count, const void* constant,
    const uint64_t* nullMask, common::sel_t* output, common::PhysicalTypeID type,
    ComparisonOperation operation);
common::sel_t selectVectorNEON(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output,
    common::PhysicalTypeID type, ComparisonOperation operation);
#endif

namespace {

#if defined(LBUG_SIMD_COMPILED_AVX2) || defined(LBUG_SIMD_COMPILED_NEON)
constexpr common::sel_t MIN_SIMD_FILTER_SIZE = 64;
#endif

template<typename T, ComparisonOperation OPERATION>
bool compare(const T& left, const T& right) {
    if constexpr (OPERATION == ComparisonOperation::EQUAL) {
        return left == right;
    } else if constexpr (OPERATION == ComparisonOperation::NOT_EQUAL) {
        return !(left == right);
    } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN) {
        return left > right;
    } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN_EQUAL) {
        return left > right || left == right;
    } else if constexpr (OPERATION == ComparisonOperation::LESS_THAN) {
        // Match LessThan::operation exactly, including its unordered floating-point behavior.
        return !(left > right || left == right);
    } else {
        // Match LessThanEquals::operation exactly, including NaN behavior.
        return !(left > right);
    }
}

template<typename T, ComparisonOperation OPERATION>
common::sel_t selectConstantScalarTyped(const void* dataPtr, common::sel_t count,
    const void* constantPtr, const uint64_t* nullMask, common::sel_t* output) {
    const auto data = static_cast<const T*>(dataPtr);
    const auto constant = *static_cast<const T*>(constantPtr);
    common::sel_t selected = 0;
    for (common::sel_t i = 0; i < count; ++i) {
        if (nullMask && common::NullMask::isNull(nullMask, i)) {
            continue;
        }
        if (compare<T, OPERATION>(data[i], constant)) {
            output[selected++] = i;
        }
    }
    return selected;
}

template<typename T>
common::sel_t selectConstantScalarTyped(const void* data, common::sel_t count, const void* constant,
    const uint64_t* nullMask, common::sel_t* output, ComparisonOperation operation) {
    switch (operation) {
    case ComparisonOperation::EQUAL:
        return selectConstantScalarTyped<T, ComparisonOperation::EQUAL>(data, count, constant,
            nullMask, output);
    case ComparisonOperation::NOT_EQUAL:
        return selectConstantScalarTyped<T, ComparisonOperation::NOT_EQUAL>(data, count, constant,
            nullMask, output);
    case ComparisonOperation::GREATER_THAN:
        return selectConstantScalarTyped<T, ComparisonOperation::GREATER_THAN>(data, count,
            constant, nullMask, output);
    case ComparisonOperation::GREATER_THAN_EQUAL:
        return selectConstantScalarTyped<T, ComparisonOperation::GREATER_THAN_EQUAL>(data, count,
            constant, nullMask, output);
    case ComparisonOperation::LESS_THAN:
        return selectConstantScalarTyped<T, ComparisonOperation::LESS_THAN>(data, count, constant,
            nullMask, output);
    case ComparisonOperation::LESS_THAN_EQUAL:
        return selectConstantScalarTyped<T, ComparisonOperation::LESS_THAN_EQUAL>(data, count,
            constant, nullMask, output);
    }
    return 0;
}

template<typename T, ComparisonOperation OPERATION>
common::sel_t selectVectorScalarTyped(const void* leftPtr, const void* rightPtr,
    common::sel_t count, const uint64_t* leftNullMask, const uint64_t* rightNullMask,
    common::sel_t* output) {
    const auto left = static_cast<const T*>(leftPtr);
    const auto right = static_cast<const T*>(rightPtr);
    common::sel_t selected = 0;
    for (common::sel_t i = 0; i < count; ++i) {
        const auto isValid = (!leftNullMask || !common::NullMask::isNull(leftNullMask, i)) &&
                             (!rightNullMask || !common::NullMask::isNull(rightNullMask, i));
        const auto matches = isValid && compare<T, OPERATION>(left[i], right[i]);
        output[selected] = i;
        selected += matches;
    }
    return selected;
}

template<typename T>
common::sel_t selectVectorScalarTyped(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output,
    ComparisonOperation operation) {
    switch (operation) {
    case ComparisonOperation::EQUAL:
        return selectVectorScalarTyped<T, ComparisonOperation::EQUAL>(left, right, count,
            leftNullMask, rightNullMask, output);
    case ComparisonOperation::NOT_EQUAL:
        return selectVectorScalarTyped<T, ComparisonOperation::NOT_EQUAL>(left, right, count,
            leftNullMask, rightNullMask, output);
    case ComparisonOperation::GREATER_THAN:
        return selectVectorScalarTyped<T, ComparisonOperation::GREATER_THAN>(left, right, count,
            leftNullMask, rightNullMask, output);
    case ComparisonOperation::GREATER_THAN_EQUAL:
        return selectVectorScalarTyped<T, ComparisonOperation::GREATER_THAN_EQUAL>(left, right,
            count, leftNullMask, rightNullMask, output);
    case ComparisonOperation::LESS_THAN:
        return selectVectorScalarTyped<T, ComparisonOperation::LESS_THAN>(left, right, count,
            leftNullMask, rightNullMask, output);
    case ComparisonOperation::LESS_THAN_EQUAL:
        return selectVectorScalarTyped<T, ComparisonOperation::LESS_THAN_EQUAL>(left, right, count,
            leftNullMask, rightNullMask, output);
    }
    return 0;
}

#if defined(LBUG_SIMD_COMPILED_AVX2) || defined(LBUG_SIMD_COMPILED_NEON)
bool isSupportedType(common::PhysicalTypeID type) {
    switch (type) {
    case common::PhysicalTypeID::INT8:
    case common::PhysicalTypeID::INT16:
    case common::PhysicalTypeID::INT32:
    case common::PhysicalTypeID::INT64:
    case common::PhysicalTypeID::UINT8:
    case common::PhysicalTypeID::UINT16:
    case common::PhysicalTypeID::UINT32:
    case common::PhysicalTypeID::UINT64:
    case common::PhysicalTypeID::FLOAT:
    case common::PhysicalTypeID::DOUBLE:
        return true;
    default:
        return false;
    }
}

#endif

#if defined(LBUG_SIMD_COMPILED_AVX2)
bool cpuSupportsAVX2() {
    return common::simd::cpuSupportsAVX2();
}

bool useAVX2() {
    const auto requestedValue = std::getenv("LBUG_SIMD");
    const auto requested = requestedValue ? std::string_view{requestedValue} : "auto";
    if (requested == "scalar" || requested == "0") {
        return false;
    }
    if (requested != "auto" && requested != "avx2" && requested != "1") {
        throw common::RuntimeException("LBUG_SIMD must be one of auto, scalar, avx2, 0, or 1.");
    }
    if (cpuSupportsAVX2()) {
        return true;
    }
    if (requested == "avx2" || requested == "1") {
        throw common::RuntimeException(
            "LBUG_SIMD requested AVX2, but the running CPU or OS does not support it.");
    }
    return false;
}

#endif

#if defined(LBUG_SIMD_COMPILED_NEON)
bool useNEON() {
    const auto requestedValue = std::getenv("LBUG_SIMD");
    const auto requested = requestedValue ? std::string_view{requestedValue} : "auto";
    if (requested == "scalar" || requested == "0") {
        return false;
    }
    if (requested == "auto" || requested == "neon" || requested == "1") {
        return true;
    }
    throw common::RuntimeException("LBUG_SIMD must be one of auto, scalar, neon, 0, or 1.");
}
#endif

} // namespace

common::sel_t selectConstantScalar(const void* data, common::sel_t count, const void* constant,
    const uint64_t* nullMask, common::sel_t* output, common::PhysicalTypeID type,
    ComparisonOperation operation) {
    switch (type) {
    case common::PhysicalTypeID::INT8:
        return selectConstantScalarTyped<int8_t>(data, count, constant, nullMask, output,
            operation);
    case common::PhysicalTypeID::INT16:
        return selectConstantScalarTyped<int16_t>(data, count, constant, nullMask, output,
            operation);
    case common::PhysicalTypeID::INT32:
        return selectConstantScalarTyped<int32_t>(data, count, constant, nullMask, output,
            operation);
    case common::PhysicalTypeID::INT64:
        return selectConstantScalarTyped<int64_t>(data, count, constant, nullMask, output,
            operation);
    case common::PhysicalTypeID::UINT8:
        return selectConstantScalarTyped<uint8_t>(data, count, constant, nullMask, output,
            operation);
    case common::PhysicalTypeID::UINT16:
        return selectConstantScalarTyped<uint16_t>(data, count, constant, nullMask, output,
            operation);
    case common::PhysicalTypeID::UINT32:
        return selectConstantScalarTyped<uint32_t>(data, count, constant, nullMask, output,
            operation);
    case common::PhysicalTypeID::UINT64:
        return selectConstantScalarTyped<uint64_t>(data, count, constant, nullMask, output,
            operation);
    case common::PhysicalTypeID::FLOAT:
        return selectConstantScalarTyped<float>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::DOUBLE:
        return selectConstantScalarTyped<double>(data, count, constant, nullMask, output,
            operation);
    default:
        return 0;
    }
}

common::sel_t selectConstant(const void* data, common::sel_t count, const void* constant,
    const uint64_t* nullMask, common::sel_t* output, common::PhysicalTypeID type,
    ComparisonOperation operation) {
#if defined(LBUG_SIMD_COMPILED_AVX2)
    static const SelectConstantKernel kernel =
        useAVX2() ? selectConstantAVX2 : selectConstantScalar;
    return kernel(data, count, constant, nullMask, output, type, operation);
#elif defined(LBUG_SIMD_COMPILED_NEON)
    static const SelectConstantKernel kernel =
        useNEON() ? selectConstantNEON : selectConstantScalar;
    return kernel(data, count, constant, nullMask, output, type, operation);
#else
    return selectConstantScalar(data, count, constant, nullMask, output, type, operation);
#endif
}

common::sel_t selectVectorScalar(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output,
    common::PhysicalTypeID type, ComparisonOperation operation) {
    switch (type) {
    case common::PhysicalTypeID::INT8:
        return selectVectorScalarTyped<int8_t>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    case common::PhysicalTypeID::INT16:
        return selectVectorScalarTyped<int16_t>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    case common::PhysicalTypeID::INT32:
        return selectVectorScalarTyped<int32_t>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    case common::PhysicalTypeID::INT64:
        return selectVectorScalarTyped<int64_t>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    case common::PhysicalTypeID::UINT8:
        return selectVectorScalarTyped<uint8_t>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    case common::PhysicalTypeID::UINT16:
        return selectVectorScalarTyped<uint16_t>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    case common::PhysicalTypeID::UINT32:
        return selectVectorScalarTyped<uint32_t>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    case common::PhysicalTypeID::UINT64:
        return selectVectorScalarTyped<uint64_t>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    case common::PhysicalTypeID::FLOAT:
        return selectVectorScalarTyped<float>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    case common::PhysicalTypeID::DOUBLE:
        return selectVectorScalarTyped<double>(left, right, count, leftNullMask, rightNullMask,
            output, operation);
    default:
        return 0;
    }
}

common::sel_t selectVector(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output,
    common::PhysicalTypeID type, ComparisonOperation operation) {
#if defined(LBUG_SIMD_COMPILED_AVX2)
    static const SelectVectorKernel kernel = useAVX2() ? selectVectorAVX2 : selectVectorScalar;
    return kernel(left, right, count, leftNullMask, rightNullMask, output, type, operation);
#elif defined(LBUG_SIMD_COMPILED_NEON)
    static const SelectVectorKernel kernel = useNEON() ? selectVectorNEON : selectVectorScalar;
    return kernel(left, right, count, leftNullMask, rightNullMask, output, type, operation);
#else
    return selectVectorScalar(left, right, count, leftNullMask, rightNullMask, output, type,
        operation);
#endif
}

bool trySelectConstant(common::ValueVector& data, common::ValueVector& constant,
    ComparisonOperation operation, common::SelectionVector& output) {
#if !defined(LBUG_SIMD_COMPILED_AVX2) && !defined(LBUG_SIMD_COMPILED_NEON)
    (void)data;
    (void)constant;
    (void)operation;
    (void)output;
    return false;
#else
    if (!data.state || !constant.state || data.state->isFlat() || !constant.state->isFlat()) {
        return false;
    }
    const auto count = data.state->getSelSize();
    if (count < MIN_SIMD_FILTER_SIZE || !data.state->getSelVector().isUnfiltered()) {
        return false;
    }
    const auto type = data.dataType.getPhysicalType();
    if (type != constant.dataType.getPhysicalType() || !isSupportedType(type)) {
        return false;
    }
    const auto constantPosition = constant.state->getSelVector()[0];
    if (constant.isNull(constantPosition)) {
        return false;
    }
    const auto nullMask = data.hasNoNullsGuarantee() ? nullptr : data.getNullMask().getData();
    const auto selected = selectConstant(data.getData(), count,
        constant.getData() + constantPosition * constant.getNumBytesPerValue(), nullMask,
        output.getMutableBuffer().data(), type, operation);
    // Match BinaryFunctionExecutor's existing contract: fill the mutable buffer and update its
    // size. ExpressionEvaluator::select activates filtered mode after selectInternal returns.
    output.setSelSize(selected);
    return true;
#endif
}

bool trySelectVector(common::ValueVector& left, common::ValueVector& right,
    ComparisonOperation operation, common::SelectionVector& output) {
#if !defined(LBUG_SIMD_COMPILED_AVX2) && !defined(LBUG_SIMD_COMPILED_NEON)
    (void)left;
    (void)right;
    (void)operation;
    (void)output;
    return false;
#else
    if (!left.state || left.state != right.state || left.state->isFlat()) {
        return false;
    }
    const auto count = left.state->getSelSize();
    if (count < MIN_SIMD_FILTER_SIZE || !left.state->getSelVector().isUnfiltered()) {
        return false;
    }
    const auto type = left.dataType.getPhysicalType();
    if (type != right.dataType.getPhysicalType() || !isSupportedType(type)) {
        return false;
    }
    const auto leftNullMask = left.hasNoNullsGuarantee() ? nullptr : left.getNullMask().getData();
    const auto rightNullMask =
        right.hasNoNullsGuarantee() ? nullptr : right.getNullMask().getData();
    const auto selected = selectVector(left.getData(), right.getData(), count, leftNullMask,
        rightNullMask, output.getMutableBuffer().data(), type, operation);
    output.setSelSize(selected);
    return true;
#endif
}

} // namespace simd
} // namespace function
} // namespace lbug
