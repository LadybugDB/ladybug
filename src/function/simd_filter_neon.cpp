#include <cstdint>
#include <type_traits>

#include <bit>

#if defined(_MSC_VER)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif

#include "common/null_mask.h"
#include "function/comparison/simd_filter.h"

#if !defined(__aarch64__) && !defined(_M_ARM64)
#error "simd_filter_neon.cpp requires ARM64 NEON"
#endif

namespace lbug {
namespace function {
namespace simd {

namespace {

template<typename T>
auto loadVector(const T* data) {
    if constexpr (std::is_same_v<T, int8_t>) {
        return vld1q_s8(data);
    } else if constexpr (std::is_same_v<T, int16_t>) {
        return vld1q_s16(data);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return vld1q_s32(data);
    } else if constexpr (std::is_same_v<T, int64_t>) {
        return vld1q_s64(data);
    } else if constexpr (std::is_same_v<T, uint8_t>) {
        return vld1q_u8(data);
    } else if constexpr (std::is_same_v<T, uint16_t>) {
        return vld1q_u16(data);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        return vld1q_u32(data);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
        return vld1q_u64(data);
    } else if constexpr (std::is_same_v<T, float>) {
        return vld1q_f32(data);
    } else {
        return vld1q_f64(data);
    }
}

template<typename T>
auto broadcastVector(T value) {
    if constexpr (std::is_same_v<T, int8_t>) {
        return vdupq_n_s8(value);
    } else if constexpr (std::is_same_v<T, int16_t>) {
        return vdupq_n_s16(value);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return vdupq_n_s32(value);
    } else if constexpr (std::is_same_v<T, int64_t>) {
        return vdupq_n_s64(value);
    } else if constexpr (std::is_same_v<T, uint8_t>) {
        return vdupq_n_u8(value);
    } else if constexpr (std::is_same_v<T, uint16_t>) {
        return vdupq_n_u16(value);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        return vdupq_n_u32(value);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
        return vdupq_n_u64(value);
    } else if constexpr (std::is_same_v<T, float>) {
        return vdupq_n_f32(value);
    } else {
        return vdupq_n_f64(value);
    }
}

template<typename T, typename VECTOR>
auto equalVector(VECTOR left, VECTOR right) {
    if constexpr (std::is_same_v<T, int8_t>) {
        return vceqq_s8(left, right);
    } else if constexpr (std::is_same_v<T, int16_t>) {
        return vceqq_s16(left, right);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return vceqq_s32(left, right);
    } else if constexpr (std::is_same_v<T, int64_t>) {
        return vceqq_s64(left, right);
    } else if constexpr (std::is_same_v<T, uint8_t>) {
        return vceqq_u8(left, right);
    } else if constexpr (std::is_same_v<T, uint16_t>) {
        return vceqq_u16(left, right);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        return vceqq_u32(left, right);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
        return vceqq_u64(left, right);
    } else if constexpr (std::is_same_v<T, float>) {
        return vceqq_f32(left, right);
    } else {
        return vceqq_f64(left, right);
    }
}

template<typename T, typename VECTOR>
auto greaterVector(VECTOR left, VECTOR right) {
    if constexpr (std::is_same_v<T, int8_t>) {
        return vcgtq_s8(left, right);
    } else if constexpr (std::is_same_v<T, int16_t>) {
        return vcgtq_s16(left, right);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return vcgtq_s32(left, right);
    } else if constexpr (std::is_same_v<T, int64_t>) {
        return vcgtq_s64(left, right);
    } else if constexpr (std::is_same_v<T, uint8_t>) {
        return vcgtq_u8(left, right);
    } else if constexpr (std::is_same_v<T, uint16_t>) {
        return vcgtq_u16(left, right);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        return vcgtq_u32(left, right);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
        return vcgtq_u64(left, right);
    } else if constexpr (std::is_same_v<T, float>) {
        return vcgtq_f32(left, right);
    } else {
        return vcgtq_f64(left, right);
    }
}

template<typename T, typename MASK>
auto orMask(MASK left, MASK right) {
    if constexpr (sizeof(T) == 1) {
        return vorrq_u8(left, right);
    } else if constexpr (sizeof(T) == 2) {
        return vorrq_u16(left, right);
    } else if constexpr (sizeof(T) == 4) {
        return vorrq_u32(left, right);
    } else {
        return vorrq_u64(left, right);
    }
}

template<typename T, typename MASK>
auto notMask(MASK mask) {
    if constexpr (sizeof(T) == 1) {
        return veorq_u8(mask, vdupq_n_u8(UINT8_MAX));
    } else if constexpr (sizeof(T) == 2) {
        return veorq_u16(mask, vdupq_n_u16(UINT16_MAX));
    } else if constexpr (sizeof(T) == 4) {
        return veorq_u32(mask, vdupq_n_u32(UINT32_MAX));
    } else {
        return veorq_u64(mask, vdupq_n_u64(UINT64_MAX));
    }
}

template<typename T, ComparisonOperation OPERATION, typename VECTOR>
auto compareVector(VECTOR left, VECTOR right) {
    const auto equal = equalVector<T>(left, right);
    if constexpr (OPERATION == ComparisonOperation::EQUAL) {
        return equal;
    } else if constexpr (OPERATION == ComparisonOperation::NOT_EQUAL) {
        return notMask<T>(equal);
    } else {
        const auto greater = greaterVector<T>(left, right);
        if constexpr (OPERATION == ComparisonOperation::GREATER_THAN) {
            return greater;
        } else if constexpr (OPERATION == ComparisonOperation::GREATER_THAN_EQUAL) {
            return orMask<T>(greater, equal);
        } else if constexpr (OPERATION == ComparisonOperation::LESS_THAN) {
            return notMask<T>(orMask<T>(greater, equal));
        } else {
            return notMask<T>(greater);
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
    const auto nullLanes = nullMask[base >> common::NullMask::NUM_BITS_PER_NULL_ENTRY_LOG2] >>
                           (base & (common::NullMask::NUM_BITS_PER_NULL_ENTRY - 1));
    return laneMask & ~static_cast<uint32_t>(nullLanes);
}

template<common::sel_t LANE_COUNT>
common::sel_t compactLaneMask(uint32_t laneMask, common::sel_t base, common::sel_t* output,
    common::sel_t selected) {
    constexpr auto FULL_MASK = UINT32_MAX >> (32 - LANE_COUNT);
    if (laneMask == 0) {
        return selected;
    }
    if (laneMask == FULL_MASK) {
        auto positions = vaddq_u64(vdupq_n_u64(base), vsetq_lane_u64(1, vdupq_n_u64(0), 1));
        for (common::sel_t lane = 0; lane < LANE_COUNT; lane += 2) {
            vst1q_u64(output + selected + lane, positions);
            positions = vaddq_u64(positions, vdupq_n_u64(2));
        }
        return selected + LANE_COUNT;
    }
    while (laneMask) {
        const auto lane = static_cast<common::sel_t>(std::countr_zero(laneMask));
        output[selected++] = base + lane;
        laneMask &= laneMask - 1;
    }
    return selected;
}

template<typename T, typename MASK>
uint32_t toLaneMask(MASK mask) {
    if constexpr (sizeof(T) == 1) {
        const auto bits = vshrq_n_u8(mask, 7);
        // MSVC ARM64 maps NEON vector types to __n64/__n128, which cannot take brace
        // initializers (C2078), so build the {1, 2, 4, 8, 16, 32, 64, 128} lane weights
        // from a bit pattern instead.
        const auto weights = vcreate_u8(0x8040201008040201ULL);
        const auto low = vaddv_u8(vmul_u8(vget_low_u8(bits), weights));
        const auto high = vaddv_u8(vmul_u8(vget_high_u8(bits), weights));
        return static_cast<uint32_t>(low) | (static_cast<uint32_t>(high) << 8);
    } else if constexpr (sizeof(T) == 2) {
        const auto weights =
            vcombine_u16(vcreate_u16(0x0008000400020001ULL), vcreate_u16(0x0080004000200010ULL));
        return vaddvq_u16(vmulq_u16(vshrq_n_u16(mask, 15), weights));
    } else if constexpr (sizeof(T) == 4) {
        const auto weights =
            vcombine_u32(vcreate_u32(0x0000000200000001ULL), vcreate_u32(0x0000000800000004ULL));
        return vaddvq_u32(vmulq_u32(vshrq_n_u32(mask, 31), weights));
    } else {
        const auto bits = vshrq_n_u64(mask, 63);
        return static_cast<uint32_t>(vgetq_lane_u64(bits, 0) | (vgetq_lane_u64(bits, 1) << 1));
    }
}

template<typename T, ComparisonOperation OPERATION>
common::sel_t selectConstantTyped(const void* dataPtr, common::sel_t count, const void* constantPtr,
    const uint64_t* nullMask, common::sel_t* output) {
    constexpr common::sel_t LANE_COUNT = 16 / sizeof(T);
    constexpr common::sel_t GROUP_LANE_COUNT = 32;
    constexpr common::sel_t VECTORS_PER_GROUP = GROUP_LANE_COUNT / LANE_COUNT;
    const auto data = static_cast<const T*>(dataPtr);
    const auto constant = *static_cast<const T*>(constantPtr);
    const auto constantVector = broadcastVector(constant);
    common::sel_t selected = 0;
    common::sel_t base = 0;
    for (; base + GROUP_LANE_COUNT <= count; base += GROUP_LANE_COUNT) {
        uint32_t groupMask = 0;
        for (common::sel_t vector = 0; vector < VECTORS_PER_GROUP; ++vector) {
            const auto vectorBase = base + vector * LANE_COUNT;
            auto laneMask = toLaneMask<T>(
                compareVector<T, OPERATION>(loadVector(data + vectorBase), constantVector));
            laneMask = applyNullMask<LANE_COUNT>(laneMask, nullMask, vectorBase);
            groupMask |= laneMask << (vector * LANE_COUNT);
        }
        selected = compactLaneMask<GROUP_LANE_COUNT>(groupMask, base, output, selected);
    }
    for (; base + LANE_COUNT <= count; base += LANE_COUNT) {
        auto laneMask =
            toLaneMask<T>(compareVector<T, OPERATION>(loadVector(data + base), constantVector));
        laneMask = applyNullMask<LANE_COUNT>(laneMask, nullMask, base);
        selected = compactLaneMask<LANE_COUNT>(laneMask, base, output, selected);
    }
    for (; base < count; ++base) {
        if ((!nullMask || !common::NullMask::isNull(nullMask, base)) &&
            compareScalar<T, OPERATION>(data[base], constant)) {
            output[selected++] = base;
        }
    }
    return selected;
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
common::sel_t selectVectorTyped(const void* leftPtr, const void* rightPtr, common::sel_t count,
    const uint64_t* leftNullMask, const uint64_t* rightNullMask, common::sel_t* output) {
    constexpr common::sel_t LANE_COUNT = 16 / sizeof(T);
    constexpr common::sel_t GROUP_LANE_COUNT = 32;
    constexpr common::sel_t VECTORS_PER_GROUP = GROUP_LANE_COUNT / LANE_COUNT;
    const auto left = static_cast<const T*>(leftPtr);
    const auto right = static_cast<const T*>(rightPtr);
    common::sel_t selected = 0;
    common::sel_t base = 0;
    for (; base + GROUP_LANE_COUNT <= count; base += GROUP_LANE_COUNT) {
        uint32_t groupMask = 0;
        for (common::sel_t vector = 0; vector < VECTORS_PER_GROUP; ++vector) {
            const auto vectorBase = base + vector * LANE_COUNT;
            auto laneMask = toLaneMask<T>(compareVector<T, OPERATION>(loadVector(left + vectorBase),
                loadVector(right + vectorBase)));
            laneMask = applyNullMask<LANE_COUNT>(laneMask, leftNullMask, vectorBase);
            laneMask = applyNullMask<LANE_COUNT>(laneMask, rightNullMask, vectorBase);
            groupMask |= laneMask << (vector * LANE_COUNT);
        }
        selected = compactLaneMask<GROUP_LANE_COUNT>(groupMask, base, output, selected);
    }
    for (; base + LANE_COUNT <= count; base += LANE_COUNT) {
        auto laneMask = toLaneMask<T>(
            compareVector<T, OPERATION>(loadVector(left + base), loadVector(right + base)));
        laneMask = applyNullMask<LANE_COUNT>(laneMask, leftNullMask, base);
        laneMask = applyNullMask<LANE_COUNT>(laneMask, rightNullMask, base);
        selected = compactLaneMask<LANE_COUNT>(laneMask, base, output, selected);
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

common::sel_t selectConstantNEON(const void* data, common::sel_t count, const void* constant,
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

common::sel_t selectVectorNEON(const void* left, const void* right, common::sel_t count,
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
        return selectVectorScalar(left, right, count, leftNullMask, rightNullMask, output, type,
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
        return selectVectorScalar(left, right, count, leftNullMask, rightNullMask, output, type,
            operation);
    case common::PhysicalTypeID::FLOAT:
        return selectVectorTyped<float>(left, right, count, leftNullMask, rightNullMask, output,
            operation);
    case common::PhysicalTypeID::DOUBLE:
        return selectVectorScalar(left, right, count, leftNullMask, rightNullMask, output, type,
            operation);
    default:
        return 0;
    }
}

} // namespace simd
} // namespace function
} // namespace lbug
