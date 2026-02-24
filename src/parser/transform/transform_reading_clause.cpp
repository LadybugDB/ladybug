#include "common/assert.h"
#include "parser/query/reading_clause/in_query_call_clause.h"
#include "parser/query/reading_clause/load_from.h"
#include "parser/query/reading_clause/match_clause.h"
#include "parser/query/reading_clause/unwind_clause.h"
#include "parser/transformer.h"

using namespace lbug::common;

namespace lbug {
namespace parser {

std::unique_ptr<ReadingClause> Transformer::transformReadingClause(
    CypherParser::OC_ReadingClauseContext& ctx) {
    if (ctx.oC_Match()) {
        return transformMatch(*ctx.oC_Match());
    } else if (ctx.oC_Unwind()) {
        return transformUnwind(*ctx.oC_Unwind());
    } else if (ctx.iC_InQueryCall()) {
        return transformInQueryCall(*ctx.iC_InQueryCall());
    } else if (ctx.iC_LoadFrom()) {
        return transformLoadFrom(*ctx.iC_LoadFrom());
    }
    UNREACHABLE_CODE;
}

std::unique_ptr<ReadingClause> Transformer::transformMatch(CypherParser::OC_MatchContext& ctx) {
    auto matchClauseType =
        ctx.OPTIONAL() ? MatchClauseType::OPTIONAL_MATCH : MatchClauseType::MATCH;
    auto matchClause =
        std::make_unique<MatchClause>(transformPattern(*ctx.oC_Pattern()), matchClauseType);
    if (ctx.oC_Where()) {
        matchClause->setWherePredicate(transformWhere(*ctx.oC_Where()));
    }
    if (ctx.iC_Hint()) {
        matchClause->setHint(transformJoinHint(*ctx.iC_Hint()->iC_JoinNode()));
    }
    return matchClause;
}

std::shared_ptr<JoinHintNode> Transformer::transformJoinHint(
    CypherParser::IC_JoinNodeContext& ctx) {
    if (!ctx.MULTI_JOIN().empty()) {
        auto joinNode = std::make_shared<JoinHintNode>();
        joinNode->addChild(transformJoinHint(*ctx.iC_JoinNode(0)));
        for (auto& schemaNameCtx : ctx.oC_SchemaName()) {
            joinNode->addChild(std::make_shared<JoinHintNode>(transformSchemaName(*schemaNameCtx)));
        }
        return joinNode;
    }
    if (!ctx.oC_SchemaName().empty()) {
        return std::make_shared<JoinHintNode>(transformSchemaName(*ctx.oC_SchemaName(0)));
    }
    if (ctx.iC_JoinNode().size() == 1) {
        return transformJoinHint(*ctx.iC_JoinNode(0));
    }
    DASSERT(ctx.iC_JoinNode().size() == 2);
    auto joinNode = std::make_shared<JoinHintNode>();
    joinNode->addChild(transformJoinHint(*ctx.iC_JoinNode(0)));
    joinNode->addChild(transformJoinHint(*ctx.iC_JoinNode(1)));
    return joinNode;
}

std::unique_ptr<ReadingClause> Transformer::transformUnwind(CypherParser::OC_UnwindContext& ctx) {
    auto expression = transformExpression(*ctx.oC_Expression());
    auto transformedVariable = transformVariable(*ctx.oC_Variable());
    return std::make_unique<UnwindClause>(std::move(expression), std::move(transformedVariable));
}

std::vector<YieldVariable> Transformer::transformYieldVariables(
    CypherParser::OC_YieldItemsContext& ctx) {
    std::vector<YieldVariable> yieldVariables;
    std::string name;
    for (auto& yieldItem : ctx.oC_YieldItem()) {
        std::string alias;
        if (yieldItem->AS()) {
            alias = transformVariable(*yieldItem->oC_Variable(1));
        }
        name = transformVariable(*yieldItem->oC_Variable(0));
        yieldVariables.emplace_back(name, alias);
    }
    return yieldVariables;
}

std::unique_ptr<ReadingClause> Transformer::transformInQueryCall(
    CypherParser::IC_InQueryCallContext& ctx) {
    auto functionExpression =
        Transformer::transformFunctionInvocation(*ctx.oC_FunctionInvocation());
    std::vector<YieldVariable> yieldVariables;
    if (ctx.oC_YieldItems()) {
        yieldVariables = transformYieldVariables(*ctx.oC_YieldItems());
    }
    auto inQueryCall = std::make_unique<InQueryCallClause>(std::move(functionExpression),
        std::move(yieldVariables));
    if (ctx.oC_Where()) {
        inQueryCall->setWherePredicate(transformWhere(*ctx.oC_Where()));
    }
    return inQueryCall;
}

std::unique_ptr<ReadingClause> Transformer::transformLoadFrom(
    CypherParser::IC_LoadFromContext& ctx) {
    auto source = transformScanSource(*ctx.iC_ScanSource());
    auto loadFrom = std::make_unique<LoadFrom>(std::move(source));
    if (ctx.iC_ColumnDefinitions()) {
        loadFrom->setPropertyDefinitions(transformColumnDefinitions(*ctx.iC_ColumnDefinitions()));
    }
    if (ctx.iC_Options()) {
        loadFrom->setParingOptions(transformOptions(*ctx.iC_Options()));
    }
    if (ctx.oC_Where()) {
        loadFrom->setWherePredicate(transformWhere(*ctx.oC_Where()));
    }
    return loadFrom;
}

} // namespace parser
} // namespace lbug
