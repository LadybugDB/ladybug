#pragma once

#include "common/data_chunk/sel_vector.h"

namespace lbug {
namespace storage {

// Keeps the selected positions for which keep(pos) is true.
template<typename Keep>
void filterSelection(common::SelectionVector& sel, Keep&& keep) {
    auto buffer = sel.getMutableBuffer();
    const auto selSize = sel.getSelSize();
    common::sel_t numKept = 0;
    if (sel.isUnfiltered()) {
        for (common::sel_t pos = 0; pos < selSize; pos++) {
            buffer[numKept] = pos;
            numKept += keep(pos);
        }
    } else {
        for (common::sel_t i = 0; i < selSize; i++) {
            const auto pos = sel[i];
            buffer[numKept] = pos;
            numKept += keep(pos);
        }
    }
    if (numKept < selSize) {
        sel.setToFiltered(numKept);
    }
}

} // namespace storage
} // namespace lbug
