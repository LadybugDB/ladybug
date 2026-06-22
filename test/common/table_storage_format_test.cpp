#include "common/enums/table_storage_format.h"
#include "common/exception/binder.h"
#include "gtest/gtest.h"

using namespace lbug::common;

TEST(TableStorageFormatUtilsTest, ToString) {
    EXPECT_EQ("unknown", TableStorageFormatUtils::toString(TableStorageFormat::UNKNOWN));
    EXPECT_EQ("native", TableStorageFormatUtils::toString(TableStorageFormat::NATIVE));
    EXPECT_EQ("icebug-disk", TableStorageFormatUtils::toString(TableStorageFormat::ICEBUG_DISK));
    EXPECT_EQ("arrow", TableStorageFormatUtils::toString(TableStorageFormat::ARROW));
    EXPECT_EQ("foreign", TableStorageFormatUtils::toString(TableStorageFormat::FOREIGN));
}
