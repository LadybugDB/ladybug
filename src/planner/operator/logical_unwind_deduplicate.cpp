#include "planner/operator/logical_unwind_deduplicate.h"

using namespace lbug::binder;
using namespace lbug::common;

namespace lbug {
namespace planner {

void LogicalUnwindDeduplicate::computeFactorizedSchema() {
    copyChildSchema(0);
}

} // namespace planner
} // namespace lbug
