#pragma once

#include "common/assert.h"
#include "common/types/string_t.h"
#include "common/vector/value_vector.h"

namespace lbug {
namespace function {

struct PadOperation {
public:
    static inline void operation(common::string_t& src, int64_t count,
        common::string_t& characterToPad, common::string_t& result,
        common::ValueVector& resultValueVector,
        void (*padOperation)(common::string_t& result, common::string_t& src,
            common::string_t& characterToPad)) {
        if (count <= 0) {
            result.set("", 0);
            return;
        }
        DASSERT(characterToPad.len == 1);
        padOperation(result, src, characterToPad);
        common::StringVector::addString(&resultValueVector, result, (const char*)result.getData(),
            count);
    }
};

} // namespace function
} // namespace lbug
