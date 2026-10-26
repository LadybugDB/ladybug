#include <string>

#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "function/string/functions/base_pad_function.h"
#include "gtest/gtest.h"

using namespace lbug::common;

TEST(StringTest, splitBySpace) {
    std::string str = " a b  c\td ";
    std::vector<std::string> result = StringUtils::splitBySpace(str);
    EXPECT_EQ(result.size(), 4);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "b");
    EXPECT_EQ(result[2], "c");
    EXPECT_EQ(result[3], "d");
}

TEST(StringTest, leftTrim) {
    std::string str = "    command ";
    std::string result = StringUtils::ltrim(str);
    EXPECT_EQ(result, "command ");
}

TEST(StringTest, rightTrim) {
    std::string str = " command    ";
    std::string result = StringUtils::rtrim(str);
    EXPECT_EQ(result, " command");
}

TEST(StringTest, extractStringBetween) {
    std::string str = "command (a b c)";
    std::string result = StringUtils::extractStringBetween(str, '(', ')', false);
    EXPECT_EQ(result, "a b c");
    result = StringUtils::extractStringBetween(str, '(', ')', true);
    EXPECT_EQ(result, "(a b c)");
}

TEST(StringTest, extractStringBetweenNoDelimiterFound) {
    std::string str = "command (a b c)";
    std::string result = StringUtils::extractStringBetween(str, '"', ')', false);
    EXPECT_TRUE(result.empty());
    result = StringUtils::extractStringBetween(str, '(', '.', true);
    EXPECT_TRUE(result.empty());
    result = StringUtils::extractStringBetween(str, ')', '(', true);
    EXPECT_TRUE(result.empty());
}

// checkResultSize counts the bytes without building the string, so widths whose results reach
// the maximum string size can be checked here.
TEST(StringTest, padResultSize) {
    using lbug::function::BasePadOperation;
    string_t empty("", 0), hi("hi", 2), x("x", 1), wide("成", 3), mixed("x成", 4),
        three("成成x", 7);
    EXPECT_NO_THROW(BasePadOperation::checkResultSize(empty, 1431655765, wide));
    EXPECT_THROW(BasePadOperation::checkResultSize(empty, 1431655766, wide), RuntimeException);
    EXPECT_NO_THROW(BasePadOperation::checkResultSize(empty, 2147483647, mixed));
    EXPECT_THROW(BasePadOperation::checkResultSize(empty, 2147483648, mixed), RuntimeException);
    EXPECT_NO_THROW(BasePadOperation::checkResultSize(empty, 1840700269, three));
    EXPECT_THROW(BasePadOperation::checkResultSize(empty, 1840700270, three), RuntimeException);
    EXPECT_NO_THROW(BasePadOperation::checkResultSize(hi, string_t::MAX_LENGTH, x));
    EXPECT_THROW(BasePadOperation::checkResultSize(hi, string_t::MAX_LENGTH + 1, x),
        RuntimeException);
    EXPECT_NO_THROW(BasePadOperation::checkResultSize(hi, string_t::MAX_LENGTH, empty));
}
