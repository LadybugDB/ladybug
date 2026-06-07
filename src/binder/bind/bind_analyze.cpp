#include "binder/binder.h"
#include "binder/bound_analyze.h"
#include "parser/analyze_statement.h"

using namespace lbug::parser;

namespace lbug {
namespace binder {

std::unique_ptr<BoundStatement> Binder::bindAnalyze(const Statement& statement) {
    auto& analyze = statement.constCast<AnalyzeStatement>();
    return std::make_unique<BoundAnalyze>(analyze.getTableName());
}

} // namespace binder
} // namespace lbug
