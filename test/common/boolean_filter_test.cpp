#include <memory>

#include "common/data_chunk/data_chunk_state.h"
#include "common/data_chunk/sel_vector.h"
#include "common/system_config.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "expression_evaluator/expression_evaluator.h"
#include "gtest/gtest.h"

using namespace lbug::common;
using namespace lbug::evaluator;

namespace {

// Test adapter that exposes the protected updateSelectedPos method so we can
// exercise the boolean-filter fallback path directly, without constructing a
// full query plan (issue #692 regression).
class BooleanFilterTester : public ExpressionEvaluator {
public:
    explicit BooleanFilterTester(std::shared_ptr<ValueVector> resultVector)
        : ExpressionEvaluator(EvaluatorType::LITERAL, nullptr) {
        this->resultVector = std::move(resultVector);
    }

    bool invokeUpdateSelectedPos(SelectionVector& selVector) const {
        return updateSelectedPos(selVector);
    }

    void evaluate() override {}
    std::unique_ptr<ExpressionEvaluator> copy() override {
        // Not used in this test.
        return nullptr;
    }

protected:
    void resolveResultVector(const lbug::processor::ResultSet&,
        lbug::storage::MemoryManager*) override {}
    bool selectInternal(SelectionVector&) override { return false; }
};

} // namespace

// ---------------------------------------------------------------------------
// Bug regression: unflat state with a single selected position.
//
// When the result vector's state is UNFLAT but the current selection happens
// to hold exactly one position, updateSelectedPos must walk the selection and
// write the buffer. The pre-fix shortcut (checking getSelSize() > 1)
// incorrectly skipped the write for this case, leaving stale positions in the
// selection buffer that the caller activates via setToFiltered().
// ---------------------------------------------------------------------------
TEST(BooleanFilterTest, UnflatSingleSelectionWritesBuffer) {
    // Build an output SelectionVector and pre-seed its buffer with "stale"
    // positions (e.g. the index that would have matched a *previous* batch).
    SelectionVector outSel{DEFAULT_VECTOR_CAPACITY};
    outSel.setToFiltered();
    auto buffer = outSel.getMutableBuffer();
    buffer[0] = 99; // stale position
    buffer[1] = 99;
    outSel.setSelSize(0);

    // Build a boolean result vector with an UNFLAT state whose current
    // selection has size 1.  This is the exact scenario that triggered #692:
    // a non-flat chunk whose selection window narrowed to a single row.
    auto resultVector = std::make_shared<ValueVector>(LogicalType::BOOL());
    auto state = std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY);
    state->setToUnflat();
    state->getSelVectorUnsafe().setRange(0, 1); // positions [0]
    state->getSelVectorUnsafe().setSelSize(1);
    resultVector->setState(state);

    // Set the boolean value at position 0 to true (selected).
    resultVector->setValue<bool>(0, true);
    resultVector->setNull(0, false);

    BooleanFilterTester tester(resultVector);
    bool result = tester.invokeUpdateSelectedPos(outSel);

    // The output selection vector must have size 1 and must hold position 0,
    // NOT the stale value 99 that was in the buffer before the call.
    ASSERT_TRUE(result);
    ASSERT_EQ(outSel.getSelSize(), 1u);
    EXPECT_EQ(outSel[0], 0u);
}

TEST(BooleanFilterTest, UnflatSingleSelectionSkipsNull) {
    SelectionVector outSel{DEFAULT_VECTOR_CAPACITY};
    outSel.setToFiltered();
    outSel.setSelSize(0);
    auto buffer = outSel.getMutableBuffer();
    buffer[0] = 99;

    auto resultVector = std::make_shared<ValueVector>(LogicalType::BOOL());
    auto state = std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY);
    state->setToUnflat();
    state->getSelVectorUnsafe().setRange(0, 1);
    state->getSelVectorUnsafe().setSelSize(1);
    resultVector->setState(state);

    // The value at position 0 is NULL -> should not be selected.
    resultVector->setValue<bool>(0, true);
    resultVector->setNull(0, true);

    BooleanFilterTester tester(resultVector);
    bool result = tester.invokeUpdateSelectedPos(outSel);

    ASSERT_FALSE(result);
    ASSERT_EQ(outSel.getSelSize(), 0u);
}

TEST(BooleanFilterTest, UnflatSingleSelectionFalseValue) {
    SelectionVector outSel{DEFAULT_VECTOR_CAPACITY};
    outSel.setToFiltered();
    outSel.setSelSize(0);
    auto buffer = outSel.getMutableBuffer();
    buffer[0] = 99;

    auto resultVector = std::make_shared<ValueVector>(LogicalType::BOOL());
    auto state = std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY);
    state->setToUnflat();
    state->getSelVectorUnsafe().setRange(0, 1);
    state->getSelVectorUnsafe().setSelSize(1);
    resultVector->setState(state);

    // The value at position 0 is false -> should not be selected.
    resultVector->setValue<bool>(0, false);
    resultVector->setNull(0, false);

    BooleanFilterTester tester(resultVector);
    bool result = tester.invokeUpdateSelectedPos(outSel);

    ASSERT_FALSE(result);
    ASSERT_EQ(outSel.getSelSize(), 0u);
}

// ---------------------------------------------------------------------------
// Unflat state with 2 selected positions (also hit the bug when the selection
// was built across batches).
// ---------------------------------------------------------------------------
TEST(BooleanFilterTest, UnflatTwoSelections) {
    SelectionVector outSel{DEFAULT_VECTOR_CAPACITY};
    outSel.setToFiltered();
    outSel.setSelSize(0);

    auto resultVector = std::make_shared<ValueVector>(LogicalType::BOOL());
    auto state = std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY);
    state->setToUnflat();
    state->getSelVectorUnsafe().setRange(0, 2);
    state->getSelVectorUnsafe().setSelSize(2);
    resultVector->setState(state);

    resultVector->setValue<bool>(0, false);
    resultVector->setNull(0, false);
    resultVector->setValue<bool>(1, true);
    resultVector->setNull(1, false);

    BooleanFilterTester tester(resultVector);
    bool result = tester.invokeUpdateSelectedPos(outSel);

    ASSERT_TRUE(result);
    ASSERT_EQ(outSel.getSelSize(), 1u);
    EXPECT_EQ(outSel[0], 1u);
}

// ---------------------------------------------------------------------------
// Unflat state with multiple selections (already worked before the fix, but
// make sure it still works).
// ---------------------------------------------------------------------------
TEST(BooleanFilterTest, UnflatMultipleSelections) {
    SelectionVector outSel{DEFAULT_VECTOR_CAPACITY};
    outSel.setToFiltered();
    outSel.setSelSize(0);

    auto resultVector = std::make_shared<ValueVector>(LogicalType::BOOL());
    auto state = std::make_shared<DataChunkState>(DEFAULT_VECTOR_CAPACITY);
    state->setToUnflat();
    state->getSelVectorUnsafe().setRange(0, 5); // positions [0..4]
    state->getSelVectorUnsafe().setSelSize(5);
    resultVector->setState(state);

    // pos 0: true  (selected)
    // pos 1: false (not selected)
    // pos 2: true  (selected)
    // pos 3: true  (selected)
    // pos 4: false (not selected)
    for (auto i = 0u; i < 5; ++i) {
        resultVector->setValue<bool>(i, i == 0 || i == 2 || i == 3);
        resultVector->setNull(i, false);
    }

    BooleanFilterTester tester(resultVector);
    bool result = tester.invokeUpdateSelectedPos(outSel);

    ASSERT_TRUE(result);
    ASSERT_EQ(outSel.getSelSize(), 3u);
    EXPECT_EQ(outSel[0], 0u);
    EXPECT_EQ(outSel[1], 2u);
    EXPECT_EQ(outSel[2], 3u);
}

// ---------------------------------------------------------------------------
// Flat state with single selection (the "fast path" that skips the buffer
// write).  This must still work because selection on a flat vector is driven
// by the leading child's position, not by a constructed buffer.
// ---------------------------------------------------------------------------
TEST(BooleanFilterTest, FlatSingleSelection) {
    SelectionVector outSel{DEFAULT_VECTOR_CAPACITY};
    outSel.setToFiltered();
    outSel.setSelSize(0);

    auto resultVector = std::make_shared<ValueVector>(LogicalType::BOOL());
    auto state = std::make_shared<DataChunkState>();
    state->setToFlat();
    state->initOriginalAndSelectedSize(1);
    resultVector->setState(state);

    resultVector->setValue<bool>(0, true);
    resultVector->setNull(0, false);

    BooleanFilterTester tester(resultVector);
    bool result = tester.invokeUpdateSelectedPos(outSel);

    ASSERT_TRUE(result);
    // Flat path does not touch the selection buffer, so the vector stays at
    // the initial size 0 -- the caller would normally handle this via
    // setToFiltered only when isUnfiltered().
    EXPECT_EQ(outSel.getSelSize(), 0u);
}

TEST(BooleanFilterTest, FlatSingleSelectionFalse) {
    SelectionVector outSel{DEFAULT_VECTOR_CAPACITY};
    outSel.setToFiltered();
    outSel.setSelSize(0);

    auto resultVector = std::make_shared<ValueVector>(LogicalType::BOOL());
    auto state = std::make_shared<DataChunkState>();
    state->setToFlat();
    state->initOriginalAndSelectedSize(1);
    resultVector->setState(state);

    resultVector->setValue<bool>(0, false);
    resultVector->setNull(0, false);

    BooleanFilterTester tester(resultVector);
    bool result = tester.invokeUpdateSelectedPos(outSel);

    ASSERT_FALSE(result);
    EXPECT_EQ(outSel.getSelSize(), 0u);
}
