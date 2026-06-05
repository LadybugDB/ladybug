#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "common/cast.h"

namespace lbug {
namespace common {

class Reader {
public:
    virtual void read(uint8_t* data, uint64_t size) = 0;
    virtual ~Reader() = default;

    virtual bool finished() = 0;
    virtual uint64_t getReadOffset() const { return 0; }
    virtual void skip(uint64_t size) {
        std::array<uint8_t, 4096> buffer{};
        while (size > 0) {
            const auto numBytesToRead = std::min<uint64_t>(size, buffer.size());
            read(buffer.data(), numBytesToRead);
            size -= numBytesToRead;
        }
    }
    virtual void onObjectBegin() {};
    virtual void onObjectEnd() {};

    template<typename TARGET>
    TARGET* cast() {
        return common::dynamic_cast_checked<TARGET*>(this);
    }
};

} // namespace common
} // namespace lbug
