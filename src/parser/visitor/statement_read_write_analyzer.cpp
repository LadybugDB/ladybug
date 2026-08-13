#include "parser/visitor/statement_read_write_analyzer.h"

#include <algorithm>

#include "common/string_utils.h"
#include "main/client_context.h"
#include "main/db_config.h"
#include "parser/expression/parsed_expression_visitor.h"
#include "parser/expression/parsed_function_expression.h"
#include "parser/query/reading_clause/reading_clause.h"
#include "parser/query/return_with_clause/with_clause.h"
#include "parser/standalone_call_function.h"

namespace lbug {
namespace parser {

void StatementReadWriteAnalyzer::visitStandaloneCallFunction(const Statement& statement) {
    // Standalone CALL functions are write-classified by default: extensions can register
    // storage-writing calls (e.g. index builds), and the analyzer runs before binding, so the
    // name is all we have. The graph-projection trio only mutates the session-local
    // GraphEntrySet — no storage is touched — so it is safe (and useful) on read-only
    // databases: project + run GDS over a read-only replica. Names match
    // function/table/standalone_call_function.h.
    static constexpr std::string_view READ_ONLY_SAFE_FUNCS[] = {"PROJECT_GRAPH",
        "PROJECT_GRAPH_CYPHER", "DROP_PROJECTED_GRAPH"};
    const auto& funcExpr = statement.constCast<StandaloneCallFunction>()
                               .getFunctionExpression()
                               ->constCast<ParsedFunctionExpression>();
    const auto name = common::StringUtils::getUpper(funcExpr.getFunctionName());
    readOnly = std::find(std::begin(READ_ONLY_SAFE_FUNCS), std::end(READ_ONLY_SAFE_FUNCS), name) !=
               std::end(READ_ONLY_SAFE_FUNCS);
}

void StatementReadWriteAnalyzer::visitExtension(const Statement& /*statement*/) {
    // We allow LOAD EXTENSION to run in read-only mode.
    if (context->getDBConfig()->readOnly) {
        readOnly = true;
    } else {
        readOnly = false;
    }
}

void StatementReadWriteAnalyzer::visitReadingClause(const ReadingClause* readingClause) {
    if (readingClause->hasWherePredicate()) {
        if (!isExprReadOnly(readingClause->getWherePredicate())) {
            readOnly = false;
        }
    }
}

void StatementReadWriteAnalyzer::visitWithClause(const WithClause* withClause) {
    for (auto& expr : withClause->getProjectionBody()->getProjectionExpressions()) {
        if (!isExprReadOnly(expr.get())) {
            readOnly = false;
            return;
        }
    }
}

void StatementReadWriteAnalyzer::visitReturnClause(const ReturnClause* returnClause) {
    for (auto& expr : returnClause->getProjectionBody()->getProjectionExpressions()) {
        if (!isExprReadOnly(expr.get())) {
            readOnly = false;
            return;
        }
    }
}

bool StatementReadWriteAnalyzer::isExprReadOnly(const ParsedExpression* expr) {
    auto analyzer = ReadWriteExprAnalyzer(context);
    analyzer.visit(expr);
    return analyzer.isReadOnly();
}

} // namespace parser
} // namespace lbug
