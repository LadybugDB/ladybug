#pragma once

#include <vector>

#include "common/arrow/arrow_converter.h"
#include "common/vector/value_vector.h"

namespace lbug {
namespace storage {

class ArrowUtils {
public:
    static void copyArrowMorselToOutputVectors(const ArrowArrayWrapper& batch,
        const ArrowSchemaWrapper& schema, const size_t currentMorselStartOffset,
        const uint64_t numRowsToCopy, const std::vector<common::ValueVector*>& outputVectors,
        const std::vector<int64_t>& outputToArrowColumnIdx) {
        auto numChildren = static_cast<uint64_t>(batch.n_children);

        for (uint64_t outCol = 0; outCol < outputVectors.size(); ++outCol) {
            if (!outputVectors[outCol]) {
                continue;
            }
            auto arrowColIdx = outputToArrowColumnIdx[outCol];
            if (arrowColIdx < 0 || static_cast<uint64_t>(arrowColIdx) >= numChildren ||
                !batch.children[arrowColIdx] || !schema.children[arrowColIdx]) {
                continue;
            }
            auto& outputVector = *outputVectors[outCol];
            auto* childArray = batch.children[arrowColIdx];
            auto* childSchema = schema.children[arrowColIdx];
            common::ArrowNullMaskTree nullMask(childSchema, childArray, childArray->offset,
                childArray->length);
            common::ArrowConverter::fromArrowArray(childSchema, childArray, outputVector, &nullMask,
                childArray->offset + currentMorselStartOffset, 0, numRowsToCopy);
        }
    }
};

} // namespace storage
} // namespace lbug
