#include "parser/graph_statement.h"
#include "parser/transformer.h"

namespace lbug {
namespace parser {

std::unique_ptr<Statement> Transformer::transformCreateGraph(
    CypherParser::KU_CreateGraphContext& ctx) {
    auto graphName = transformSchemaName(*ctx.oC_SchemaName());
    bool isAny = ctx.ANY() != nullptr;
    return std::make_unique<CreateGraph>(std::move(graphName), isAny);
}

std::unique_ptr<Statement> Transformer::transformUseGraph(CypherParser::KU_UseGraphContext& ctx) {
    auto graphName = transformSchemaName(*ctx.oC_SchemaName());
    return std::make_unique<UseGraph>(std::move(graphName));
}

} // namespace parser
} // namespace lbug
