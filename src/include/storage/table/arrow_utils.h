#pragma once

#include <vector>

#include "common/arrow/arrow_converter.h"
#include "common/vector/value_vector.h"

namespace lbug {
namespace storage {

class ArrowUtils {
public:
    static uint64_t getArrowBatchLength(const ArrowArrayWrapper& array) {
        if (array.length > 0) {
            return array.length;
        }
        if (array.n_children > 0 && array.children && array.children[0]) {
            return array.children[0]->length;
        }
        return 0;
    }

    static std::vector<size_t> getBatchSizes(const std::vector<ArrowArrayWrapper>& arrays) {
        std::vector<size_t> batchSizes;

        for (const auto& array : arrays) {
            batchSizes.push_back(ArrowUtils::getArrowBatchLength(array));
        }

        return batchSizes;
    }

    static int64_t findColumnIdx(const ArrowSchemaWrapper& schema, const std::string& colName) {
        for (int64_t i = 0; i < schema.n_children; ++i) {
            if (schema.children && schema.children[i] && schema.children[i]->name &&
                colName == schema.children[i]->name) {
                return i;
            }
        }
        return -1;
    }

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

            readArrowValues(childSchema, childArray, outputVector,
                childArray->offset + currentMorselStartOffset, 0, numRowsToCopy);
        }
    }

    static void readArrowValues(const ArrowSchema* schema, const ArrowArray* array,
        common::ValueVector& outputVector, uint64_t srcOffset, uint64_t dstOffset, uint64_t count) {
        common::ArrowNullMaskTree nullMask(schema, array, array->offset, array->length);
        common::ArrowConverter::fromArrowArray(schema, array, outputVector, &nullMask, srcOffset,
            dstOffset, count);
    }
};

} // namespace storage
} // namespace lbug
