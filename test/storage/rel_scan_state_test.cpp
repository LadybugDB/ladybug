#include <memory>

#include "common/data_chunk/data_chunk_state.h"
#include "common/vector/value_vector.h"
#include "gtest/gtest.h"
#include "storage/table/rel_table.h"

using namespace lbug::common;
using namespace lbug::storage;

class RestoreBoundNodeBatchTest : public testing::Test {
protected:
    RestoreBoundNodeBatchTest()
        : nodeIDVector{LogicalType::INTERNAL_ID()}, boundState{std::make_shared<DataChunkState>()},
          scanState{&nodeIDVector, {}, std::make_shared<DataChunkState>()} {
        nodeIDVector.setState(boundState);
    }

    SelectionVector& boundSelVector() { return boundState->getSelVectorUnsafe(); }
    void startBatch() { scanState.initState(nullptr, nullptr); }

    ValueVector nodeIDVector;
    std::shared_ptr<DataChunkState> boundState;
    RelTableScanState scanState;
};

TEST_F(RestoreBoundNodeBatchTest, RestoresFilteredBatch) {
    boundSelVector().setToFiltered(3);
    boundSelVector()[0] = 5;
    boundSelVector()[1] = 2;
    boundSelVector()[2] = 9;
    startBatch();
    scanState.setNodeIDVectorToFlat(9);
    ASSERT_TRUE(boundState->isFlat());

    scanState.restoreBoundNodeBatch();

    EXPECT_FALSE(boundState->isFlat());
    ASSERT_EQ(boundSelVector().getSelSize(), 3u);
    EXPECT_EQ(boundSelVector()[0], 5u);
    EXPECT_EQ(boundSelVector()[1], 2u);
    EXPECT_EQ(boundSelVector()[2], 9u);
}

TEST_F(RestoreBoundNodeBatchTest, RestoresUnfilteredBatch) {
    boundSelVector().setToUnfiltered(4);
    startBatch();
    scanState.setNodeIDVectorToFlat(3);

    scanState.restoreBoundNodeBatch();

    EXPECT_FALSE(boundState->isFlat());
    EXPECT_TRUE(boundSelVector().isUnfiltered());
    EXPECT_EQ(boundSelVector().getSelSize(), 4u);
}

TEST_F(RestoreBoundNodeBatchTest, OneNodeBatchStaysFlat) {
    boundState->setToFlat();
    boundSelVector().setToFiltered(1);
    boundSelVector()[0] = 7;
    startBatch();
    scanState.setNodeIDVectorToFlat(7);

    scanState.restoreBoundNodeBatch();

    EXPECT_TRUE(boundState->isFlat());
    ASSERT_EQ(boundSelVector().getSelSize(), 1u);
    EXPECT_EQ(boundSelVector()[0], 7u);
}

TEST_F(RestoreBoundNodeBatchTest, RestoresABatchOnlyOnce) {
    boundSelVector().setToUnfiltered(3);
    startBatch();
    scanState.setNodeIDVectorToFlat(2);
    scanState.restoreBoundNodeBatch();

    // The chunk's owner refills it, and the caller pulls again without starting a batch.
    boundSelVector().setToFiltered(2);
    boundSelVector()[0] = 8;
    boundSelVector()[1] = 6;
    scanState.restoreBoundNodeBatch();

    ASSERT_EQ(boundSelVector().getSelSize(), 2u);
    EXPECT_EQ(boundSelVector()[0], 8u);
    EXPECT_EQ(boundSelVector()[1], 6u);
}
