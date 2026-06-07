#include "planner/operator/simple/logical_analyze.h"
#include "processor/operator/simple/analyze.h"
#include "processor/plan_mapper.h"
#include "processor/result/factorized_table_util.h"
#include "storage/buffer_manager/memory_manager.h"

using namespace lbug::planner;

namespace lbug {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::mapAnalyze(const LogicalOperator* logicalOperator) {
    auto& logicalAnalyze = logicalOperator->constCast<LogicalAnalyze>();
    auto messageTable = FactorizedTableUtils::getSingleStringColumnFTable(
        storage::MemoryManager::Get(*clientContext));
    auto printInfo = std::make_unique<AnalyzePrintInfo>(logicalAnalyze.getTableName());
    return std::make_unique<Analyze>(logicalAnalyze.getTableName(), std::move(messageTable),
        getOperatorID(), std::move(printInfo));
}

} // namespace processor
} // namespace lbug
