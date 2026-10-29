#pragma once
#include "common/exception/runtime.h"
#include "common/type_utils.h"
#include "common/vector/value_vector.h"
#include <format>

namespace lbug {
namespace function {

struct ListExtract {
public:
    // Note: this function takes in a 1-based position (The index of the first value in the list
    // is 1).
    template<typename T>
    static inline void operation(common::list_entry_t& listEntry, int64_t pos, T& result,
        common::ValueVector& listVector, common::ValueVector& /*posVector*/,
        common::ValueVector& resultVector, uint64_t resPos) {
        if (pos == 0) {
            throw common::RuntimeException("List extract takes 1-based position.");
        }
        if ((pos > 0 && pos > listEntry.size) || (pos < 0 && pos < -(int64_t)listEntry.size)) {
            throw common::RuntimeException(
                std::format("list_extract(list, index): index={} is out of range.",
                    common::TypeUtils::toString(pos)));
        }
        if (pos > 0) {
            pos--;
        } else {
            pos = listEntry.size + pos;
        }
        auto listDataVector = common::ListVector::getDataVector(&listVector);
        resultVector.setNull(resPos, listDataVector->isNull(listEntry.offset + pos));
        if (!resultVector.isNull(resPos)) {
            auto listValues =
                common::ListVector::getListValuesWithOffset(&listVector, listEntry, pos);
            resultVector.copyFromVectorData(reinterpret_cast<uint8_t*>(&result), listDataVector,
                listValues);
        }
    }
};

} // namespace function
} // namespace lbug
