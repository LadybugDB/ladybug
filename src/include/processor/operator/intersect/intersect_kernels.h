#pragma once

#include <algorithm>

#include "common/assert.h"
#include "common/types/types.h"

namespace lbug {
namespace processor {

namespace intersect_kernels_detail {

constexpr common::sel_t MIN_GALLOPING_RIGHT_COUNT = 64;
constexpr common::sel_t MIN_GALLOPING_SIZE_RATIO = 8;

inline bool isHomogeneous(const common::nodeID_t* values, common::sel_t count) {
    return count != 0 && values[0].tableID == values[count - 1].tableID;
}

inline common::sel_t lowerBoundOffset(const common::nodeID_t* values, common::sel_t begin,
    common::sel_t end, common::offset_t target) {
    while (begin < end) {
        const auto middle = begin + (end - begin) / 2;
        if (values[middle].offset < target) {
            begin = middle + 1;
        } else {
            end = middle;
        }
    }
    return begin;
}

// Branchless lower bound over offsets, in the style of CRoaring's binarySearch: the caller
// advances the base monotonically, so every probe searches a shrinking suffix. Unlike galloping
// there is no exponential probing phase; each left element pays a full binary search.
inline common::sel_t lowerBoundOffsetBranchless(const common::nodeID_t* values, common::sel_t count,
    common::offset_t target) {
    const auto* base = values;
    auto n = count;
    while (n > 1) {
        const auto half = n >> 1;
        base = (base[half].offset < target) ? &base[half] : base;
        n -= half;
    }
    if (n == 0) {
        return 0;
    }
    return static_cast<common::sel_t>((base[0].offset < target) + (base - values));
}

// CRoaring-style skewed intersection (cf. intersect_skewed_uint16): one branchless binary search
// per left element over the remaining right suffix. Same output contract as the galloping path:
// matches are compacted into left[] and both sides' positions are recorded.
inline common::sel_t intersectSameTableBatchedSearch(common::nodeID_t* left,
    common::sel_t leftCount, const common::nodeID_t* right, common::sel_t rightCount,
    common::sel_t* leftPositions, common::sel_t* rightPositions) {
    common::sel_t rightPosition = 0;
    common::sel_t outputPosition = 0;
    for (common::sel_t leftPosition = 0; leftPosition < leftCount; ++leftPosition) {
        const auto leftNodeID = left[leftPosition];
        rightPosition += lowerBoundOffsetBranchless(right + rightPosition,
            rightCount - rightPosition, leftNodeID.offset);
        if (rightPosition == rightCount) {
            break;
        }
        if (right[rightPosition].offset == leftNodeID.offset) {
            leftPositions[outputPosition] = leftPosition;
            rightPositions[outputPosition] = rightPosition;
            left[outputPosition++] = leftNodeID;
            ++rightPosition;
        }
    }
    return outputPosition;
}

inline common::sel_t intersectSameTableGalloping(common::nodeID_t* left, common::sel_t leftCount,
    const common::nodeID_t* right, common::sel_t rightCount, common::sel_t* leftPositions,
    common::sel_t* rightPositions) {
    common::sel_t leftPosition = 0;
    common::sel_t rightPosition = 0;
    common::sel_t outputPosition = 0;
    while (leftPosition < leftCount && rightPosition < rightCount) {
        const auto leftNodeID = left[leftPosition];
        const auto target = leftNodeID.offset;
        if (right[rightPosition].offset < target) {
            common::sel_t step = 1;
            const auto remaining = rightCount - rightPosition;
            while (step < remaining && right[rightPosition + step].offset < target) {
                step = step > remaining - step ? remaining : step + step;
            }
            const auto begin = std::min(rightPosition + (step >> 1) + 1, rightCount);
            const auto end = std::min(rightPosition + step + 1, rightCount);
            rightPosition = lowerBoundOffset(right, begin, end, target);
            if (rightPosition == rightCount) {
                break;
            }
        }
        if (right[rightPosition].offset == target) {
            leftPositions[outputPosition] = leftPosition;
            rightPositions[outputPosition] = rightPosition;
            left[outputPosition++] = leftNodeID;
            ++rightPosition;
        }
        ++leftPosition;
    }
    return outputPosition;
}

} // namespace intersect_kernels_detail

// Reference merge used for mixed-table lists, balanced lists, and differential tests.
inline common::sel_t intersectNodeIDsScalar(common::nodeID_t* left, common::sel_t leftCount,
    const common::nodeID_t* right, common::sel_t rightCount, common::sel_t* leftPositions,
    common::sel_t* rightPositions) {
    common::sel_t leftPosition = 0;
    common::sel_t rightPosition = 0;
    common::sel_t outputPosition = 0;
    while (leftPosition < leftCount && rightPosition < rightCount) {
        const auto leftNodeID = left[leftPosition];
        const auto rightNodeID = right[rightPosition];
        if (leftNodeID < rightNodeID) {
            ++leftPosition;
        } else if (leftNodeID > rightNodeID) {
            ++rightPosition;
        } else {
            leftPositions[outputPosition] = leftPosition;
            rightPositions[outputPosition] = rightPosition;
            left[outputPosition++] = leftNodeID;
            ++leftPosition;
            ++rightPosition;
        }
    }
    return outputPosition;
}

// Same gating as intersectNodeIDs but uses the CRoaring-style batched binary search for the
// same-table fast path. Exists for differential testing and benchmarking against galloping.
inline common::sel_t intersectNodeIDsBatched(common::nodeID_t* left, common::sel_t leftCount,
    const common::nodeID_t* right, common::sel_t rightCount, common::sel_t* leftPositions,
    common::sel_t* rightPositions) {
    DASSERT(leftCount <= rightCount);
    using namespace intersect_kernels_detail;
    if (leftCount == 0 || rightCount < MIN_GALLOPING_RIGHT_COUNT ||
        rightCount / leftCount < MIN_GALLOPING_SIZE_RATIO || !isHomogeneous(left, leftCount) ||
        !isHomogeneous(right, rightCount) || left[0].tableID != right[0].tableID) {
        return intersectNodeIDsScalar(left, leftCount, right, rightCount, leftPositions,
            rightPositions);
    }
    return intersectSameTableBatchedSearch(left, leftCount, right, rightCount, leftPositions,
        rightPositions);
}

// Selects the skew-aware same-table fast path when it is profitable and otherwise uses the
// reference merge. Both inputs must be sorted by nodeID_t's lexicographic ordering, and leftCount
// must be less than or equal to rightCount.
inline common::sel_t intersectNodeIDs(common::nodeID_t* left, common::sel_t leftCount,
    const common::nodeID_t* right, common::sel_t rightCount, common::sel_t* leftPositions,
    common::sel_t* rightPositions) {
    DASSERT(leftCount <= rightCount);
    using namespace intersect_kernels_detail;
    if (leftCount == 0 || rightCount < MIN_GALLOPING_RIGHT_COUNT ||
        rightCount / leftCount < MIN_GALLOPING_SIZE_RATIO || !isHomogeneous(left, leftCount) ||
        !isHomogeneous(right, rightCount) || left[0].tableID != right[0].tableID) {
        return intersectNodeIDsScalar(left, leftCount, right, rightCount, leftPositions,
            rightPositions);
    }
    return intersectSameTableGalloping(left, leftCount, right, rightCount, leftPositions,
        rightPositions);
}

} // namespace processor
} // namespace lbug
