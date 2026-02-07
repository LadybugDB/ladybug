#include "planner/operator/logical_unwind_deduplicate.h"
#include "processor/operator/physical_operator.h"
#include "processor/operator/unwind_dedup.h"
#include "processor/plan_mapper.h"

using namespace lbug::common;
using namespace lbug::planner;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapUnwindDedup(
    const LogicalOperator* logicalOperator) {
    auto& unwindDedup = logicalOperator->constCast<LogicalUnwindDeduplicate>();
    auto outSchema = unwindDedup.getSchema();
    auto prevOperator = mapOperator(logicalOperator->getChild(0).get());
    auto keyDataPos = DataPos(outSchema->getExpressionPos(*unwindDedup.getKeyExpression()));
    auto printInfo = std::make_unique<UnwindDedupPrintInfo>();
    return std::make_unique<UnwindDedup>(keyDataPos, std::move(prevOperator), getOperatorID(),
        std::move(printInfo));
}

} // namespace processor
} // namespace lbug
