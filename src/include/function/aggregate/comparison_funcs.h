#pragma once

#include "common/types/int128_t.h"
#include "common/types/types.h"
#include "common/types/uint128_t.h"

namespace lbug {
namespace function {

inline bool valueLess(const uint8_t* left, const uint8_t* right, common::LogicalTypeID typeID) {
    switch (typeID) {
    case common::LogicalTypeID::INT8:
        return *reinterpret_cast<const int8_t*>(left) < *reinterpret_cast<const int8_t*>(right);
    case common::LogicalTypeID::INT16:
        return *reinterpret_cast<const int16_t*>(left) < *reinterpret_cast<const int16_t*>(right);
    case common::LogicalTypeID::INT32:
        return *reinterpret_cast<const int32_t*>(left) < *reinterpret_cast<const int32_t*>(right);
    case common::LogicalTypeID::INT64:
    case common::LogicalTypeID::SERIAL:
        return *reinterpret_cast<const int64_t*>(left) < *reinterpret_cast<const int64_t*>(right);
    case common::LogicalTypeID::UINT8:
        return *reinterpret_cast<const uint8_t*>(left) < *reinterpret_cast<const uint8_t*>(right);
    case common::LogicalTypeID::UINT16:
        return *reinterpret_cast<const uint16_t*>(left) < *reinterpret_cast<const uint16_t*>(right);
    case common::LogicalTypeID::UINT32:
        return *reinterpret_cast<const uint32_t*>(left) < *reinterpret_cast<const uint32_t*>(right);
    case common::LogicalTypeID::UINT64:
        return *reinterpret_cast<const uint64_t*>(left) < *reinterpret_cast<const uint64_t*>(right);
    case common::LogicalTypeID::FLOAT:
        return *reinterpret_cast<const float*>(left) < *reinterpret_cast<const float*>(right);
    case common::LogicalTypeID::DOUBLE:
        return *reinterpret_cast<const double*>(left) < *reinterpret_cast<const double*>(right);
    case common::LogicalTypeID::INT128:
        return *reinterpret_cast<const common::int128_t*>(left) <
               *reinterpret_cast<const common::int128_t*>(right);
    case common::LogicalTypeID::UINT128:
        return *reinterpret_cast<const common::uint128_t*>(left) <
               *reinterpret_cast<const common::uint128_t*>(right);
    default:
        UNREACHABLE_CODE;
    }
}

} // namespace function
} // namespace lbug
