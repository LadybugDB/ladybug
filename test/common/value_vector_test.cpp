#include "common/exception/internal.h"
#include "common/vector/value_vector.h"
#include "gtest/gtest.h"

using namespace lbug::common;

TEST(ValueVectorTests, SetValueRejectsAnotherValueSize) {
#if defined(RUNTIME_CHECKS) || !defined(NDEBUG)
    std::vector<StructField> fields;
    fields.emplace_back("a", LogicalType::INT64());
    ValueVector unionVector(LogicalType::UNION(std::move(fields)));
    EXPECT_THROW(unionVector.setValue<union_field_idx_t>(1, 0), InternalException);
    EXPECT_EQ(unionVector.getValue<struct_entry_t>(1).pos, 1);
    EXPECT_NO_THROW(UnionVector::getTagVector(&unionVector)->setValue<union_field_idx_t>(1, 0));

    ValueVector int32Vector(LogicalType::INT32());
    EXPECT_THROW(int32Vector.setValue<int64_t>(0, 1), InternalException);
    int32Vector.setValue<int32_t>(0, 1);
    EXPECT_EQ(int32Vector.getValue<int32_t>(0), 1);
#else
    GTEST_SKIP() << "the value size check is a DASSERT";
#endif
}
