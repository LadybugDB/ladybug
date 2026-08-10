#include <cstdint>
#include <limits>
#include <type_traits>

#include "common/null_mask.h"
#include "function/comparison/simd_filter.h"
#include <bit>
#include <immintrin.h>

#if !defined(__AVX2__) && !defined(_MSC_VER)
#error "simd_filter_avx2.cpp must be compiled with AVX2 enabled"
#endif

namespace lbug {
namespace function {
namespace simd {

namespace {

template<typename T>
__m256i broadcastIntegral(T value) {
    using SignedT = std::make_signed_t<T>;
    const auto bits = std::bit_cast<SignedT>(value);
    if constexpr (sizeof(T) == 1) {
        return _mm256_set1_epi8(bits);
    } else if constexpr (sizeof(T) == 2) {
        return _mm256_set1_epi16(bits);
    } else if constexpr (sizeof(T) == 4) {
        return _mm256_set1_epi32(bits);
    } else {
        return _mm256_set1_epi64x(bits);
    }
}

template<typename T>
__m256i equalIntegral(__m256i left, __m256i right) {
    if constexpr (sizeof(T) == 1) {
        return _mm256_cmpeq_epi8(left, right);
    } else if constexpr (sizeof(T) == 2) {
        return _mm256_cmpeq_epi16(left, right);
    } else if constexpr (sizeof(T) == 4) {
        return _mm256_cmpeq_epi32(left, right);
    } else {
        return _mm256_cmpeq_epi64(left, right);
    }
}

template<typename T>
__m256i greaterIntegral(__m256i left, __m256i right) {
    if constexpr (std::is_unsigned_v<T>) {
        using SignedT = std::make_signed_t<T>;
        const auto signBit = broadcastIntegral(std::numeric_limits<SignedT>::min());
        left = _mm256_xor_si256(left, signBit);
        right = _mm256_xor_si256(right, signBit);
    }
    if constexpr (sizeof(T) == 1) {
        return _mm256_cmpgt_epi8(left, right);
    } else if constexpr (sizeof(T) == 2) {
        return _mm256_cmpgt_epi16(left, right);
    } else if constexpr (sizeof(T) == 4) {
        return _mm256_cmpgt_epi32(left, right);
    } else {
        return _mm256_cmpgt_epi64(left, right);
    }
}

template<typename T, ComparisonOperation OPERATION>
__m256i compareIntegral(__m256i left, __m256i right) {
    const auto equal = equalIntegral<T>(left, right);
    if constexpr (OPERATION == ComparisonOperation::EQUAL) {
        return equal;
    } else if constexpr (OPERATION == ComparisonOperation::NOT_EQUAL) {
        return _mm256_xor_si256(equal, _mm256_set1_epi32(-1));
    } else {
        const auto greater = greaterIntegral<T>(left, right);
        if constexpr (OPERATION == ComparisonOperation::GREATER_THAN) {
            return greater;
        } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN_EQUAL) {
            return _mm256_or_si256(greater, equal);
        } else if constexpr (OPERATION == ComparisonOperation::LESS_THAN) {
            const auto greaterOrEqual = _mm256_or_si256(greater, equal);
            return _mm256_xor_si256(greaterOrEqual, _mm256_set1_epi32(-1));
        } else {
            return _mm256_xor_si256(greater, _mm256_set1_epi32(-1));
        }
    }
}

template<typename T, ComparisonOperation OPERATION>
bool compareScalar(const T& left, const T& right) {
    if constexpr (OPERATION == ComparisonOperation::EQUAL) {
        return left == right;
    } else if constexpr (OPERATION == ComparisonOperation::NOT_EQUAL) {
        return !(left == right);
    } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN) {
        return left > right;
    } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN_EQUAL) {
        return left > right || left == right;
    } else if constexpr (OPERATION == ComparisonOperation::LESS_THAN) {
        return !(left > right || left == right);
    } else {
        return !(left > right);
    }
}

template<common::sel_t LANE_COUNT>
uint32_t applyNullMask(uint32_t laneMask, const uint64_t* nullMask, common::sel_t base) {
    if (!nullMask) {
        return laneMask;
    }
    // Every supported AVX2 lane count divides 64 and base starts at zero, so an AVX2 block never
    // crosses a NullMask word boundary.
    const auto nullLanes = nullMask[base >> common::NullMask::NUM_BITS_PER_NULL_ENTRY_LOG2] >>
                           (base & (common::NullMask::NUM_BITS_PER_NULL_ENTRY - 1));
    return laneMask & ~static_cast<uint32_t>(nullLanes);
}

common::sel_t compactLaneMask(uint32_t laneMask, common::sel_t base, common::sel_t* output,
    common::sel_t selected) {
    while (laneMask) {
        const auto lane = static_cast<common::sel_t>(std::countr_zero(laneMask));
        output[selected++] = base + lane;
        laneMask &= laneMask - 1;
    }
    return selected;
}

template<typename T, ComparisonOperation OPERATION>
common::sel_t selectConstantIntegral(const void* dataPtr, common::sel_t count,
    const void* constantPtr, const uint64_t* nullMask, common::sel_t* output) {
    constexpr common::sel_t LANE_COUNT = 32 / sizeof(T);
    const auto data = static_cast<const T*>(dataPtr);
    const auto constant = *static_cast<const T*>(constantPtr);
    const auto constantVector = broadcastIntegral(constant);
    common::sel_t selected = 0;
    common::sel_t base = 0;
    for (; base + LANE_COUNT <= count; base += LANE_COUNT) {
        const auto values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + base));
        const auto compared = compareIntegral<T, OPERATION>(values, constantVector);
        const auto byteMask = static_cast<uint32_t>(_mm256_movemask_epi8(compared));
        uint32_t laneMask = 0;
        for (common::sel_t lane = 0; lane < LANE_COUNT; ++lane) {
            laneMask |= ((byteMask >> (lane * sizeof(T))) & 1u) << lane;
        }
        laneMask = applyNullMask<LANE_COUNT>(laneMask, nullMask, base);
        selected = compactLaneMask(laneMask, base, output, selected);
    }
    for (; base < count; ++base) {
        if ((!nullMask || !common::NullMask::isNull(nullMask, base)) &&
            compareScalar<T, OPERATION>(data[base], constant)) {
            output[selected++] = base;
        }
    }
    return selected;
}

template<typename T, ComparisonOperation OPERATION>
common::sel_t selectVectorIntegral(const void* leftPtr, const void* rightPtr, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output) {
    constexpr common::sel_t LANE_COUNT = 32 / sizeof(T);
    const auto left = static_cast<const T*>(leftPtr);
    const auto right = static_cast<const T*>(rightPtr);
    common::sel_t selected = 0;
    common::sel_t base = 0;
    for (; base + LANE_COUNT <= count; base += LANE_COUNT) {
        const auto leftValues = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(left + base));
        const auto rightValues = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(right + base));
        const auto compared = compareIntegral<T, OPERATION>(leftValues, rightValues);
        const auto byteMask = static_cast<uint32_t>(_mm256_movemask_epi8(compared));
        uint32_t laneMask = 0;
        for (common::sel_t lane = 0; lane < LANE_COUNT; ++lane) {
            laneMask |= ((byteMask >> (lane * sizeof(T))) & 1u) << lane;
        }
        laneMask = applyNullMask<LANE_COUNT>(laneMask, leftNullMask, base);
        laneMask = applyNullMask<LANE_COUNT>(laneMask, rightNullMask, base);
        selected = compactLaneMask(laneMask, base, output, selected);
    }
    for (; base < count; ++base) {
        if ((!leftNullMask || !common::NullMask::isNull(leftNullMask, base)) &&
            (!rightNullMask || !common::NullMask::isNull(rightNullMask, base)) &&
            compareScalar<T, OPERATION>(left[base], right[base])) {
            output[selected++] = base;
        }
    }
    return selected;
}

template<typename T, ComparisonOperation OPERATION>
common::sel_t selectConstantFloating(const void* dataPtr, common::sel_t count,
    const void* constantPtr, const uint64_t* nullMask, common::sel_t* output) {
    constexpr common::sel_t LANE_COUNT = 32 / sizeof(T);
    const auto data = static_cast<const T*>(dataPtr);
    const auto constant = *static_cast<const T*>(constantPtr);
    common::sel_t selected = 0;
    common::sel_t base = 0;
    for (; base + LANE_COUNT <= count; base += LANE_COUNT) {
        uint32_t laneMask;
        if constexpr (std::is_same_v<T, float>) {
            const auto values = _mm256_loadu_ps(data + base);
            const auto constantVector = _mm256_set1_ps(constant);
            if constexpr (OPERATION == ComparisonOperation::EQUAL) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(values, constantVector, _CMP_EQ_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::NOT_EQUAL) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(values, constantVector, _CMP_NEQ_UQ));
            } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(values, constantVector, _CMP_GT_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN_EQUAL) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(values, constantVector, _CMP_GE_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::LESS_THAN) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(values, constantVector, _CMP_NGE_UQ));
            } else {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(values, constantVector, _CMP_NGT_UQ));
            }
        } else {
            const auto values = _mm256_loadu_pd(data + base);
            const auto constantVector = _mm256_set1_pd(constant);
            if constexpr (OPERATION == ComparisonOperation::EQUAL) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(values, constantVector, _CMP_EQ_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::NOT_EQUAL) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(values, constantVector, _CMP_NEQ_UQ));
            } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(values, constantVector, _CMP_GT_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN_EQUAL) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(values, constantVector, _CMP_GE_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::LESS_THAN) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(values, constantVector, _CMP_NGE_UQ));
            } else {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(values, constantVector, _CMP_NGT_UQ));
            }
        }
        laneMask = applyNullMask<LANE_COUNT>(laneMask, nullMask, base);
        selected = compactLaneMask(laneMask, base, output, selected);
    }
    for (; base < count; ++base) {
        if ((!nullMask || !common::NullMask::isNull(nullMask, base)) &&
            compareScalar<T, OPERATION>(data[base], constant)) {
            output[selected++] = base;
        }
    }
    return selected;
}

template<typename T, ComparisonOperation OPERATION>
common::sel_t selectVectorFloating(const void* leftPtr, const void* rightPtr, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output) {
    constexpr common::sel_t LANE_COUNT = 32 / sizeof(T);
    const auto left = static_cast<const T*>(leftPtr);
    const auto right = static_cast<const T*>(rightPtr);
    common::sel_t selected = 0;
    common::sel_t base = 0;
    for (; base + LANE_COUNT <= count; base += LANE_COUNT) {
        uint32_t laneMask;
        if constexpr (std::is_same_v<T, float>) {
            const auto leftValues = _mm256_loadu_ps(left + base);
            const auto rightValues = _mm256_loadu_ps(right + base);
            if constexpr (OPERATION == ComparisonOperation::EQUAL) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(leftValues, rightValues, _CMP_EQ_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::NOT_EQUAL) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(leftValues, rightValues, _CMP_NEQ_UQ));
            } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(leftValues, rightValues, _CMP_GT_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN_EQUAL) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(leftValues, rightValues, _CMP_GE_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::LESS_THAN) {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(leftValues, rightValues, _CMP_NGE_UQ));
            } else {
                laneMask = _mm256_movemask_ps(_mm256_cmp_ps(leftValues, rightValues, _CMP_NGT_UQ));
            }
        } else {
            const auto leftValues = _mm256_loadu_pd(left + base);
            const auto rightValues = _mm256_loadu_pd(right + base);
            if constexpr (OPERATION == ComparisonOperation::EQUAL) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(leftValues, rightValues, _CMP_EQ_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::NOT_EQUAL) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(leftValues, rightValues, _CMP_NEQ_UQ));
            } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(leftValues, rightValues, _CMP_GT_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN_EQUAL) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(leftValues, rightValues, _CMP_GE_OQ));
            } else if constexpr (OPERATION == ComparisonOperation::LESS_THAN) {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(leftValues, rightValues, _CMP_NGE_UQ));
            } else {
                laneMask = _mm256_movemask_pd(_mm256_cmp_pd(leftValues, rightValues, _CMP_NGT_UQ));
            }
        }
        laneMask = applyNullMask<LANE_COUNT>(laneMask, leftNullMask, base);
        laneMask = applyNullMask<LANE_COUNT>(laneMask, rightNullMask, base);
        selected = compactLaneMask(laneMask, base, output, selected);
    }
    for (; base < count; ++base) {
        if ((!leftNullMask || !common::NullMask::isNull(leftNullMask, base)) &&
            (!rightNullMask || !common::NullMask::isNull(rightNullMask, base)) &&
            compareScalar<T, OPERATION>(left[base], right[base])) {
            output[selected++] = base;
        }
    }
    return selected;
}

template<typename T, ComparisonOperation OPERATION>
common::sel_t selectConstantTyped(const void* data, common::sel_t count, const void* constant,
    const uint64_t* nullMask, common::sel_t* output) {
    if constexpr (std::is_integral_v<T>) {
        return selectConstantIntegral<T, OPERATION>(data, count, constant, nullMask, output);
    } else {
        return selectConstantFloating<T, OPERATION>(data, count, constant, nullMask, output);
    }
}

template<typename T>
common::sel_t selectConstantTyped(const void* data, common::sel_t count, const void* constant,
    const uint64_t* nullMask, common::sel_t* output, ComparisonOperation operation) {
    switch (operation) {
    case ComparisonOperation::EQUAL:
        return selectConstantTyped<T, ComparisonOperation::EQUAL>(data, count, constant, nullMask,
            output);
    case ComparisonOperation::NOT_EQUAL:
        return selectConstantTyped<T, ComparisonOperation::NOT_EQUAL>(data, count, constant,
            nullMask, output);
    case ComparisonOperation::GREATER_THAN:
        return selectConstantTyped<T, ComparisonOperation::GREATER_THAN>(data, count, constant,
            nullMask, output);
    case ComparisonOperation::GREATER_THAN_EQUAL:
        return selectConstantTyped<T, ComparisonOperation::GREATER_THAN_EQUAL>(data, count,
            constant, nullMask, output);
    case ComparisonOperation::LESS_THAN:
        return selectConstantTyped<T, ComparisonOperation::LESS_THAN>(data, count, constant,
            nullMask, output);
    case ComparisonOperation::LESS_THAN_EQUAL:
        return selectConstantTyped<T, ComparisonOperation::LESS_THAN_EQUAL>(data, count, constant,
            nullMask, output);
    }
    return 0;
}

template<typename T, ComparisonOperation OPERATION>
common::sel_t selectVectorTyped(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output) {
    if constexpr (std::is_integral_v<T>) {
        return selectVectorIntegral<T, OPERATION>(left, right, count, leftNullMask, rightNullMask,
            output);
    } else {
        return selectVectorFloating<T, OPERATION>(left, right, count, leftNullMask, rightNullMask,
            output);
    }
}

template<typename T>
common::sel_t selectVectorTyped(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output,
    ComparisonOperation operation) {
    switch (operation) {
    case ComparisonOperation::EQUAL:
        return selectVectorTyped<T, ComparisonOperation::EQUAL>(left, right, count, leftNullMask,
            rightNullMask, output);
    case ComparisonOperation::NOT_EQUAL:
        return selectVectorTyped<T, ComparisonOperation::NOT_EQUAL>(left, right, count,
            leftNullMask, rightNullMask, output);
    case ComparisonOperation::GREATER_THAN:
        return selectVectorTyped<T, ComparisonOperation::GREATER_THAN>(left, right, count,
            leftNullMask, rightNullMask, output);
    case ComparisonOperation::GREATER_THAN_EQUAL:
        return selectVectorTyped<T, ComparisonOperation::GREATER_THAN_EQUAL>(left, right, count,
            leftNullMask, rightNullMask, output);
    case ComparisonOperation::LESS_THAN:
        return selectVectorTyped<T, ComparisonOperation::LESS_THAN>(left, right, count,
            leftNullMask, rightNullMask, output);
    case ComparisonOperation::LESS_THAN_EQUAL:
        return selectVectorTyped<T, ComparisonOperation::LESS_THAN_EQUAL>(left, right, count,
            leftNullMask, rightNullMask, output);
    }
    return 0;
}

} // namespace

common::sel_t selectConstantAVX2(const void* data, common::sel_t count, const void* constant,
    const uint64_t* nullMask, common::sel_t* output, common::PhysicalTypeID type,
    ComparisonOperation operation) {
    switch (type) {
    case common::PhysicalTypeID::INT8:
        return selectConstantTyped<int8_t>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::INT16:
        return selectConstantTyped<int16_t>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::INT32:
        return selectConstantTyped<int32_t>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::INT64:
        return selectConstantTyped<int64_t>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::UINT8:
        return selectConstantTyped<uint8_t>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::UINT16:
        return selectConstantTyped<uint16_t>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::UINT32:
        return selectConstantTyped<uint32_t>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::UINT64:
        return selectConstantTyped<uint64_t>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::FLOAT:
        return selectConstantTyped<float>(data, count, constant, nullMask, output, operation);
    case common::PhysicalTypeID::DOUBLE:
        return selectConstantTyped<double>(data, count, constant, nullMask, output, operation);
    default:
        return 0;
    }
}

common::sel_t selectVectorAVX2(const void* left, const void* right, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output,
    common::PhysicalTypeID type, ComparisonOperation operation) {
    switch (type) {
    case common::PhysicalTypeID::INT8:
        return selectVectorTyped<int8_t>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::INT16:
        return selectVectorTyped<int16_t>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::INT32:
        return selectVectorTyped<int32_t>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::INT64:
        return selectVectorTyped<int64_t>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::UINT8:
        return selectVectorTyped<uint8_t>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::UINT16:
        return selectVectorTyped<uint16_t>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::UINT32:
        return selectVectorTyped<uint32_t>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::UINT64:
        return selectVectorTyped<uint64_t>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::FLOAT:
        return selectVectorTyped<float>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::DOUBLE:
        return selectVectorTyped<double>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    default:
        return 0;
    }
}

} // namespace simd
} // namespace function
} // namespace lbug
