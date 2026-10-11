#include "binder/binder.h"
#include "binder/visitor/confidential_statement_analyzer.h"
#include "graph_test/private_graph_test.h"
#include "parser/parser.h"

namespace lbug {
namespace testing {

TEST_F(EmptyDBTest, ConfidentialStatementVisitor) {
    createDBAndConn();
    conn->query("BEGIN TRANSACTION");
    database->addExtensionOption("binder_test_conf", common::LogicalTypeID::UINT64,
        common::Value{5}, true /* isConfidential */);
    database->addExtensionOption("binder_test_not_conf", common::LogicalTypeID::UINT64,
        common::Value{2}, false /* isConfidential */);
    binder::Binder binder{conn->getClientContext()};

    binder::ConfidentialStatementAnalyzer analyzerConfidential{};
    auto parsed = parser::Parser::parseQuery("CALL binder_test_conf = 3");
    auto boundStatement = binder.bind(*parsed[0]);
    analyzerConfidential.visit(*boundStatement);
    ASSERT_TRUE(analyzerConfidential.isConfidential());

    binder::ConfidentialStatementAnalyzer analyzerNotConfidential{};
    parsed = parser::Parser::parseQuery("CALL binder_test_not_conf = 444");
    boundStatement = binder.bind(*parsed[0]);
    analyzerConfidential.visit(*boundStatement);
    ASSERT_FALSE(analyzerConfidential.isConfidential());

    parsed = parser::Parser::parseQuery("CALL timeout = 4");
    boundStatement = binder.bind(*parsed[0]);
    analyzerNotConfidential.visit(*boundStatement);
    ASSERT_FALSE(analyzerNotConfidential.isConfidential());
}

TEST_F(EmptyDBTest, LargeConstantListIsNotFolded) {
    createDBAndConn();
    conn->query("BEGIN TRANSACTION");
    auto bindReturnType = [&](const std::string& expression) {
        binder::Binder binder{conn->getClientContext()};
        auto parsed = parser::Parser::parseQuery("RETURN " + expression);
        return binder.bind(*parsed[0])->getStatementResult()->getColumns()[0]->expressionType;
    };
    const auto literal = common::ExpressionType::LITERAL;
    const auto function = common::ExpressionType::FUNCTION;
    EXPECT_EQ(bindReturnType("range(1, 8192)"), literal);
    EXPECT_EQ(bindReturnType("range(1, 8193)"), function);
    EXPECT_EQ(bindReturnType("[range(1, 9000)]"), function);
    EXPECT_EQ(bindReturnType("{a: range(1, 9000)}"), function);
    // Built from folded parts, or written out in the query: no larger than the expression.
    EXPECT_EQ(bindReturnType("[range(1, 5000), range(1, 5000)]"), literal);
    std::string writtenOutList = "[0";
    for (auto i = 1; i < 10000; i++) {
        writtenOutList += ", " + std::to_string(i);
    }
    writtenOutList += "]";
    EXPECT_EQ(bindReturnType(writtenOutList), literal);
    EXPECT_EQ(bindReturnType("CASE WHEN true THEN " + writtenOutList + " ELSE [1] END"), literal);
}

} // namespace testing
} // namespace lbug
