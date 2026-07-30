#pragma once

#include "common/types/int128_t.h"
#include "common/types/types.h"
#include "common/types/uint128_t.h"

namespace lbug {
namespace function {

inline double valueToDouble(const uint8_t* data, common::LogicalTypeID typeID) {
    switch (typeID) {
    case common::LogicalTypeID::INT8:
        return static_cast<double>(*reinterpret_cast<const int8_t*>(data));
    case common::LogicalTypeID::INT16:
        return static_cast<double>(*reinterpret_cast<const int16_t*>(data));
    case common::LogicalTypeID::INT32:
        return static_cast<double>(*reinterpret_cast<const int32_t*>(data));
    case common::LogicalTypeID::INT64:
    case common::LogicalTypeID::SERIAL:
        return static_cast<double>(*reinterpret_cast<const int64_t*>(data));
    case common::LogicalTypeID::UINT8:
        return static_cast<double>(*reinterpret_cast<const uint8_t*>(data));
    case common::LogicalTypeID::UINT16:
        return static_cast<double>(*reinterpret_cast<const uint16_t*>(data));
    case common::LogicalTypeID::UINT32:
        return static_cast<double>(*reinterpret_cast<const uint32_t*>(data));
    case common::LogicalTypeID::UINT64:
        return static_cast<double>(*reinterpret_cast<const uint64_t*>(data));
    case common::LogicalTypeID::FLOAT:
        return static_cast<double>(*reinterpret_cast<const float*>(data));
    case common::LogicalTypeID::DOUBLE:
        return *reinterpret_cast<const double*>(data);
    case common::LogicalTypeID::INT128:
        return static_cast<double>(*reinterpret_cast<const common::int128_t*>(data));
    case common::LogicalTypeID::UINT128:
        return static_cast<double>(*reinterpret_cast<const common::uint128_t*>(data));
    default:
        UNREACHABLE_CODE;
    }
}

} // namespace function
} // namespace lbug
