#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "common/types/types.h"
#include "gtest/gtest.h"
#include "processor/operator/intersect/intersect_kernels.h"

using namespace lbug;

namespace {

void verifyMatchesScalar(std::vector<common::nodeID_t> left,
    const std::vector<common::nodeID_t>& right) {
    auto scalarLeft = left;
    std::vector<common::sel_t> scalarLeftPositions(left.size());
    std::vector<common::sel_t> scalarRightPositions(left.size());
    std::vector<common::sel_t> fastLeftPositions(left.size());
    std::vector<common::sel_t> fastRightPositions(left.size());

    const auto scalarCount = processor::intersectNodeIDsScalar(scalarLeft.data(), scalarLeft.size(),
        right.data(), right.size(), scalarLeftPositions.data(), scalarRightPositions.data());
    const auto fastCount = processor::intersectNodeIDs(left.data(), left.size(), right.data(),
        right.size(), fastLeftPositions.data(), fastRightPositions.data());

    ASSERT_EQ(fastCount, scalarCount);
    EXPECT_TRUE(std::equal(left.begin(), left.begin() + fastCount, scalarLeft.begin()));
    EXPECT_TRUE(std::equal(fastLeftPositions.begin(), fastLeftPositions.begin() + fastCount,
        scalarLeftPositions.begin()));
    EXPECT_TRUE(std::equal(fastRightPositions.begin(), fastRightPositions.begin() + fastCount,
        scalarRightPositions.begin()));
}

std::vector<common::nodeID_t> makeSortedIDs(uint64_t count, uint64_t domain,
    common::table_id_t tableID, uint64_t seed) {
    std::mt19937_64 random{seed};
    std::vector<common::nodeID_t> result;
    result.reserve(count);
    for (auto i = 0u; i < count; ++i) {
        result.emplace_back(random() % domain, tableID);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace

TEST(IntersectKernelsTest, MatchesScalarAcrossSizesAndSkews) {
    for (const auto& [leftCount, rightCount] :
        {std::pair{1u, 64u}, std::pair{8u, 8u}, std::pair{16u, 128u}, std::pair{32u, 2048u},
            std::pair{128u, 128u}, std::pair{256u, 2048u}}) {
        for (auto seed = 0u; seed < 20; ++seed) {
            const auto domain = std::max<uint64_t>(rightCount * 4, 1);
            verifyMatchesScalar(makeSortedIDs(leftCount, domain, 7, seed),
                makeSortedIDs(rightCount, domain, 7, seed + 1000));
        }
    }
}

TEST(IntersectKernelsTest, PreservesDuplicatePairing) {
    std::vector<common::nodeID_t> left = {{1, 7}, {1, 7}, {2, 7}, {8, 7}, {8, 7}, {8, 7}, {64, 7},
        {128, 7}};
    std::vector<common::nodeID_t> right(128, common::nodeID_t{3, 7});
    right[0] = {1, 7};
    right[1] = {1, 7};
    right[2] = {1, 7};
    right[60] = {8, 7};
    right[61] = {8, 7};
    right[126] = {128, 7};
    right[127] = {128, 7};
    std::sort(right.begin(), right.end());
    verifyMatchesScalar(left, right);
}

TEST(IntersectKernelsTest, FallsBackForMixedTableIDs) {
    std::vector<common::nodeID_t> left = {{0, 1}, {1, 1}, {0, 2}, {4, 2}};
    std::vector<common::nodeID_t> right;
    for (auto i = 0u; i < 128; ++i) {
        right.emplace_back(i, i < 64 ? 1 : 2);
    }
    std::sort(right.begin(), right.end());
    verifyMatchesScalar(left, right);
}

TEST(IntersectKernelsTest, HandlesEmptyLeftInput) {
    std::vector<common::nodeID_t> left;
    std::vector<common::nodeID_t> right = {{1, 7}, {2, 7}};
    verifyMatchesScalar(left, right);
}
