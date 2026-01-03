#include "binder/binder.h"
#include "binder/bound_graph_statement.h"
#include "parser/graph_statement.h"

namespace lbug {
namespace binder {

std::unique_ptr<BoundStatement> Binder::bindCreateGraph(const parser::Statement& statement) {
    auto createGraph = statement.constCast<parser::CreateGraph>();
    return std::make_unique<BoundCreateGraph>(createGraph.getGraphName());
}

std::unique_ptr<BoundStatement> Binder::bindUseGraph(const parser::Statement& statement) {
    auto useGraph = statement.constCast<parser::UseGraph>();
    return std::make_unique<BoundUseGraph>(useGraph.getGraphName());
}

} // namespace binder
} // namespace lbug
