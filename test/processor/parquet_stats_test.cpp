#include <limits>

#include "gtest/gtest.h"
#include "processor/operator/persistent/writer/parquet/standard_column_writer.h"

using namespace lbug::processor;

namespace {

template<typename T>
void verifyNaNStaysOutOfMinMax() {
    using Stats = NumericStatisticsState<T, T, BaseParquetOperator>;
    const auto nan = std::numeric_limits<T>::quiet_NaN();
    auto stats = BaseParquetOperator::initializeStats<T, T>();
    for (const auto value : {nan, T{1.5}, -nan, T{-2}}) {
        BaseParquetOperator::handleStats<T, T>(stats.get(), value, value);
    }
    auto& numericStats = static_cast<Stats&>(*stats);
    EXPECT_EQ(numericStats.min, T{-2});
    EXPECT_EQ(numericStats.max, T{1.5});

    auto nanOnly = BaseParquetOperator::initializeStats<T, T>();
    BaseParquetOperator::handleStats<T, T>(nanOnly.get(), nan, nan);
    EXPECT_TRUE(nanOnly->getMinValue().empty());
    EXPECT_TRUE(nanOnly->getMaxValue().empty());
}

} // namespace

TEST(ParquetStatsTest, NaNStaysOutOfMinMax) {
    verifyNaNStaysOutOfMinMax<float>();
    verifyNaNStaysOutOfMinMax<double>();
}
