#include <algorithm>
#include <bitset>
#include <limits>
#include <random>
#include <vector>

#include "gtest/gtest.h"
#include "misc/IntervalSet.h"

using antlr4::misc::Interval;
using antlr4::misc::IntervalSet;

namespace {

constexpr ssize_t RANGE = 512;

void checkMatches(const std::vector<Interval>& added) {
    IntervalSet set;
    std::bitset<RANGE> expected;
    for (auto& interval : added) {
        set.add(interval);
        for (auto v = interval.a; v <= interval.b; v++) {
            expected.set(v);
        }
    }
    for (ssize_t v = 0; v < RANGE; v++) {
        ASSERT_EQ(set.contains(v), expected.test(v)) << v;
    }
    // Canonical form: sorted, and neither overlapping nor touching.
    auto& intervals = set.getIntervals();
    for (auto i = 1u; i < intervals.size(); i++) {
        ASSERT_GT(intervals[i].a, intervals[i - 1].b + 1);
    }
}

} // namespace

TEST(AntlrIntervalSetTest, AddKeepsCanonicalForm) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<ssize_t> start(0, RANGE - 20), length(0, 6);
    for (auto round = 0; round < 200; round++) {
        std::vector<Interval> added;
        for (auto i = 0; i < 40; i++) {
            auto a = start(rng);
            added.emplace_back(a, a + length(rng));
        }
        checkMatches(added);
        std::sort(added.begin(), added.end(),
            [](const Interval& l, const Interval& r) { return l.a < r.a; });
        checkMatches(added);
    }
    // Sorted input that touches or overlaps the last interval: [1,3] and [4,6] merge, [8,10] is
    // appended, and [10,12] merges with it.
    std::vector<Interval> touching;
    for (ssize_t a : {1, 4, 8, 10}) {
        touching.emplace_back(a, a + 2);
    }
    checkMatches(touching);
}

// The deserializer adds EOF (-1) first: a range starting at 0 must merge with it, one starting
// at 1 must not.
TEST(AntlrIntervalSetTest, EofMergesOnlyWithTouchingRange) {
    IntervalSet merged;
    merged.add(-1);
    merged.add(0, 2);
    ASSERT_EQ(merged.getIntervals().size(), 1u);
    EXPECT_EQ(merged.getIntervals()[0],
        Interval(static_cast<ssize_t>(-1), static_cast<ssize_t>(2)));
    IntervalSet split;
    split.add(-1);
    split.add(1, 2);
    ASSERT_EQ(split.getIntervals().size(), 2u);
    EXPECT_EQ(split.getIntervals()[0],
        Interval(static_cast<ssize_t>(-1), static_cast<ssize_t>(-1)));
    EXPECT_EQ(split.getIntervals()[1], Interval(static_cast<ssize_t>(1), static_cast<ssize_t>(2)));
}

// An empty interval is ignored, and the fast path's comparisons hold at the top of the range.
// Each step keeps ANTLR's adjacent() checks below SSIZE_MAX, where they would overflow.
TEST(AntlrIntervalSetTest, EmptyIntervalAndRangeLimits) {
    constexpr auto MAX = std::numeric_limits<ssize_t>::max();
    IntervalSet set;
    set.add(5, 3);
    EXPECT_TRUE(set.getIntervals().empty());
    set.add(MAX - 3);
    set.add(MAX - 1);
    ASSERT_EQ(set.getIntervals().size(), 2u);
    set.add(MAX - 2);
    ASSERT_EQ(set.getIntervals().size(), 1u);
    set.add(MAX);
    ASSERT_EQ(set.getIntervals().size(), 1u);
    EXPECT_EQ(set.getIntervals()[0], Interval(MAX - 3, MAX));
}
