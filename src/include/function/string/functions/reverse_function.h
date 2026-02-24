#pragma once

#include "common/api.h"
#include "common/types/string_t.h"
#include "common/vector/value_vector.h"

namespace lbug {
namespace function {

struct Reverse {
public:
    LBUG_API static void operation(common::string_t& input, common::string_t& result,
        common::ValueVector& resultValueVector);

private:
    static uint32_t reverseStr(char* data, uint32_t len) {
        for (auto i = 0u; i < len / 2; i++) {
            std::swap(data[i], data[len - i - 1]);
        }
        return len;
    }
};

} // namespace function
} // namespace lbug
