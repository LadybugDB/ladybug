#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
    auto batchedLeft = left;
    std::vector<common::sel_t> scalarLeftPositions(left.size());
    std::vector<common::sel_t> scalarRightPositions(left.size());
    std::vector<common::sel_t> fastLeftPositions(left.size());
    std::vector<common::sel_t> fastRightPositions(left.size());
    std::vector<common::sel_t> batchedLeftPositions(left.size());
    std::vector<common::sel_t> batchedRightPositions(left.size());

    const auto scalarCount = processor::intersectNodeIDsScalar(scalarLeft.data(), scalarLeft.size(),
        right.data(), right.size(), scalarLeftPositions.data(), scalarRightPositions.data());
    const auto fastCount = processor::intersectNodeIDs(left.data(), left.size(), right.data(),
        right.size(), fastLeftPositions.data(), fastRightPositions.data());
    const auto batchedCount =
        processor::intersectNodeIDsBatched(batchedLeft.data(), batchedLeft.size(), right.data(),
            right.size(), batchedLeftPositions.data(), batchedRightPositions.data());

    ASSERT_EQ(fastCount, scalarCount);
    EXPECT_TRUE(std::equal(left.begin(), left.begin() + fastCount, scalarLeft.begin()));
    EXPECT_TRUE(std::equal(fastLeftPositions.begin(), fastLeftPositions.begin() + fastCount,
        scalarLeftPositions.begin()));
    EXPECT_TRUE(std::equal(fastRightPositions.begin(), fastRightPositions.begin() + fastCount,
        scalarRightPositions.begin()));
    ASSERT_EQ(batchedCount, scalarCount);
    EXPECT_TRUE(
        std::equal(batchedLeft.begin(), batchedLeft.begin() + batchedCount, scalarLeft.begin()));
    EXPECT_TRUE(std::equal(batchedLeftPositions.begin(),
        batchedLeftPositions.begin() + batchedCount, scalarLeftPositions.begin()));
    EXPECT_TRUE(std::equal(batchedRightPositions.begin(),
        batchedRightPositions.begin() + batchedCount, scalarRightPositions.begin()));
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

namespace {

struct BenchmarkConfig {
    uint64_t leftCount;
    uint64_t rightCount;
    uint64_t domain;
    const char* label;
};

template<typename Kernel>
uint64_t timeKernel(Kernel kernel, const std::vector<common::nodeID_t>& left,
    const std::vector<common::nodeID_t>& right, int iterations) {
    std::vector<common::nodeID_t> workLeft(left.size());
    std::vector<common::sel_t> leftPositions(left.size());
    std::vector<common::sel_t> rightPositions(left.size());
    // Warm up.
    for (auto i = 0; i < 100; ++i) {
        std::copy(left.begin(), left.end(), workLeft.begin());
        kernel(workLeft.data(), workLeft.size(), right.data(), right.size(), leftPositions.data(),
            rightPositions.data());
    }
    volatile uint64_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (auto i = 0; i < iterations; ++i) {
        std::copy(left.begin(), left.end(), workLeft.begin());
        sink += kernel(workLeft.data(), workLeft.size(), right.data(), right.size(),
            leftPositions.data(), rightPositions.data());
    }
    const auto end = std::chrono::steady_clock::now();
    if (sink == UINT64_MAX) {
        std::printf("unreachable\n");
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / iterations;
}

} // namespace

TEST(IntersectKernelsTest, BenchmarkSkewedPaths) {
    constexpr int kIterations = 2000;
    constexpr uint64_t kRightCount = 2048;
    const BenchmarkConfig configs[] = {
        {8, kRightCount, kRightCount * 64, "sparse"},
        {32, kRightCount, kRightCount * 64, "sparse"},
        {256, kRightCount, kRightCount * 64, "sparse"},
        {8, kRightCount, kRightCount * 2, "dense"},
        {32, kRightCount, kRightCount * 2, "dense"},
        {256, kRightCount, kRightCount * 2, "dense"},
    };
    std::printf("\n%-16s %8s %12s %12s %12s %12s %12s\n", "config", "matches", "scalar(ns)",
        "gallop(ns)", "batched(ns)", "gallop/scal", "batch/scal");
    for (const auto& config : configs) {
        uint64_t scalarNs = 0, gallopNs = 0, batchedNs = 0, matches = 0;
        for (auto seed = 0u; seed < 3; ++seed) {
            const auto left = makeSortedIDs(config.leftCount, config.domain, 7, seed);
            const auto right = makeSortedIDs(config.rightCount, config.domain, 7, seed + 1000);
            verifyMatchesScalar(left, right);
            std::vector<common::nodeID_t> probe = left;
            std::vector<common::sel_t> lPos(left.size()), rPos(left.size());
            matches += processor::intersectNodeIDsScalar(probe.data(), probe.size(), right.data(),
                right.size(), lPos.data(), rPos.data());
            scalarNs += timeKernel(processor::intersectNodeIDsScalar, left, right, kIterations);
            gallopNs += timeKernel(processor::intersectNodeIDs, left, right, kIterations);
            batchedNs += timeKernel(processor::intersectNodeIDsBatched, left, right, kIterations);
        }
        std::printf("L=%-4lu R=%-4lu %-6s %8lu %12lu %12lu %12lu %11.2fx %11.2fx\n",
            config.leftCount, config.rightCount, config.label, matches / 3, scalarNs / 3,
            gallopNs / 3, batchedNs / 3, static_cast<double>(scalarNs) / gallopNs,
            static_cast<double>(scalarNs) / batchedNs);
    }
}
